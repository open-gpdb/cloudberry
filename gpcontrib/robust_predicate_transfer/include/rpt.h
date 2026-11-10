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
 * rpt.h
 *	  Shared data model for the robust_predicate_transfer extension.
 *
 * The structures here are a port of the RPT+ artifact's transfer graph
 * (embryo-labs/dynamic-predicate-transfer, MIT license, Copyright 2018-2025
 * Stichting DuckDB Foundation; see licenses/LICENSE-mit-duckdb-rpt.txt):
 * DuckDB's ColumnBinding becomes RptKey (rtindex, attno), EdgeInfo becomes
 * RptEdgeInfo, JoinKeyTableGroup becomes RptKeyGroup, and dag.hpp's
 * GraphNode/GraphEdge become RptGraphNode/RptGraphEdge.  What does not exist
 * there is ours: the extraction of the join graph from a finished
 * PlannedStmt (rptregion.cpp) replaces DuckDB's TableOperatorManager, and
 * the consumer-eligibility check answers an MPP question (Anser's deadlock
 * rule) the single-node original never had.
 *
 * Phase I is a pure analysis pass: nothing is injected or executed, and the
 * dumps in rptplan.cpp are the only output.  STL containers own malloc-backed
 * storage. PostgreSQL errors must be translated to C++ exceptions before
 * unwinding those containers, then rethrown at the C boundary.
 *
 * IDENTIFICATION
 *	  gpcontrib/robust_predicate_transfer/include/rpt.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RPT_H
#define RPT_H

extern "C"
{
#include "postgres.h"

#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "lib/stringinfo.h"
}

#include <map>
#include <set>
#include <string>
#include <vector>

/*
 * Like gpopt's GP_WRAP: never longjmp over a live C++ container. Only call
 * PostgreSQL through this boundary; callbacks must do the same for their
 * own PostgreSQL calls. The guard also restores PG's stacks when a callback
 * throws a C++ exception through a C walker.
 */
struct RptPgError {};

struct RptPgStackGuard
{
	sigjmp_buf *exception_stack;
	ErrorContextCallback *context_stack;

	RptPgStackGuard() : exception_stack(PG_exception_stack),
		context_stack(error_context_stack) {}
	~RptPgStackGuard()
	{
		PG_exception_stack = exception_stack;
		error_context_stack = context_stack;
	}
};

template <typename F>
auto RptPgCall(const F &call) -> decltype(call())
{
	RptPgStackGuard guard;
	sigjmp_buf local;
	int saved_dispatch_nest_level = dispatch_nest_level;

	if (sigsetjmp(local, 0) != 0)
	{
		dispatch_nest_level = saved_dispatch_nest_level;
		throw RptPgError();
	}
	PG_exception_stack = &local;
	return call();
}

/* GUCs (defined in rptinit.cpp). */
extern bool rpt_enable;
extern bool rpt_debug;
extern bool rpt_backward;
extern int rpt_max_backward_consumers;
extern int	rpt_max_chain_depth;

/*
 * One join key: a column of a base relation.  Ports DuckDB's ColumnBinding;
 * the union-find element for equivalence classes.
 */
struct RptKey
{
	int			rtindex;		/* rangetable index of the base relation */
	AttrNumber	attno;			/* column in that relation */
};

inline bool
operator<(const RptKey &a, const RptKey &b)
{
	return a.rtindex < b.rtindex ||
		(a.rtindex == b.rtindex && a.attno < b.attno);
}

inline bool
operator==(const RptKey &a, const RptKey &b)
{
	return a.rtindex == b.rtindex && a.attno == b.attno;
}

/*
 * A base scan taking part in at least one resolved equijoin edge; the join
 * graph's vertex.  Accepted scan types are the ones a reducer pass could
 * replay: SeqScan (heap and AO/AOCO), IndexScan, IndexOnlyScan,
 * BitmapHeapScan.  Everything else never becomes a vertex, because no key
 * resolution reaches it.
 */
struct RptVertexInfo
{
	int			id;				/* rtindex of the scan, the vertex identity */
	Plan	   *scan;			/* the base scan node */
	Oid relid = InvalidOid;
	std::string relname;		/* RTE alias, for dumps */
	int64		cardinality;	/* scan->plan_rows */
	bool		filtered;		/* the scan carries restriction quals */
	bool		intermediate;	/* touches >1 equivalence class */
	bool		unfiltered;		/* neither filtered nor intermediate */
	bool replicated = false;
	int numsegments = 0;
	std::vector<AttrNumber> distkeys;
	std::vector<Oid> distopclasses;
	std::set<RptKey> join_keys; /* this table's keys over all its edges */
};

