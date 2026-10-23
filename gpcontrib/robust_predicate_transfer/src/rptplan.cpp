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

/*
 * Find the representative key of a key's equivalence class, with path
 * compression.  A file-local copy of rptgraph.cpp's rpt_find_key_root
 * (FindBindingRoot, transfer_graph_manager.cpp:12-23).
 */
static RptKey
rpt_find_key_root(const RptKey &key, std::map<RptKey, RptKey> &uf_parent)
{
	std::map<RptKey, RptKey>::iterator it = uf_parent.find(key);
	RptKey		root;

	if (it == uf_parent.end())
		return key;

	root = it->second;
	if (!(root == key))
	{
		root = rpt_find_key_root(root, uf_parent);
		uf_parent[key] = root;	/* path compression */
	}
	return root;
}

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
		RptKey		rep = rpt_find_key_root(*kit, mgr->uf_parent);
		std::map<RptKey, RptKeyGroup>::iterator git = mgr->groups.find(rep);

		if (git != mgr->groups.end() && git->second.leader_id == -1)
		{
			git->second.leader_id = vertex_id;
			git->second.leader_key = *kit;
		}
	}
}

/*
 * FindEdge (transfer_graph_manager.cpp:736-759): the frontier edge to the
 * unconstructed vertex with the largest cardinality, preferring edges that
 * are unprotected both ways.
 */
