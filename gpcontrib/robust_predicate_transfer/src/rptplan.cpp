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
 * rptplan.cpp
 *	  The asymmetric transfer plan: a Prim spanning forest
 *	  (LargestRootUpdated), forward/backward stage assignment
 *	  (CreateTransferPlanUpdated), backward-consumer eligibility, and
 *	  the deterministic text dump that feeds the regression tests.
 *
 * Ported from the RPT+ artifact (embryo-labs/dynamic-predicate-transfer),
 * src/optimizer/predicate_transfer/transfer_graph_manager.cpp and dag.cpp --
 * MIT license, Copyright 2018-2025 Stichting DuckDB Foundation (see
 * licenses/LICENSE-mit-duckdb-rpt.txt):
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * IDENTIFICATION
 *	  gpcontrib/robust_predicate_transfer/src/rptplan.cpp
 *
 *-------------------------------------------------------------------------
 */
extern "C"
{
#include "postgres.h"

#include "optimizer/walkers.h"
}

#include "rpt.h"

#include <algorithm>
#include <sstream>

/*
 * GraphNode::Add (dag.cpp:9-19): find-or-create the edge to `other` in the
 * given stage and direction.
 */
static RptGraphEdge *
rpt_node_add(RptGraphNode *node, int other, bool is_forward, bool is_in_edge)
{
	std::vector<RptGraphEdge> &edges = is_forward
		? (is_in_edge ? node->forward_stage.in : node->forward_stage.out)
		: (is_in_edge ? node->backward_stage.in : node->backward_stage.out);
	size_t		i;

	for (i = 0; i < edges.size(); i++)
	{
		if (edges[i].destination == other)
			return &edges[i];
	}
	edges.push_back(RptGraphEdge());
	edges.back().destination = other;
	return &edges.back();
}

/* GraphNode::Add with keys (dag.cpp:21-28). */
static RptGraphEdge *
rpt_node_add_keys(RptGraphNode *node, int other,
				  const std::vector<RptKey> &left_keys,
				  const std::vector<RptKey> &right_keys,
				  const std::vector<Oid> &key_types,
				  bool is_forward, bool is_in_edge)
{
	RptGraphEdge *edge = rpt_node_add(node, other, is_forward, is_in_edge);

	edge->left_keys = left_keys;
	edge->right_keys = right_keys;
	edge->key_types = key_types;
	return edge;
}

/*
 * RegisterLeader (transfer_graph_manager.hpp:50-55): the first vertex of a
 * class to enter the transfer order becomes the leader the backward
 * broadcast builds its single filter on.
 */
static void
rpt_register_leader(RptTransferMgr *mgr, int vertex_id)
{
	const RptVertexInfo &v = mgr->vertices[mgr->by_rti[vertex_id]];
	std::set<RptKey>::const_iterator kit;

	for (kit = v.join_keys.begin(); kit != v.join_keys.end(); ++kit)
	{
		RptKey		rep = RptFindKeyRoot(*kit, mgr->uf_parent);
		std::map<RptKey, RptKeyGroup>::iterator git = mgr->groups.find(rep);

		if (git != mgr->groups.end() && git->second.leader_id == -1)
		{
			git->second.leader_id = vertex_id;
			git->second.leader_key = *kit;
		}
	}
}

/*
 * Algorithm 1: choose the maximum-weight frontier vertex, break ties by
 * cardinality, then select deepest forward / shallowest backward parents.
 * Fully protected edges never participate.
 */
