/*-------------------------------------------------------------------------
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * rptforwardplan.cpp
 *	  Forward reducer selection: which forward chains to build, their channels,
 *	  and the cost gate.  No PostgreSQL calls, so it is unit-tested directly.
 *
 * The cost model was measured on one host with three primaries: every
 * global hop pays about c milliseconds per MiB of bitset per producer, because
 * the coordinator receives and folds the parts one at a time.  Hops of one
 * chain run one after another inside the reducer, so a component's cost is
 * the sum over its channels.  A local hop consumed only inside the reducer is
 * handed over in-process (rptlocal.c) and costs no exchange.
 *
 * The benefit side is the paper's selectivity test: inject only when the
 * chain is expected to keep less than lambda of the root's rows.  A vertex
 * keeps the fraction its own reducer quals keep, times what its inbound
 * filters keep; a filter from u keeps s(u) * min(1, ndv(u.key) / ndv(v.key))
 * of v, assuming the smaller key domain is contained in the larger one.
 *
 * IDENTIFICATION
 *	  gpcontrib/robust_predicate_transfer/src/rptforwardplan.cpp
 *
 *-------------------------------------------------------------------------
 */
#include "rpt.h"

extern "C"
{
#include "postgres.h"
}


#include <sstream>

/* Every vertex whose filters can reach `root` through forward edges. */
static std::set<int>
rpt_forward_member_rtindexes(const RptTransferMgr &forward, int root_rtindex)
{
	std::set<int> member_rtindexes;
	std::vector<int> pending_rtindexes(1, root_rtindex);

	member_rtindexes.insert(root_rtindex);
	while (!pending_rtindexes.empty())
	{
		int			rtindex = pending_rtindexes.back();
		auto		it = forward.graph.find(rtindex);

		pending_rtindexes.pop_back();
		if (it == forward.graph.end())
			continue;
		/* An in-edge's other_rtindex names the producing side. */
		for (const RptGraphEdge &in : it->second.forward_stage.in)
			if (member_rtindexes.insert(in.other_rtindex).second)
				pending_rtindexes.push_back(in.other_rtindex);
	}
	return member_rtindexes;
}

/* The fraction of `to` rows a filter from `from` keeps. */
static double
rpt_transfer_keep(const RptTransferMgr &forward, const RptForwardChannel &ch,
				  double from_keep, RptNdvFn ndv_fn)
{
	double		d = ndv_fn ? ndv_fn(forward.vertices[forward.vertex_index_by_rtindex.at(ch.source_rtindex)],
									ch.source_key.attno) : -1;
	double		n = ndv_fn ? ndv_fn(forward.vertices[forward.vertex_index_by_rtindex.at(ch.target_rtindex)],
									ch.target_key.attno) : -1;

	if (d > 0 && n > 0 && d < n)
		return from_keep * d / n;
	return from_keep;
}