/*
 * One equijoin edge between two vertices.  Ports EdgeInfo.  All equality
 * clauses of one join over the same vertex pair merge into one edge, so
 * clauses.size() is the shared-attribute count the paper uses as the edge
 * weight; each clause is one (left key, right key) pair.
 *
 * protect_left/protect_right name the protected destination: the preserved side of an
 * outer join keeps its rows whether or not they match, so it must never be
 * reduced by the other side's keys.  LEFT protects left and RIGHT
 * protects right; unsupported join kinds, ANTI and FULL create no edge.
 */
struct RptEdgeInfo
{
	int			left_v;
	int			right_v;		/* vertex ids (RptVertexInfo.id) */
	Oid			key_type;		/* the equality operator's input type */
	std::vector<std::pair<RptKey, RptKey> > clauses;
	bool		protect_left;
	bool		protect_right;
};

/*
 * One equivalence class of join keys.  Ports JoinKeyTableGroup: members are
 * the vertices touching the class, and the leader is the vertex the backward
 * broadcast builds its single filter on (registered in Prim insertion
 * order, so it is the class's earliest-placed vertex).
 */
struct RptKeyGroup
{
	std::set<int> members;
	int			leader_id;		/* -1 until registered */
	RptKey		leader_key;
	Oid			key_type;
};

/* Ports dag.hpp's GraphEdge: one transfer edge of the built plan. */
struct RptGraphEdge
{
	int			destination;
	std::vector<RptKey> left_keys;	/* keys on the smaller table's side */
	std::vector<RptKey> right_keys; /* keys on the larger table's side */
	std::vector<Oid>	key_types;
	bool local = false; /* policy permits local merge; never use QD transport */
};

/* Ports dag.hpp's GraphNode: one vertex's full transfer plan. */
struct RptGraphNode
{
	int			id;
	int32_t		cardinality_order;	/* Prim insertion order (0 = root) */

	struct
	{
		std::vector<RptGraphEdge> in;
		std::vector<RptGraphEdge> out;
	}			forward_stage,
				backward_stage;
};

/* One class/member decision. Relink points and tuple-output positions are
 * recorded only after the complete consumer and producer paths are proven. */
struct RptBackwardConsumer
{
	RptKey class_key = {0, 0};
	int producer = 0;
	int consumer = 0;
	Plan *producer_parent = NULL;
	Plan *consumer_parent = NULL;
	AttrNumber producer_attno = 0;
	AttrNumber consumer_attno = 0;
	int producer_slice = -1;
	int consumer_slice = -1;
	bool local = false;
	bool rule9 = false;
	bool strict_eligible = false;
	bool eligible = false;
	bool defer_first = true;
	double estimated_rows_removed = -1; /* unknown without column statistics */
	std::string reason;
};

/*
 * The whole analysis.  Ports TransferGraphManager; the neighbor matrix is
 * symmetric (both [a][b] and [b][a] name the same edge) as in the original.
 */
struct RptTransferMgr
{
	/* rptregion.cpp fills these from the PlannedStmt. */
	std::vector<RptVertexInfo> vertices;
	std::map<int, int>	by_rti;		/* vertex id -> index in vertices */
	/* vertex id -> (vertex id -> edge) */
	std::map<int, std::map<int, RptEdgeInfo> > neighbor_matrix;

	/* rptgraph.cpp: classes and classification. */
	std::map<RptKey, RptKeyGroup> groups;	/* by class representative key */
	std::map<RptKey, RptKey> uf_parent;		/* union-find over keys */

	/* rptplan.cpp: the transfer plan. */
	std::map<int, RptGraphNode> graph;
	std::set<int> roots;
	std::set<int> bridges;
	std::vector<int> reducer_order;
	std::vector<int> transfer_order;	/* Prim insertion order of vertex ids */

	/* Eligibility for blocking global backward consumers only. Every global
	 * producer must be on a hash join's build side in a different slice from
	 * the probe-side consumer. Local edges never wait and do not participate;
	 * a vertex with no global in-edges has consumer_ok=false. */
	std::map<int, bool> consumer_ok;
	std::vector<RptBackwardConsumer> backward_consumers;
};

/*
 * Phase III forward pass.  A channel is one forward transfer edge: the
 * reducer scan of `from` builds a bloom filter on from_key, and every scan of
 * `to` (in the reducer, and the root's scan in the main plan) applies it on
 * to_key.  Only the first clause of a multi-key edge is transferred; one key
 * of a conjunctive equality still over-approximates, so this loses
 * selectivity, never rows.
 */