static bool
rpt_find_edge(const RptTransferMgr *mgr, const std::set<int> &constructed,
			  const std::set<int> &unconstructed,
			  const std::map<int, int> &forward_depth,
			  const std::map<int, int> &backward_depth,
			  int *forward_from, int *backward_from, int *to)
{
	size_t best_weight = 0;
	int64 best_rows = -1;
	bool found = false;

	/* Algorithm 1: select the frontier vertex, then each tree's parent. */
	for (int candidate : unconstructed)
	{
		for (int parent : constructed)
		{
			const auto &row = mgr->neighbor_matrix.find(parent);
			if (row == mgr->neighbor_matrix.end())
				continue;
			const auto &eit = row->second.find(candidate);
			if (eit == row->second.end() ||
				(eit->second.protect_left && eit->second.protect_right))
				continue;
			size_t weight = eit->second.clauses.size();
			int64 rows = mgr->vertices[mgr->by_rti.find(candidate)->second].cardinality;

			if (!found || weight > best_weight ||
				(weight == best_weight && rows > best_rows))
			{
				found = true;
				best_weight = weight;
				best_rows = rows;
				*to = candidate;
			}
		}
	}
	if (!found)
		return false;

	*forward_from = *backward_from = -1;
	size_t forward_weight = 0;
	size_t backward_weight = 0;
	for (int parent : constructed)
	{
		const auto &row = mgr->neighbor_matrix.find(parent);
		if (row == mgr->neighbor_matrix.end())
			continue;
		const auto &eit = row->second.find(*to);
		if (eit == row->second.end() ||
			(eit->second.protect_left && eit->second.protect_right))
			continue;
		const RptEdgeInfo &edge = eit->second;
		size_t weight = edge.clauses.size();
		bool forward_open = edge.left_v == *to ? !edge.protect_right : !edge.protect_left;
		bool backward_open = edge.left_v == parent ? !edge.protect_right : !edge.protect_left;

		/* A protected edge is not a parent candidate for that stage. Prefer
		 * its best open alternative, even if it has lower weight. */
		if (forward_open && (*forward_from == -1 || weight > forward_weight ||
			(weight == forward_weight &&
			 forward_depth.at(parent) > forward_depth.at(*forward_from))))
		{
			*forward_from = parent;
			forward_weight = weight;
		}
		if (backward_open && (*backward_from == -1 || weight > backward_weight ||
			(weight == backward_weight &&
			 backward_depth.at(parent) < backward_depth.at(*backward_from))))
		{
			*backward_from = parent;
			backward_weight = weight;
		}
	}
	return true;
}

/*
 * LargestRootUpdated (transfer_graph_manager.cpp:557-626): grow one
 * component of the spanning forest by Prim's algorithm, rooted at the
 * largest filtered-or-intermediate vertex, falling back to the largest
 * vertex.  sorted_ids lists every vertex by ascending cardinality; only
 * those still in `remaining` are candidates, and placed vertices are removed
 * from it (the artifact instead erases from its operator map and can pick a
 * stale root from its sorted list on the next component; keeping `remaining`
 * explicit avoids that).
 */
static void
rpt_largest_root_updated(RptTransferMgr *mgr, std::set<int> &remaining,
						 const std::vector<int> &sorted_ids,
						 std::vector<std::pair<int, int> > &forward_edges,
						 std::vector<std::pair<int, int> > &backward_edges,
						 int32 &next_order)
{
	std::map<int, int> forward_depth;
	std::map<int, int> backward_depth;
	std::set<int> constructed_set;
	std::set<int> unconstructed_set;
	std::vector<int>::const_reverse_iterator rit;
	std::set<int>::iterator sit;
	int			root = -1;

	/* Try to choose the largest filtered or intermediate table as the root */
	for (rit = sorted_ids.rbegin(); rit != sorted_ids.rend(); ++rit)
	{
		const RptVertexInfo &v = mgr->vertices[mgr->by_rti[*rit]];

		if (remaining.count(*rit) && (v.filtered || v.intermediate))
		{
			root = *rit;
			break;
		}
	}

	/* If we cannot find it, all tables have no filter */
	if (root == -1)
	{
		for (rit = sorted_ids.rbegin(); rit != sorted_ids.rend(); ++rit)
		{
			if (remaining.count(*rit))
			{
				root = *rit;
				break;
			}
		}
	}
	Assert(root != -1);

	/* Initialize nodes */
	for (sit = remaining.begin(); sit != remaining.end(); ++sit)
	{
		if (*sit == root)
			constructed_set.insert(*sit);
		else
			unconstructed_set.insert(*sit);

		mgr->graph[*sit].id = *sit;
		mgr->graph[*sit].cardinality_order = -1;
	}

	/* Add root */
	mgr->roots.insert(root);
	forward_depth[root] = backward_depth[root] = 0;
	mgr->graph[root].cardinality_order = next_order++;
	mgr->transfer_order.push_back(root);
	rpt_register_leader(mgr, root);
	remaining.erase(root);

	/* Build graph */
	while (!unconstructed_set.empty())
	{
		int			forward_from;
		int			backward_from;
		int			to_id;

		if (!rpt_find_edge(mgr, constructed_set, unconstructed_set,
						   forward_depth, backward_depth,
						   &forward_from, &backward_from, &to_id))
			break;

		if (forward_from != -1)
			forward_edges.push_back(std::make_pair(forward_from, to_id));
		if (backward_from != -1)
			backward_edges.push_back(std::make_pair(backward_from, to_id));
		forward_depth[to_id] = forward_from == -1 ? 0 : forward_depth[forward_from] + 1;
		backward_depth[to_id] = backward_from == -1 ? 0 : backward_depth[backward_from] + 1;
		mgr->graph[to_id].cardinality_order = next_order++;
		mgr->transfer_order.push_back(to_id);
		rpt_register_leader(mgr, to_id);

		unconstructed_set.erase(to_id);
		constructed_set.insert(to_id);
		remaining.erase(to_id);
	}

	/*
	 * Whatever is still unconstructed is disconnected from this root; it
	 * stays in `remaining` and the caller wraps it in another component.
	 */
}