static bool
rpt_find_edge(const RptTransferMgr *mgr, const std::set<int> &constructed_set,
			  const std::set<int> &unconstructed_set, int *from_id, int *to_id)
{
	int64		max_cardinality = 0;
	bool		is_indirected = false;
	bool		found = false;
	std::set<int>::const_iterator ii;

	for (ii = unconstructed_set.begin(); ii != unconstructed_set.end(); ++ii)
	{
		std::set<int>::const_iterator ji;

		for (ji = constructed_set.begin(); ji != constructed_set.end(); ++ji)
		{
			std::map<int, std::map<int, RptEdgeInfo> >::const_iterator row;
			std::map<int, RptEdgeInfo>::const_iterator eit;
			const RptEdgeInfo *edge;
			int64		cardinality;

			row = mgr->neighbor_matrix.find(*ji);
			if (row == mgr->neighbor_matrix.end())
				continue;
			eit = row->second.find(*ii);
			if (eit == row->second.end())
				continue;
			edge = &eit->second;

			cardinality = mgr->vertices[mgr->by_rti.find(*ii)->second].cardinality;
			if (cardinality > max_cardinality ||
				(!is_indirected && !edge->protect_left && !edge->protect_right))
			{
				max_cardinality = cardinality;
				*from_id = *ji;
				*to_id = *ii;
				found = true;
				is_indirected = !edge->protect_left && !edge->protect_right;
			}
		}
	}
	return found;
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
						 std::vector<std::pair<int, int> > &selected_edges,
						 int32 &next_order)
{
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
	mgr->graph[root].cardinality_order = next_order++;
	mgr->transfer_order.push_back(root);
	rpt_register_leader(mgr, root);
	remaining.erase(root);

	/* Build graph */
	while (!unconstructed_set.empty())
	{
		int			from_id;
		int			to_id;

		if (!rpt_find_edge(mgr, constructed_set, unconstructed_set,
						   &from_id, &to_id))
			break;

		selected_edges.push_back(std::make_pair(from_id, to_id));
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
						 const std::vector<std::pair<int, int> > &selected_edges)
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
		if (!protect_right)
		{
			rpt_node_add_keys(left_node, right_node->id, left_keys, right_keys,
							  key_types, true, false);
			rpt_node_add_keys(right_node, left_node->id, left_keys, right_keys,
							  key_types, true, true);
		}

		/* backward: from the larger to the smaller */
		if (!protect_left)
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
				RptKey		rep = rpt_find_key_root(right_keys[c], mgr->uf_parent);
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

/*
 * Backward-consumer eligibility: Anser's deadlock rule, with no counterpart
 * in the single-node artifact.  A live backward consumer blocks until its
 * producer's filter arrives, so it may only wait on a producer the plan
 * order guarantees runs first: the producer's scan on the build (inner)
 * side of a hash join and the consumer's scan on the probe (outer) side of
 * the same join.  consumer_ok[v] is true exactly when that holds for one of
 * v's backward-stage in-edges; everything else fails open (no backward
 * filter for that table).
 *
 * The contexts are prefixed with plan_tree_base_prefix because
 * plan_tree_walker resolves SubPlan nodes through it, even when not
 * recursing into subplans.
 */
typedef struct RptScanCollectContext
{
	plan_tree_base_prefix base; /* must be first */
	const std::map<const Plan *, int> *scan_vertex;
	std::set<int> ids;
} RptScanCollectContext;

typedef struct RptConsumerCheckContext
{
	plan_tree_base_prefix base; /* must be first */
	PlannedStmt *stmt;
	const std::map<const Plan *, int> *scan_vertex;
	const std::map<int, std::set<int> > *producers;
	std::map<int, bool> *consumer_ok;
} RptConsumerCheckContext;

/* Collect the ids of every vertex whose scan appears under this subtree. */
static bool
rpt_collect_scan_ids_walker(Node *node, void *context)
{
	RptScanCollectContext *ctx = (RptScanCollectContext *) context;
	std::map<const Plan *, int>::const_iterator it;

	if (node == NULL)
		return false;

	/* vertex scans match by pointer; non-Plan nodes simply never hit */
	it = ctx->scan_vertex->find((const Plan *) node);
	if (it != ctx->scan_vertex->end())
		ctx->ids.insert(it->second);

	return plan_tree_walker(node, rpt_collect_scan_ids_walker, context, false);
}

static bool
rpt_consumer_check_walker(Node *node, void *context)
{
	RptConsumerCheckContext *ctx = (RptConsumerCheckContext *) context;

	if (node == NULL)
		return false;

	if (IsA(node, HashJoin))
	{
		HashJoin   *hj = (HashJoin *) node;
		RptScanCollectContext inner_ctx;
		RptScanCollectContext outer_ctx;
		std::map<int, std::set<int> >::const_iterator pit;

		exec_init_plan_tree_base(&inner_ctx.base, ctx->stmt);
		inner_ctx.scan_vertex = ctx->scan_vertex;
		exec_init_plan_tree_base(&outer_ctx.base, ctx->stmt);
		outer_ctx.scan_vertex = ctx->scan_vertex;

		/* build side is the inner subtree, probe side the outer */
		rpt_collect_scan_ids_walker((Node *) innerPlan(hj), &inner_ctx);
		rpt_collect_scan_ids_walker((Node *) outerPlan(hj), &outer_ctx);

		for (pit = ctx->producers->begin(); pit != ctx->producers->end(); ++pit)
		{
			int			consumer = pit->first;
			std::set<int>::const_iterator prod;

			if ((*ctx->consumer_ok)[consumer] ||
				!outer_ctx.ids.count(consumer))
				continue;
			for (prod = pit->second.begin(); prod != pit->second.end(); ++prod)
			{
				if (inner_ctx.ids.count(*prod))
				{
					(*ctx->consumer_ok)[consumer] = true;
					break;
				}
			}
		}
	}

	return plan_tree_walker(node, rpt_consumer_check_walker, context, false);
}

static void
rpt_check_consumers(PlannedStmt *stmt, RptTransferMgr *mgr)
{
	std::map<const Plan *, int> scan_vertex;
	std::map<int, std::set<int> > producers;
	RptConsumerCheckContext ctx;
	size_t		i;
	std::map<int, RptGraphNode>::const_iterator nit;

	for (i = 0; i < mgr->vertices.size(); i++)
	{
		mgr->consumer_ok[mgr->vertices[i].id] = false;
		scan_vertex[mgr->vertices[i].scan] = mgr->vertices[i].id;
	}

	/*
	 * The producer of a backward in-edge is its destination: the class
	 * leader for a redirected edge, the larger table otherwise.
	 */
	for (nit = mgr->graph.begin(); nit != mgr->graph.end(); ++nit)
	{
		const std::vector<RptGraphEdge> &ins = nit->second.backward_stage.in;

		for (i = 0; i < ins.size(); i++)
			producers[nit->first].insert(ins[i].destination);
	}

	if (producers.empty() || stmt == NULL || stmt->planTree == NULL)
		return;

	exec_init_plan_tree_base(&ctx.base, stmt);
	ctx.stmt = stmt;
	ctx.scan_vertex = &scan_vertex;
	ctx.producers = &producers;
	ctx.consumer_ok = &mgr->consumer_ok;

	rpt_consumer_check_walker((Node *) stmt->planTree, &ctx);
}

/*
 * Build the transfer plan for the join graph: the Prim forest, the
 * forward/backward stage assignment, and the consumer-eligibility check.
 */
void
RptBuildTransferPlan(PlannedStmt *stmt, RptTransferMgr *mgr)
{
	std::vector<std::pair<int, int> > selected_edges;
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
		rpt_largest_root_updated(mgr, remaining, sorted_ids, selected_edges,
								 next_order);

	rpt_create_transfer_plan(mgr, selected_edges);

	rpt_check_consumers(stmt, mgr);
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
rpt_dump_key_pairs(StringInfo out, const RptGraphEdge &edge)
{
	size_t		i;

	for (i = 0; i < edge.left_keys.size(); i++)
	{
		appendStringInfo(out, "%s%d.%d->%d.%d", i > 0 ? "," : "",
						 edge.left_keys[i].rtindex, (int) edge.left_keys[i].attno,
						 edge.right_keys[i].rtindex, (int) edge.right_keys[i].attno);
	}
}

/*
 * One stage of one vertex: " build for <dest> on (<keys>)" for each
 * out-edge, " apply from <dest> on (<keys>)" for each in-edge, entries
 * separated by "; ", edges sorted by (destination, keys).
 */
static void
rpt_dump_stage(StringInfo out, int vertex_id, const char *stage,
			   const std::vector<RptGraphEdge> &in_edges,
			   const std::vector<RptGraphEdge> &out_edges)
{
	std::vector<RptGraphEdge> outs(out_edges);
	std::vector<RptGraphEdge> ins(in_edges);
	bool		any = false;
	size_t		i;

	std::sort(outs.begin(), outs.end(), rpt_edge_less);
	std::sort(ins.begin(), ins.end(), rpt_edge_less);

	appendStringInfo(out, "vertex %d %s:", vertex_id, stage);
	for (i = 0; i < outs.size(); i++)
	{
		appendStringInfo(out, "%sbuild for %d on (", any ? "; " : " ",
						 outs[i].destination);
		rpt_dump_key_pairs(out, outs[i]);
		appendStringInfoChar(out, ')');
		any = true;
	}
	for (i = 0; i < ins.size(); i++)
	{
		appendStringInfo(out, "%sapply from %d on (", any ? "; " : " ",
						 ins[i].destination);
		rpt_dump_key_pairs(out, ins[i]);
		appendStringInfoChar(out, ')');
		any = true;
	}
	if (!any)
		appendStringInfoString(out, " none");
	appendStringInfoChar(out, '\n');
}

/*
 * Dump the whole analysis as deterministic text: Prim transfer order,
 * vertices by ascending id, equivalence classes by representative key, the
 * forward/backward stages per vertex, and the consumer-eligibility verdicts.
 */
void
RptDumpTransferPlan(const RptTransferMgr *mgr, StringInfo out)
{
	std::vector<int> ids;
	size_t		i;
	std::map<RptKey, RptKeyGroup>::const_iterator git;

	for (i = 0; i < mgr->vertices.size(); i++)
		ids.push_back(mgr->vertices[i].id);
	std::sort(ids.begin(), ids.end());

	/* Prim insertion order */
	appendStringInfoString(out, "transfer order:");
	for (i = 0; i < mgr->transfer_order.size(); i++)
		appendStringInfo(out, " %d", mgr->transfer_order[i]);
	appendStringInfoChar(out, '\n');

	/* vertices */
	for (i = 0; i < ids.size(); i++)
	{
		const RptVertexInfo &v = mgr->vertices[mgr->by_rti.find(ids[i])->second];

		appendStringInfo(out, "vertex %d %s rows=" INT64_FORMAT " %s\n",
						 v.id, v.relname.c_str(), v.cardinality,
						 v.filtered ? "filtered" :
						 v.intermediate ? "intermediate" : "unfiltered");
	}

	/* equivalence classes */
	for (git = mgr->groups.begin(); git != mgr->groups.end(); ++git)
	{
		const RptKeyGroup &g = git->second;
		std::set<int>::const_iterator mit;
		bool		first = true;

		appendStringInfo(out, "class %d.%d: members=",
						 git->first.rtindex, (int) git->first.attno);
		for (mit = g.members.begin(); mit != g.members.end(); ++mit)
		{
			appendStringInfo(out, "%s%d", first ? "" : ",", *mit);
			first = false;
		}
		appendStringInfo(out, " leader=%d\n", g.leader_id);
	}

	/* forward and backward stages per vertex */
	for (i = 0; i < ids.size(); i++)
	{
		std::map<int, RptGraphNode>::const_iterator nit = mgr->graph.find(ids[i]);

		if (nit == mgr->graph.end())
		{
			appendStringInfo(out, "vertex %d forward: none\n", ids[i]);
			appendStringInfo(out, "vertex %d backward: none\n", ids[i]);
			continue;
		}
		rpt_dump_stage(out, ids[i], "forward",
					   nit->second.forward_stage.in,
					   nit->second.forward_stage.out);
		rpt_dump_stage(out, ids[i], "backward",
					   nit->second.backward_stage.in,
					   nit->second.backward_stage.out);
	}

	/* backward-consumer eligibility */
	for (i = 0; i < ids.size(); i++)
	{
		std::map<int, bool>::const_iterator cit = mgr->consumer_ok.find(ids[i]);

		appendStringInfo(out, "consumer_ok %d %s\n", ids[i],
						 (cit != mgr->consumer_ok.end() && cit->second) ? "yes" : "no");
	}
}