struct RptForwardChannel
{
	int			from = 0;
	int			to = 0;
	RptKey		from_key = {0, 0};
	RptKey		to_key = {0, 0};
	bool		local = false;		/* policy-local (rptplan.cpp, rpt_local_hop) */
	bool		in_process = false; /* local and consumed only inside the reducer:
									 * handed over in-process, no Anser channel */
	uint32		condition_id = 0;
	std::string condition_key;
	int64		total_elems = 0;
	int64		max_payload = 0;
	int64		planned_bytes = 0;
	double		est_cost_ms = 0;
};

/* One root's forward chain and the decision about it. */
struct RptForwardComponent
{
	int			root = 0;
	std::vector<int> chain;			/* reducer scans, producers before consumers */
	std::vector<RptForwardChannel> channels;
	double		est_cost_ms = 0;	/* sum of hop estimates */
	double		est_selectivity = 1;	/* estimated fraction of root rows kept */
	bool		accepted = false;
	std::string reason;				/* "inject", or why not */
};

struct RptForwardParams
{
	int			width = 0;			/* reducer gang size: producers per channel */
	double		ms_per_mib_part = 0;	/* c in c * n_producers * filter_MiB */
	double		max_exchange_ms = 0;	/* component budget; < 0 disables the limit */
	double		min_root_rows = 0;	/* root scan plan_rows below this: no reducer */
	double		max_selectivity = 1;	/* lambda; >= 1 disables the check */
	bool		apply_all = false;	/* chain scans of the main plan consume too */
};

/*
 * What the reducer scan of one vertex is expected to keep, from the plan and
 * the catalog (rptforward.cpp).  `rows` is per segment, before quals; it sizes
 * the vertex's filters together with `selectivity`, the fraction its own
 * reducer quals keep -- 1 when they were dropped (rule 13).  Vertices with no
 * entry use their plan_rows and selectivity 1.
 */
struct RptForwardVertexEst
{
	double		rows = -1;
	double		selectivity = 1;
	bool		quals_dropped = false;
};

/* Distinct values of a column over the whole table, or < 0 when unknown. */
typedef double (*RptNdvFn) (const RptVertexInfo &vertex, AttrNumber attno);

/* Anser's bloom sizing (AnserRuntimeFilterSize), injectable for unit tests. */
typedef bool (*RptFilterSizeFn) (double est_rows, int64 *total_elems,
								  int64 *max_payload, int64 *planned_bytes);

/* rptforwardplan.cpp: pure selection over a forward schedule. */
extern void RptSelectForward(const RptTransferMgr &forward,
							 const RptForwardParams &params,
							 const std::map<int, RptForwardVertexEst> &est,
							 RptFilterSizeFn size_fn, RptNdvFn ndv_fn,
							 std::vector<RptForwardComponent> *out);
extern void RptDumpForward(const std::vector<RptForwardComponent> &components,
						   std::string *out);

/* rptforward.cpp: the planner-hook pass.  Returns a new statement, or stmt
 * itself when nothing is injected; *trace receives the decision dump.  May
 * throw; the caller fails open. */
extern PlannedStmt *RptInjectForward(PlannedStmt *stmt, bool materialize,
									 std::string *trace);

/* Phase III GUCs (rptinit.cpp). */
extern bool rpt_forward;
extern int	rpt_forward_apply;			/* RPT_FORWARD_APPLY_* */
extern double rpt_forward_ms_per_mib_part;
extern double rpt_forward_max_exchange_ms;
extern double rpt_forward_min_root_rows;
extern double rpt_forward_max_selectivity;

#define RPT_FORWARD_APPLY_ROOT	0
#define RPT_FORWARD_APPLY_ALL	1

/* rptregion.cpp: extract the join graph from the finished statement. */
extern void RptCollectGraph(PlannedStmt *stmt, RptTransferMgr *mgr);

extern RptKey RptFindKeyRoot(const RptKey &key,
							   std::map<RptKey, RptKey> &uf_parent);

/* rptgraph.cpp: union-find classes, classification, SkipUnfilteredTable. */
extern void RptBuildClasses(RptTransferMgr *mgr);

/* rptplan.cpp: the Prim forest (LargestRootUpdated), forward/backward stage
 * assignment (CreateTransferPlanUpdated), consumer eligibility, and dumps. */
extern void RptBuildTransferPlan(PlannedStmt *stmt, RptTransferMgr *mgr);
extern void RptBuildForwardPlan(const RptTransferMgr *analysis,
								RptTransferMgr *forward);
extern void RptAttachReducerInitPlan(PlannedStmt *stmt, List *children, int width);
extern void RptCheckBackwardConsumers(PlannedStmt *stmt, RptTransferMgr *mgr);
extern double RptEstimateDistinct(const RptVertexInfo &vertex, AttrNumber attno);
extern void RptDumpTransferPlan(const RptTransferMgr *mgr, StringInfo out);

#endif							/* RPT_H */