/*
 * CreateTransferPlanUpdated (transfer_graph_manager.cpp:677-734): assign
 * the selected edges to the forward and backward stages.  The smaller table
 * -- the one that entered the Prim order later -- is on the left; forward
 * transfers flow from the smaller to the larger, backward transfers from
 * the larger to the smaller, with backward edges redirected at the
 * equivalence-class leader.
 */
static void
rpt_create_transfer_plan(RptTransferMgr *mgr,
						 const std::vector<std::pair<int, int> > &selected_edges,
						 bool forward)
{
	size_t		i;

	for (i = 0; i < selected_edges.size(); i++)
	{
		std::map<int, RptEdgeInfo>::iterator eit;
		RptEdgeInfo *edge;
		RptGraphNode *left_node;
		RptGraphNode *right_node;
		std::vector<RptKey> left_keys;
		std::vector<RptKey> right_keys;
		std::vector<Oid> key_types;
		bool		protect_left;
		bool		protect_right;
		size_t		c;

		eit = mgr->neighbor_matrix[selected_edges[i].first].find(selected_edges[i].second);
		if (eit == mgr->neighbor_matrix[selected_edges[i].first].end())
			continue;
		edge = &eit->second;

		left_node = &mgr->graph[edge->left_v];
		right_node = &mgr->graph[edge->right_v];
		protect_left = edge->protect_left;
		protect_right = edge->protect_right;

		for (c = 0; c < edge->clauses.size(); c++)
		{
			left_keys.push_back(edge->clauses[c].first);
			right_keys.push_back(edge->clauses[c].second);
			key_types.push_back(edge->key_type);
		}

		/* smaller table is in the left */
		if (left_node->cardinality_order < right_node->cardinality_order)
		{
			std::swap(left_node, right_node);
			std::swap(left_keys, right_keys);
			std::swap(protect_left, protect_right);
		}

		/* forward: from the smaller to the larger */
		if (forward && !protect_right && !mgr->roots.count(left_node->id) &&
			(mgr->vertices[mgr->by_rti[left_node->id]].filtered ||
			 mgr->bridges.count(left_node->id)))
		{
			rpt_node_add_keys(left_node, right_node->id, left_keys, right_keys,
							  key_types, true, false);
			rpt_node_add_keys(right_node, left_node->id, left_keys, right_keys,
							  key_types, true, true);
		}

		/* backward: from the larger to the smaller */
		if (!forward && !protect_left)
		{
			/*
			 * Group-leader redirection (transfer_graph_manager.cpp:719-732):
			 * the backward edge is built on the class leader, which produces
			 * the single broadcast filter for the whole class.  The
			 * artifact's edges are single-key; ours bucket the clauses by
			 * destination so a multi-key edge can fan out to one leader per
			 * clause class.
			 */
			std::map<int, RptGraphEdge> by_dest;
			std::map<int, RptGraphEdge>::iterator dit;

			for (c = 0; c < left_keys.size(); c++)
			{
				RptKey		rep = RptFindKeyRoot(right_keys[c], mgr->uf_parent);
				std::map<RptKey, RptKeyGroup>::iterator git = mgr->groups.find(rep);
				int			dest;
				RptKey		dest_key;
				RptGraphEdge *bucket;

				if (git != mgr->groups.end() && git->second.leader_id != -1)
				{
					dest = git->second.leader_id;
					dest_key = git->second.leader_key;
				}
				else
				{
					dest = right_node->id;
					dest_key = right_keys[c];
				}

				bucket = &by_dest[dest];
				bucket->destination = dest;
				bucket->left_keys.push_back(left_keys[c]);
				bucket->right_keys.push_back(dest_key);
				bucket->key_types.push_back(key_types[c]);
			}

			for (dit = by_dest.begin(); dit != by_dest.end(); ++dit)
			{
				const RptGraphEdge &bucket = dit->second;

				rpt_node_add_keys(left_node, bucket.destination,
								  bucket.left_keys, bucket.right_keys,
								  bucket.key_types, false, true);
				rpt_node_add_keys(&mgr->graph[bucket.destination], left_node->id,
								  bucket.left_keys, bucket.right_keys,
								  bucket.key_types, false, false);
			}
		}
	}
}

