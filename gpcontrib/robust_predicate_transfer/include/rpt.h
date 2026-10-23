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
 * dumps in rptplan.cpp are the only output.  Everything lives in transient
 * C++ containers in the caller's memory context; nothing may rely on a
 * destructor running across an elog(ERROR) longjmp.
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

/* GUCs (defined in rptinit.cpp). */
extern bool rpt_enable;
extern bool rpt_debug;
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
	std::string relname;		/* RTE alias, for dumps */
	int64		cardinality;	/* scan->plan_rows */
	bool		filtered;		/* the scan carries restriction quals */
	bool		intermediate;	/* touches >1 equivalence class */
	bool		unfiltered;		/* neither filtered nor intermediate */
	std::set<RptKey> join_keys; /* this table's keys over all its edges */
};

/*
 * One equijoin edge between two vertices.  Ports EdgeInfo.  All equality
 * clauses of one join over the same vertex pair merge into one edge, so
 * clauses.size() is the shared-attribute count the paper uses as the edge
 * weight; each clause is one (left key, right key) pair.
 *
 * protect_left/protect_right are ported verbatim: the preserved side of an
 * outer join keeps its rows whether or not they match, so it must never be
 * reduced by the other side's keys.  LEFT protects left, RIGHT and MARK
 * protect right; ANTI and FULL never create an edge at all.
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
	std::vector<int> transfer_order;	/* Prim insertion order of vertex ids */

	/* Our addition: backward-consumer eligibility (Anser's deadlock rule) --
	 * consumer_ok[v] is true only where v's scan sits on the probe side of a
	 * hash join whose build side transitively contains the vertex's class
	 * leader's scan. */
	std::map<int, bool> consumer_ok;
};

/* rptregion.cpp: extract the join graph from the finished statement. */
extern void RptCollectGraph(PlannedStmt *stmt, RptTransferMgr *mgr);

/* rptgraph.cpp: union-find classes, classification, SkipUnfilteredTable. */
extern void RptBuildClasses(RptTransferMgr *mgr);

/* rptplan.cpp: the Prim forest (LargestRootUpdated), forward/backward stage
 * assignment (CreateTransferPlanUpdated), consumer eligibility, and dumps. */
extern void RptBuildTransferPlan(PlannedStmt *stmt, RptTransferMgr *mgr);
extern void RptDumpTransferPlan(const RptTransferMgr *mgr, StringInfo out);

#endif							/* RPT_H */
