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
rpt_forward_members(const RptTransferMgr &forward, int root)
{
	std::set<int> members;
	std::vector<int> queue(1, root);

	members.insert(root);
	while (!queue.empty())
	{
		int			node = queue.back();
		auto		it = forward.graph.find(node);

		queue.pop_back();
		if (it == forward.graph.end())
			continue;
		/* An in-edge's destination field names the producing side. */
		for (const RptGraphEdge &in : it->second.forward_stage.in)
			if (members.insert(in.destination).second)
				queue.push_back(in.destination);
	}
	return members;
}

/* The fraction of `to` rows a filter from `from` keeps. */
static double
rpt_transfer_keep(const RptTransferMgr &forward, const RptForwardChannel &ch,
				  double from_keep, RptNdvFn ndv_fn)
{
	double		d = ndv_fn ? ndv_fn(forward.vertices[forward.by_rti.at(ch.from)],
									ch.from_key.attno) : -1;
	double		n = ndv_fn ? ndv_fn(forward.vertices[forward.by_rti.at(ch.to)],
									ch.to_key.attno) : -1;

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
	uint32		next_condition = 0;

	out->clear();
	for (int root : forward.roots)
	{
		RptForwardComponent comp;
		std::set<int> members = rpt_forward_members(forward, root);
		std::map<int, double> keep;	/* chain vertex -> fraction it keeps */
		bool		sized = true;

		comp.root = root;

		/* reducer_order is already topological: producers before consumers. */
		for (int id : forward.reducer_order)
			if (id != root && members.count(id))
				comp.chain.push_back(id);

		for (int id : comp.chain)
		{
			const RptGraphNode &node = forward.graph.at(id);
			const RptVertexInfo &source = forward.vertices[forward.by_rti.at(id)];
			auto		e = est.find(id);
			double		rows = (e != est.end() && e->second.rows >= 0)
				? e->second.rows : (double) source.cardinality;
			double		own = e != est.end() ? e->second.selectivity : 1.0;
			double		k = own;

			/* In-channels of id were made by earlier chain vertices. */
			for (const RptForwardChannel &in : comp.channels)
				if (in.to == id)
					k *= rpt_transfer_keep(forward, in, keep[in.from], ndv_fn);
			keep[id] = k;

			for (const RptGraphEdge &edge : node.forward_stage.out)
			{
				RptForwardChannel ch;

				if (edge.left_keys.empty() || edge.right_keys.empty())
					continue;
				ch.from = id;
				ch.to = edge.destination;
				ch.from_key = edge.left_keys[0];	/* out-edge: left = this side */
				ch.to_key = edge.right_keys[0];
				ch.local = edge.local;
				ch.in_process = ch.local && ch.to != root && !params.apply_all;

				/* Sized for what the reducer scan inserts: its rows after its
				 * own quals -- all of them when the quals were dropped. */
				if (!size_fn(rows * own, &ch.total_elems,
							 &ch.max_payload, &ch.planned_bytes))
				{
					comp.reason = "filter sizing declined for " +
						std::to_string(ch.from) + "->" + std::to_string(ch.to);
					sized = false;
					break;
				}
				if (!ch.in_process)
					ch.est_cost_ms = params.ms_per_mib_part * params.width *
						((double) ch.planned_bytes / (1024.0 * 1024.0));
				comp.est_cost_ms += ch.est_cost_ms;
				comp.channels.push_back(ch);
			}
			if (!sized)
				break;
		}

		for (const RptForwardChannel &ch : comp.channels)
			if (ch.to == root)
				comp.est_selectivity *= rpt_transfer_keep(forward, ch, keep[ch.from], ndv_fn);

		const RptVertexInfo &rootv = forward.vertices[forward.by_rti.at(root)];

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
			for (RptForwardChannel &ch : comp.channels)
			{
				ch.condition_id = next_condition++;
				ch.condition_key = "rpt_f_" + std::to_string(ch.from) + "_" +
					std::to_string(ch.to);
			}
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
		text << "forward component root=" << comp.root << " chain=[";
		for (size_t i = 0; i < comp.chain.size(); ++i)
			text << (i ? " " : "") << comp.chain[i];
		text << "] channels=" << comp.channels.size()
			 << " est_cost_ms=" << comp.est_cost_ms
			 << " est_sel=" << comp.est_selectivity
			 << " decision=" << comp.reason << '\n';
		for (const RptForwardChannel &ch : comp.channels)
			text << "forward channel " << ch.from_key.rtindex << '.' << ch.from_key.attno
				 << "->" << ch.to_key.rtindex << '.' << ch.to_key.attno
				 << " key=" << (ch.condition_key.empty() ? "-" : ch.condition_key)
				 << " bytes=" << ch.planned_bytes
				 << " est_ms=" << ch.est_cost_ms
				 << (ch.local ? " local" : "")
				 << (ch.in_process ? " in-process" : "") << '\n';
	}
	*out += text.str();
}