/* Locality is a property of the base scans' distribution policies, including
 * for implied and bypass edges. A Motion elsewhere in the main plan does not
 * change where the reducer reads a key. Parallel workers still need a
 * segment-local fold (or a non-parallel reducer); see docs/design.md. */
static bool
rpt_local_hop(const RptTransferMgr *mgr, int source, const RptGraphEdge &hop)
{
	const auto &src = mgr->vertices[mgr->by_rti.at(source)];
	const auto &dst = mgr->vertices[mgr->by_rti.at(hop.destination)];

	/* A replicated destination needs the complete key set on every segment. */
	if (dst.replicated || src.numsegments <= 0 || dst.numsegments <= 0)
		return false;
	if (src.replicated)
		return src.numsegments >= dst.numsegments;
	if (src.numsegments != dst.numsegments || src.distkeys.empty() ||
		src.distkeys.size() != dst.distkeys.size() ||
		src.distopclasses.size() != src.distkeys.size() ||
		src.distopclasses != dst.distopclasses)
		return false;
	for (size_t col = 0; col < src.distkeys.size(); ++col)
	{
		bool matched = false;
		for (size_t key = 0; key < hop.left_keys.size(); ++key)
		{
			const RptKey &a = hop.left_keys[key];
			const RptKey &b = hop.right_keys[key];
			if ((a == RptKey{source, src.distkeys[col]} &&
				 b == RptKey{hop.destination, dst.distkeys[col]}) ||
				(b == RptKey{source, src.distkeys[col]} &&
				 a == RptKey{hop.destination, dst.distkeys[col]}))
				matched = true;
		}
		if (!matched)
			return false;
	}
	return true;
}

/* Persist the decision on both copies of every stage edge. In/out edges
 * carry the same key pairs, but destination means producer for an in-edge. */
static void
rpt_classify_hops(RptTransferMgr *mgr)
{
	for (auto &node : mgr->graph)
	{
		for (bool forward : {true, false})
		{
			auto &outs = forward ? node.second.forward_stage.out : node.second.backward_stage.out;
			for (auto &edge : outs)
			{
				edge.local = rpt_local_hop(mgr, node.first, edge);
				auto &dest = mgr->graph.at(edge.destination);
				auto &ins = forward ? dest.forward_stage.in : dest.backward_stage.in;
				for (auto &in : ins)
					if (in.destination == node.first)
						in.local = edge.local;
			}
		}
	}
}

/*
 * Build the transfer plan for the join graph: the Prim forest, the
 * forward/backward stage assignment, and the consumer-eligibility check.
 */