void
RptSelectForward(const RptTransferMgr &forward, const RptForwardParams &params,
				 const std::map<int, RptForwardVertexEst> &est,
				 RptFilterSizeFn size_fn, RptNdvFn ndv_fn,
				 std::vector<RptForwardComponent> *out)
{

	out->clear();
	for (int root_rtindex : forward.root_rtindexes)
	{
		RptForwardComponent comp;
		std::set<int> member_rtindexes = rpt_forward_member_rtindexes(forward, root_rtindex);
		std::map<int, double> keep;	/* chain vertex -> fraction it keeps */
		bool		sized = true;

		comp.root_rtindex = root_rtindex;

		/* reducer_order is already topological: producers before consumers. */
		for (int rtindex : forward.reducer_order_rtindexes)
			if (rtindex != root_rtindex && member_rtindexes.count(rtindex))
				comp.chain_rtindexes.push_back(rtindex);

		for (int rtindex : comp.chain_rtindexes)
		{
			const RptGraphNode &node = forward.graph.at(rtindex);
			const RptVertexInfo &source = forward.vertices[forward.vertex_index_by_rtindex.at(rtindex)];
			auto		e = est.find(rtindex);
			double		rows = (e != est.end() && e->second.rows >= 0)
				? e->second.rows : (double) source.cardinality;
			double		own = e != est.end() ? e->second.selectivity : 1.0;
			double		k = own;

			/* In-channels of this rtindex were made by earlier chain vertices. */
			for (const RptForwardChannel &in : comp.channels)
				if (in.target_rtindex == rtindex)
					k *= rpt_transfer_keep(forward, in, keep[in.source_rtindex], ndv_fn);
			keep[rtindex] = k;

			for (const RptGraphEdge &edge : node.forward_stage.out)
			{
				RptForwardChannel ch;

				if (edge.left_keys.empty() || edge.right_keys.empty())
					continue;
				ch.source_rtindex = rtindex;
				ch.target_rtindex = edge.other_rtindex;
				ch.source_key = edge.left_keys[0];	/* out-edge: left = this side */
				ch.target_key = edge.right_keys[0];
				ch.local = edge.local;
				ch.in_process = ch.local && ch.target_rtindex != root_rtindex && !params.apply_all;

				/* Sized for what the reducer scan inserts: its rows after its
				 * own quals -- all of them when the quals were dropped. */
				if (!size_fn(rows * own, &ch.total_elems,
							 &ch.max_payload, &ch.planned_bytes))
				{
					comp.reason = "filter sizing declined for " +
						std::to_string(ch.source_rtindex) + "->" + std::to_string(ch.target_rtindex);
					sized = false;
					break;
				}
				if (!ch.in_process)
					{
					/* One reducer consumer, plus a main-plan consumer for
					 * root edges or forward_apply=all. */
					auto processes = params.main_consumer_processes.find(ch.target_rtindex);
					int main_width = processes == params.main_consumer_processes.end()
						? params.width : processes->second;
					int deliveries = (ch.target_rtindex != root_rtindex ? params.width : 0) +
						(ch.target_rtindex == root_rtindex || params.apply_all ? main_width : 0);
					ch.est_cost_ms =
						(params.ms_per_mib_part * params.width +
						 params.delivery_ms_per_mib * deliveries) *
						((double) ch.planned_bytes / (1024.0 * 1024.0));
				}
				comp.est_cost_ms += ch.est_cost_ms;
				comp.channels.push_back(ch);
			}
			if (!sized)
				break;
		}

		for (const RptForwardChannel &ch : comp.channels)
			if (ch.target_rtindex == root_rtindex)
				comp.est_selectivity *= rpt_transfer_keep(forward, ch, keep[ch.source_rtindex], ndv_fn);

		const RptVertexInfo &rootv = forward.vertices[forward.vertex_index_by_rtindex.at(root_rtindex)];

		if (!sized)
			;					/* reason already set */
		else if (comp.channels.empty())
			comp.reason = "no forward edges";
		else if ((double) rootv.cardinality < params.min_root_rows)
			comp.reason = "root rows below rpt.forward_min_root_rows";
		else if (params.max_selectivity < 1 &&
				 comp.est_selectivity >= params.max_selectivity)
			comp.reason = "estimated root selectivity not below rpt.forward_max_selectivity";
		else if (params.max_exchange_ms >= 0 &&
				 comp.est_cost_ms > params.max_exchange_ms)
			comp.reason = "estimated exchange cost above rpt.forward_max_exchange_ms";
		else
		{
			comp.accepted = true;
			comp.reason = "inject";
		}
		out->push_back(comp);
	}
}

void
RptDumpForward(const std::vector<RptForwardComponent> &components, std::string *out)
{
	std::ostringstream text;

	text << std::fixed << std::setprecision(3);
	for (const RptForwardComponent &comp : components)
	{
		text << "forward component root=" << comp.root_rtindex << " chain=[";
		for (size_t i = 0; i < comp.chain_rtindexes.size(); ++i)
			text << (i ? " " : "") << comp.chain_rtindexes[i];
		text << "] channels=" << comp.channels.size()
			 << " est_cost_ms=" << comp.est_cost_ms
			 << " est_sel=" << comp.est_selectivity
			 << " decision=" << comp.reason << '\n';
		for (const RptForwardChannel &ch : comp.channels)
			text << "forward channel " << ch.source_key.rtindex << '.' << ch.source_key.attno
				 << "->" << ch.target_key.rtindex << '.' << ch.target_key.attno
				 << " key=" << (ch.condition_key.empty() ? "-" : ch.condition_key)
				 << " bytes=" << ch.planned_bytes
				 << " est_ms=" << ch.est_cost_ms
				 << (ch.local ? " local" : "")
				 << (ch.in_process ? " in-process" : "") << '\n';
	}
	*out += text.str();
}