void
RptBuildTransferPlan(PlannedStmt *stmt, RptTransferMgr *mgr)
{
	std::vector<std::pair<int, int> > forward_edges;
	std::vector<std::pair<int, int> > backward_edges;
	std::vector<int> sorted_ids;
	std::set<int> remaining;
	int32		next_order = 0;
	size_t		i;

	/* vertices by ascending cardinality, ties by id, for a stable plan */
	for (i = 0; i < mgr->vertices.size(); i++)
	{
		sorted_ids.push_back(mgr->vertices[i].id);
		remaining.insert(mgr->vertices[i].id);
	}
	std::sort(sorted_ids.begin(), sorted_ids.end(),
			  [&mgr] (int a, int b)
	{
		const RptVertexInfo &va = mgr->vertices[mgr->by_rti[a]];
		const RptVertexInfo &vb = mgr->vertices[mgr->by_rti[b]];

		if (va.cardinality != vb.cardinality)
			return va.cardinality < vb.cardinality;
		return a < b;
	});

	/*
	 * The while-loop forest wrapping (transfer_graph_manager.cpp:677-683):
	 * repeat the Prim pass until every vertex is placed, giving a spanning
	 * forest -- one tree per connected component of the join graph.
	 */
	while (!remaining.empty())
		rpt_largest_root_updated(mgr, remaining, sorted_ids, forward_edges, backward_edges,
								 next_order);

	for (const auto &group : mgr->groups)
		if (group.second.leader_id != -1 && !mgr->roots.count(group.second.leader_id))
			mgr->bridges.insert(group.second.leader_id);

	rpt_create_transfer_plan(mgr, forward_edges, true);
	rpt_create_transfer_plan(mgr, backward_edges, false);

	/* Transfers run in reverse insertion order. Cap actual dependency hops,
	 * keeping every inbound edge mirrored in its producer's out list. */
	std::map<int, int> depth;
	for (auto it = mgr->transfer_order.rbegin(); it != mgr->transfer_order.rend(); ++it)
	{
		auto &outs = mgr->graph[*it].forward_stage.out;
		for (auto edge = outs.begin(); edge != outs.end();)
		{
			int dest = edge->destination;
			if (depth[*it] >= rpt_max_chain_depth)
			{
				auto &ins = mgr->graph[dest].forward_stage.in;
				ins.erase(std::remove_if(ins.begin(), ins.end(),
					[&](const RptGraphEdge &in) { return in.destination == *it; }), ins.end());
				edge = outs.erase(edge);
			}
			else
			{
				depth[dest] = std::max(depth[dest], depth[*it] + 1);
				++edge;
			}
		}
		if (!outs.empty())
			mgr->reducer_order.push_back(*it);
	}

	rpt_classify_hops(mgr);
	RptCheckBackwardConsumers(stmt, mgr);
}

/*
 * Dump support.  Everything below iterates std::map / sorted copies only,
 * so the output is deterministic and can feed expected-output regression
 * tests.
 */

static bool
rpt_edge_less(const RptGraphEdge &a, const RptGraphEdge &b)
{
	if (a.destination != b.destination)
		return a.destination < b.destination;
	if (a.left_keys != b.left_keys)
		return a.left_keys < b.left_keys;
	return a.right_keys < b.right_keys;
}

static void
rpt_dump_key_pairs(std::ostream &out, const RptGraphEdge &edge)
{
	for (size_t i = 0; i < edge.left_keys.size(); i++)
	{
		if (i > 0)
			out << ',';
		out << edge.left_keys[i].rtindex << '.' << edge.left_keys[i].attno
			<< "->" << edge.right_keys[i].rtindex << '.' << edge.right_keys[i].attno;
	}
}

/* A stage's sorted build/apply entries, preserving the diagnostic format. */
static void
rpt_dump_stage(std::ostream &out, int vertex_id, const char *stage,
			   const std::vector<RptGraphEdge> &in_edges,
			   const std::vector<RptGraphEdge> &out_edges)
{
	std::vector<RptGraphEdge> outs(out_edges);
	std::vector<RptGraphEdge> ins(in_edges);
	bool any = false;

	std::sort(outs.begin(), outs.end(), rpt_edge_less);
	std::sort(ins.begin(), ins.end(), rpt_edge_less);
	out << "vertex " << vertex_id << ' ' << stage << ':';
	for (const auto &edge : outs)
	{
		out << (any ? "; " : " ") << "build for " << edge.destination << " on (";
		rpt_dump_key_pairs(out, edge);
		out << ')';
		any = true;
	}
	for (const auto &edge : ins)
	{
		out << (any ? "; " : " ") << "apply from " << edge.destination << " on (";
		rpt_dump_key_pairs(out, edge);
		out << ')';
		any = true;
	}
	if (!any)
		out << " none";
	out << '\n';
}

/* Build in C++ storage, then cross the PostgreSQL allocation boundary once.
 * RptPgCall ensures a failed append unwinds the stream and string owners. */
void
RptDumpTransferPlan(const RptTransferMgr *mgr, StringInfo output)
{
	std::ostringstream out;
	std::vector<int> ids;

	out.exceptions(std::ios::badbit | std::ios::failbit);
	out.imbue(std::locale::classic());
	for (const auto &vertex : mgr->vertices)
		ids.push_back(vertex.id);
	std::sort(ids.begin(), ids.end());

	out << "transfer order:";
	for (int id : mgr->transfer_order)
		out << ' ' << id;
	out << "\nforward depth limit: " << rpt_max_chain_depth << "\nreducer order:";
	for (int id : mgr->reducer_order)
		out << ' ' << id;
	out << "\nbridge tables:";
	for (int id : mgr->bridges)
		out << ' ' << id;
	out << '\n';

	for (int id : ids)
	{
		const auto &v = mgr->vertices[mgr->by_rti.at(id)];

		out << "vertex " << v.id << ' ' << v.relname << " rows=" << v.cardinality
			<< ' ' << (v.filtered ? "filtered" : v.intermediate ? "intermediate" : "unfiltered")
			<< '\n';
	}
	for (const auto &group : mgr->groups)
	{
		bool first = true;

		out << "class " << group.first.rtindex << '.' << group.first.attno << ": members=";
		for (int member : group.second.members)
		{
			if (!first)
				out << ',';
			out << member;
			first = false;
		}
		out << " leader=" << group.second.leader_id << '\n';
	}
	for (int id : ids)
	{
		const auto &node = mgr->graph.find(id);

		if (node == mgr->graph.end())
		{
			out << "vertex " << id << " forward: none\n"
				<< "vertex " << id << " backward: none\n";
			continue;
		}
		rpt_dump_stage(out, id, "forward", node->second.forward_stage.in,
					   node->second.forward_stage.out);
		rpt_dump_stage(out, id, "backward", node->second.backward_stage.in,
					   node->second.backward_stage.out);
	}
	for (const auto &node : mgr->graph)
	{
		for (const auto &edge : node.second.forward_stage.out)
			out << "forward hop " << node.first << "->" << edge.destination
				<< " merge=" << (edge.local ? "local" : "global") << '\n';
		for (const auto &edge : node.second.backward_stage.out)
			out << "backward hop " << node.first << "->" << edge.destination
				<< " merge=" << (edge.local ? "local" : "global") << '\n';
	}
	int global_edges = 0, rule9 = 0, strict = 0, eligible = 0, root_classes = 0;
	int unknown_estimates = 0;
	double rows_removed = 0;
	for (const auto &group : mgr->groups)
		if (mgr->roots.count(group.second.leader_id))
			++root_classes;
	for (const auto &decision : mgr->backward_consumers)
	{
		global_edges += !decision.local;
		rule9 += decision.rule9;
		strict += decision.strict_eligible;
		eligible += decision.eligible;
		if (decision.strict_eligible)
		{
			if (decision.estimated_rows_removed < 0)
				++unknown_estimates;
			else
				rows_removed += decision.estimated_rows_removed;
		}
		out << "backward consumer " << decision.consumer << " producer " << decision.producer
			<< " class " << decision.class_key.rtindex << '.' << decision.class_key.attno
			<< " merge=" << (decision.local ? "local" : "global")
			<< " rule9=" << (decision.rule9 ? "yes" : "no")
			<< " strict=" << (decision.strict_eligible ? "yes" : "no")
			<< " eligible=" << (decision.eligible ? "yes" : "no")
			<< " defer_first=" << (decision.defer_first ? "yes" : "no")
			<< " producer_slice=" << decision.producer_slice
			<< " consumer_slice=" << decision.consumer_slice
			<< " producer_attno=" << decision.producer_attno
			<< " consumer_attno=" << decision.consumer_attno
			<< " rows_removed=";
		if (decision.estimated_rows_removed < 0)
			out << "unknown";
		else
			out << decision.estimated_rows_removed;
		out << " reason=" << decision.reason << '\n';
	}
	out << "survey classes=" << mgr->groups.size() << " root_classes=" << root_classes
		<< " global_edges=" << global_edges << " rule9=" << rule9 << " strict=" << strict
		<< " eligible=" << eligible << " known_rows_removed=" << rows_removed
		<< " unknown_estimates=" << unknown_estimates << '\n';
	for (int id : ids)
	{
		const auto &consumer = mgr->consumer_ok.find(id);
		bool ok = consumer != mgr->consumer_ok.end() && consumer->second;
		out << "consumer_ok " << id << ' ' << (ok ? "yes" : "no");
		if (!ok)
		{
			std::string reason = "no backward edge";
			for (const auto &decision : mgr->backward_consumers)
				if (decision.consumer == id && !decision.eligible)
				{
					reason = decision.reason;
					if (!decision.local)
						break;
				}
			out << ": " << reason;
		}
		out << '\n';
	}

	std::string text = out.str();
	RptPgCall([&]() { appendStringInfoString(output, text.c_str()); });
}
