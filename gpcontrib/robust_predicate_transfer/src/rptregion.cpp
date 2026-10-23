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
 * rptregion.cpp
 *	  Extract the RPT+ join graph -- base-scan vertices and equijoin
 *	  edges -- from a finished PlannedStmt.
 *
 * This pass replaces the RPT+ artifact's TableOperatorManager
 * (embryo-labs/dynamic-predicate-transfer): where DuckDB builds its transfer
 * graph from the logical operator tree before join ordering, we read the
 * finished plan after set_plan_references, so the join clauses are already
 * in OUTER_VAR/INNER_VAR form and the key columns must be traced back to
 * their base scans.  The traversal mirrors anser_inject_walk and the key
 * descent and equality validation mirror AnserResolveKeyScan and
 * anser_hashjoin_keys (all in gpcontrib/anser/src/anserplan.c), widened as
 * noted at each site.
 *
 * Phase I is analysis only: nothing here touches the plan tree.
 *
 * IDENTIFICATION
 *	  gpcontrib/robust_predicate_transfer/src/rptregion.cpp
 *
 *-------------------------------------------------------------------------
 */
extern "C"
{
#include "postgres.h"

#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "anserplan.h"
}

#include "rpt.h"

static bool rpt_is_vertex_scan(const Plan *plan);
static bool rpt_scan_is_filtered(const Plan *plan);
static RptVertexInfo *rpt_vertex_for_scan(PlannedStmt *stmt,
										  RptTransferMgr *mgr, Plan *scan);
static bool rpt_resolve_key_scan(Plan *top, AttrNumber key_attno,
								 Plan **scan_out, AttrNumber *attno_out);
static bool rpt_equality_clause(Node *clause, Var **outer_var,
								Var **inner_var, Oid *key_type);
static void rpt_add_edge(PlannedStmt *stmt, RptTransferMgr *mgr,
						 JoinType jointype,
						 Plan *outer_plan, AttrNumber outer_attno,
						 Plan *inner_plan, AttrNumber inner_attno,
						 Oid key_type);
static void rpt_collect_clauses(PlannedStmt *stmt, RptTransferMgr *mgr,
								JoinType jointype, Plan *outer, Plan *inner,
								List *clauses);
static void rpt_walk(PlannedStmt *stmt, RptTransferMgr *mgr, Plan *plan);

/*
 * True for the scan nodes a reducer pass could replay, the only accepted
 * vertices of the join graph.
 */
static bool
rpt_is_vertex_scan(const Plan *plan)
{
	return IsA(plan, SeqScan) ||
		IsA(plan, IndexScan) ||
		IsA(plan, IndexOnlyScan) ||
		IsA(plan, BitmapHeapScan);
}

/*
 * Does the scan carry restriction quals?  Index and bitmap scans restrict
 * through their index quals as well as through plan.qual.
 */
static bool
rpt_scan_is_filtered(const Plan *plan)
{
	if (plan->qual != NIL)
		return true;

	switch (nodeTag(plan))
	{
		case T_IndexScan:
			return ((IndexScan *) plan)->indexqual != NIL;
		case T_IndexOnlyScan:
			return ((IndexOnlyScan *) plan)->indexqual != NIL;
		case T_BitmapHeapScan:
			return ((BitmapHeapScan *) plan)->bitmapqualorig != NIL;
		default:
			return false;
	}
}

/*
 * Find or create the vertex for a base scan.  One vertex per rangetable
 * index, deduplicated through mgr->by_rti; vertices are created lazily, only
 * when a resolved equijoin edge reaches the scan, so a scan no key resolves
 * to never becomes a vertex.  The classification fields (intermediate,
 * unfiltered) are rptgraph.cpp's; join_keys accumulates here as edges are
 * recorded.
 */
static RptVertexInfo *
rpt_vertex_for_scan(PlannedStmt *stmt, RptTransferMgr *mgr, Plan *scan)
{
	int			rti = ((Scan *) scan)->scanrelid;
	std::map<int, int>::iterator it;
	RptVertexInfo v;

	it = mgr->by_rti.find(rti);
	if (it != mgr->by_rti.end())
		return &mgr->vertices[it->second];

	v.id = rti;
	v.scan = scan;
	v.cardinality = (int64) scan->plan_rows;
	v.filtered = rpt_scan_is_filtered(scan);
	v.intermediate = false;
	v.unfiltered = false;

	if (rti >= 1 && rti <= list_length(stmt->rtable))
	{
		RangeTblEntry *rte = rt_fetch(rti, stmt->rtable);

		if (rte->eref != NULL)
			v.relname = rte->eref->aliasname;
	}

	mgr->by_rti[rti] = (int) mgr->vertices.size();
	mgr->vertices.push_back(v);
	return &mgr->vertices.back();
}

/*
 * Follow one join-key column down to the base scan that originates it.
 * Modeled on AnserResolveKeyScan (gpcontrib/anser/src/anserplan.c) but not
 * calling it: Anser's resolver stops at SeqScan and descends only Hash,
 * Motion, HashJoin and its own nodes, where we accept the index and bitmap
 * scans as termini as well and see through MergeJoin, NestLoop, Sort and
 * Material too.  Where Anser's loop stops at the parent of the scan (it
 * needs the parent to relink the scan under a filter node), we only need the
 * scan and its key attno, so the terminus test runs on the node itself: the
 * descent steps into the scan, and the scan's own targetlist names its
 * table's columns with varno == scanrelid after set_plan_references.
 *
 * False means no single base scan holds the whole key column -- the column
 * comes from an Append, an Agg, a set operation, a SubqueryScan, a CTE, or
 * anything else not on the descent list.  That is a correctness gate, not a
 * missed optimization: a filter built on a partial source of the column
 * would reject rows that can join, so the caller drops the edge.
 */
static bool
rpt_resolve_key_scan(Plan *top, AttrNumber key_attno,
					 Plan **scan_out, AttrNumber *attno_out)
{
	Plan	   *node = top;
	AttrNumber	attno = key_attno;

	for (;;)
	{
		TargetEntry *tle;
		Var		   *var;
		Plan	   *child;

		if (node == NULL ||
			attno < 1 || attno > list_length(node->targetlist))
			return false;

		tle = (TargetEntry *) list_nth(node->targetlist, attno - 1);
		if (tle == NULL || !IsA(tle->expr, Var))
			return false;
		var = (Var *) tle->expr;

		/*
		 * Terminus: a base scan the reducer could replay.  The key column is
		 * the Var's attno in the scanned relation.  A targetlist Var naming
		 * anything but the scan's own rangetable entry (a whole-row Var, a
		 * placeholder) is not a plain column and ends the descent.
		 */
		if (rpt_is_vertex_scan(node))
		{
			Scan	   *scan = (Scan *) node;

			if (scan->scanrelid == 0 || var->varno != (int) scan->scanrelid)
				return false;
			*scan_out = node;
			*attno_out = var->varattno;
			return true;
		}

		/*
		 * Which child supplies this column.  As in AnserResolveKeyScan, a
		 * join is the only node here with two children; unlike it, all three
		 * join types are descended, not just HashJoin.  An Anser filter node
		 * is a passthrough with an identity targetlist (its Vars carry
		 * INDEX_VAR into custom_plans), so a scan Anser already filtered
		 * stays reachable.
		 */
		if (var->varno == OUTER_VAR)
			child = outerPlan(node);
		else if (var->varno == INNER_VAR &&
				 (IsA(node, HashJoin) || IsA(node, MergeJoin) ||
				  IsA(node, NestLoop)))
			child = innerPlan(node);
		else if (var->varno == INDEX_VAR && AnserIsRuntimeFilterScan(node))
			child = (Plan *) linitial(((CustomScan *) node)->custom_plans);
		else
			return false;

		if (child == NULL)
			return false;

		/*
		 * Anything not on this list is a region boundary: Append,
		 * MergeAppend, Agg, ProjectSet, SubqueryScan, CteScan, Unique, Limit,
		 * SetOp, WindowAgg, Result, the dynamic scans, and the rest.  The
		 * walk descends into them to find joins, but a key column never
		 * crosses them.
		 */
		if (!rpt_is_vertex_scan(child) &&
			!IsA(child, Hash) && !IsA(child, Motion) &&
			!IsA(child, HashJoin) && !IsA(child, MergeJoin) &&
			!IsA(child, NestLoop) &&
			!IsA(child, Sort) && !IsA(child, Material) &&
			!AnserIsRuntimeFilterScan(child))
			return false;

		node = child;
		attno = var->varattno;
	}
}

/*
 * Validate one equijoin clause and split it into outer/inner Vars.
 *
 * The shape and the operator validation are exactly anser_hashjoin_keys's
 * (gpcontrib/anser/src/anserplan.c), lifted from HashJoin->hashclauses to
 * any clause list: a two-argument OpExpr whose RelabelType-stripped args are
 * one OUTER_VAR and one INNER_VAR Var, over the same by-value base type on
 * both sides, excluding floats, with opno equal to the type's TYPECACHE_EQ_OPR
 * equality operator.  Anser hashes raw Datum bytes, and the transfer filters
 * will too, so SQL equality must coincide with bitwise Datum equality of the
 * key; anything looser could prune rows that actually join.  A NestLoop
 * clause with a Param on either side fails the Var test, as required --
 * such a clause is not a plain col = col join.
 *
 * Unlike Anser, which accepts only single-clause equijoins, callers pass one
 * clause at a time: edge weight is the shared-attribute count, so
 * multi-clause joins must be taken apart rather than refused.
 */
static bool
rpt_equality_clause(Node *clause, Var **outer_var, Var **inner_var,
					Oid *key_type)
{
	OpExpr	   *op;
	Node	   *l;
	Node	   *r;
	Var		   *lvar;
	Var		   *rvar;
	Oid			keytype;
	TypeCacheEntry *tce;

	if (clause == NULL || !IsA(clause, OpExpr))
		return false;
	op = (OpExpr *) clause;
	if (list_length(op->args) != 2)
		return false;

	l = (Node *) linitial(op->args);
	r = (Node *) lsecond(op->args);
	while (l != NULL && IsA(l, RelabelType))
		l = (Node *) ((RelabelType *) l)->arg;
	while (r != NULL && IsA(r, RelabelType))
		r = (Node *) ((RelabelType *) r)->arg;
	if (l == NULL || r == NULL || !IsA(l, Var) || !IsA(r, Var))
		return false;

	if (((Var *) l)->varno == OUTER_VAR && ((Var *) r)->varno == INNER_VAR)
	{
		lvar = (Var *) l;
		rvar = (Var *) r;
	}
	else if (((Var *) l)->varno == INNER_VAR && ((Var *) r)->varno == OUTER_VAR)
	{
		lvar = (Var *) r;
		rvar = (Var *) l;
	}
	else
		return false;
	*outer_var = lvar;
	*inner_var = rvar;

	if ((*inner_var)->vartype != (*outer_var)->vartype)
		return false;

	keytype = getBaseType((*inner_var)->vartype);
	if (!get_typbyval(keytype))
		return false;
	if (keytype == FLOAT4OID || keytype == FLOAT8OID)
		return false;

	/*
	 * The clause has to be the type's own equality rather than just some
	 * hashable operator spelled "=".  See anser_hashjoin_keys for why this
	 * alone is not the bitwise-equality proof and the float exclusion above
	 * is still needed.
	 */
	tce = lookup_type_cache(keytype, TYPECACHE_EQ_OPR);
	if (!OidIsValid(tce->eq_opr) || op->opno != tce->eq_opr)
		return false;

	*key_type = keytype;
	return true;
}

/*
 * Resolve one validated equijoin clause to its two base scans and record the
 * edge between their vertices.
 *
 * left_v/right_v are canonicalized (lower rtindex on the left), not the
 * join's outer/inner: every clause of a vertex pair merges into one
 * RptEdgeInfo and the neighbor matrix is symmetric, so a second join over
 * the same pair with the opposite orientation must land on the same edge
 * rather than colliding with its mirror image.  The clause's key pair is
 * stored in the same canonical order, and the protect flags name the
 * protected *vertex*: JOIN_LEFT protects the outer side's vertex (its rows
 * survive whether or not they match, so it must never be reduced by the
 * other side's keys), JOIN_RIGHT the inner side's, INNER and SEMI neither,
 * and every other join type (ANTI, FULL, LASJ_NOTIN, ...) makes no edge at
 * all.  protect_left therefore means left must NOT be reduced by right's
 * keys.
 *
 * key_type is the validated equality input type of the first clause recorded
 * for the edge; the data model carries one type per edge even though
 * different clauses of a pair could equate different types.
 */
static void
rpt_add_edge(PlannedStmt *stmt, RptTransferMgr *mgr, JoinType jointype,
			 Plan *outer_plan, AttrNumber outer_attno,
			 Plan *inner_plan, AttrNumber inner_attno, Oid key_type)
{
	Plan	   *outer_scan;
	Plan	   *inner_scan;
	AttrNumber	outer_key;
	AttrNumber	inner_key;
	int			outer_id;
	int			inner_id;
	RptVertexInfo *outer_v;
	RptVertexInfo *inner_v;
	int			left_id;
	int			right_id;
	RptKey		left_key;
	RptKey		right_key;
	int			protect_id = 0;
	RptEdgeInfo *edge;

	/* ANTI, FULL and the GP extras make no usable edge at all. */
	if (jointype != JOIN_INNER && jointype != JOIN_SEMI &&
		jointype != JOIN_LEFT && jointype != JOIN_RIGHT)
		return;

	if (!rpt_resolve_key_scan(outer_plan, outer_attno,
							  &outer_scan, &outer_key))
		return;
	if (!rpt_resolve_key_scan(inner_plan, inner_attno,
							  &inner_scan, &inner_key))
		return;

	outer_id = ((Scan *) outer_scan)->scanrelid;
	inner_id = ((Scan *) inner_scan)->scanrelid;

	/*
	 * Create both vertices before taking pointers into mgr.vertices: creating
	 * the second can reallocate the vector the first pointer refers into.
	 */
	rpt_vertex_for_scan(stmt, mgr, outer_scan);
	rpt_vertex_for_scan(stmt, mgr, inner_scan);
	outer_v = &mgr->vertices[mgr->by_rti[outer_id]];
	inner_v = &mgr->vertices[mgr->by_rti[inner_id]];

	/* The preserved side of an outer join is protected from reduction. */
	if (jointype == JOIN_LEFT)
		protect_id = outer_id;
	else if (jointype == JOIN_RIGHT)
		protect_id = inner_id;

	if (outer_id <= inner_id)
	{
		left_id = outer_id;
		left_key.rtindex = outer_id;
		left_key.attno = outer_key;
		right_id = inner_id;
		right_key.rtindex = inner_id;
		right_key.attno = inner_key;
	}
	else
	{
		left_id = inner_id;
		left_key.rtindex = inner_id;
		left_key.attno = inner_key;
		right_id = outer_id;
		right_key.rtindex = outer_id;
		right_key.attno = outer_key;
	}

	edge = &mgr->neighbor_matrix[left_id][right_id];
	if (edge->clauses.empty())
	{
		edge->left_v = left_id;
		edge->right_v = right_id;
		edge->key_type = key_type;
		edge->protect_left = false;
		edge->protect_right = false;
	}
	edge->clauses.push_back(std::make_pair(left_key, right_key));
	if (protect_id == left_id)
		edge->protect_left = true;
	else if (protect_id == right_id)
		edge->protect_right = true;

	outer_v->join_keys.insert(outer_id == left_id ? left_key : right_key);
	inner_v->join_keys.insert(inner_id == left_id ? left_key : right_key);

	/* The matrix is symmetric: both directions name the same edge value. */
	mgr->neighbor_matrix[right_id][left_id] = *edge;
}

/*
 * Run one join's clause list through validation, resolution and edge
 * recording.  Clauses that fail validation or resolution are skipped
 * individually; a join contributes an edge only for the clauses that
 * survive.
 */
static void
rpt_collect_clauses(PlannedStmt *stmt, RptTransferMgr *mgr, JoinType jointype,
					Plan *outer, Plan *inner, List *clauses)
{
	ListCell   *lc;

	foreach(lc, clauses)
	{
		Var		   *outer_var;
		Var		   *inner_var;
		Oid			key_type;

		if (!rpt_equality_clause((Node *) lfirst(lc),
								 &outer_var, &inner_var, &key_type))
			continue;

		rpt_add_edge(stmt, mgr, jointype,
					 outer, outer_var->varattno,
					 inner, inner_var->varattno, key_type);
	}
}

/*
 * Manual recursive walk, after anser_inject_walk
 * (gpcontrib/anser/src/anserplan.c): the outerPlan/innerPlan spine plus the
 * child lists anser's walk omits (Append, MergeAppend, Sequence, BitmapAnd,
 * BitmapOr, SubqueryScan, CustomScan custom_plans), widened the same way
 * anser_max_plan_node_id widens its own.  plan->initPlan is skipped on
 * purpose: the SubPlan's Plan tree sits in stmt->subplans, which
 * RptCollectGraph walks separately.  Descending these lists finds joins
 * behind region boundaries; the resolver still refuses to cross them, so a
 * join inside a UNION arm can link scans of that arm but never out of it.
 */
static void
rpt_walk(PlannedStmt *stmt, RptTransferMgr *mgr, Plan *plan)
{
	ListCell   *lc;

	if (plan == NULL)
		return;

	switch (nodeTag(plan))
	{
		case T_HashJoin:
			rpt_collect_clauses(stmt, mgr, ((HashJoin *) plan)->join.jointype,
								outerPlan(plan), innerPlan(plan),
								((HashJoin *) plan)->hashclauses);
			break;
		case T_MergeJoin:
			rpt_collect_clauses(stmt, mgr, ((MergeJoin *) plan)->join.jointype,
								outerPlan(plan), innerPlan(plan),
								((MergeJoin *) plan)->mergeclauses);
			break;
		case T_NestLoop:
			rpt_collect_clauses(stmt, mgr, ((NestLoop *) plan)->join.jointype,
								outerPlan(plan), innerPlan(plan),
								((NestLoop *) plan)->join.joinqual);
			break;
		default:
			break;
	}

	rpt_walk(stmt, mgr, outerPlan(plan));
	rpt_walk(stmt, mgr, innerPlan(plan));

	switch (nodeTag(plan))
	{
		case T_Append:
			foreach(lc, ((Append *) plan)->appendplans)
				rpt_walk(stmt, mgr, (Plan *) lfirst(lc));
			break;
		case T_MergeAppend:
			foreach(lc, ((MergeAppend *) plan)->mergeplans)
				rpt_walk(stmt, mgr, (Plan *) lfirst(lc));
			break;
		case T_Sequence:
			foreach(lc, ((Sequence *) plan)->subplans)
				rpt_walk(stmt, mgr, (Plan *) lfirst(lc));
			break;
		case T_BitmapAnd:
			foreach(lc, ((BitmapAnd *) plan)->bitmapplans)
				rpt_walk(stmt, mgr, (Plan *) lfirst(lc));
			break;
		case T_BitmapOr:
			foreach(lc, ((BitmapOr *) plan)->bitmapplans)
				rpt_walk(stmt, mgr, (Plan *) lfirst(lc));
			break;
		case T_SubqueryScan:
			rpt_walk(stmt, mgr, ((SubqueryScan *) plan)->subplan);
			break;
		case T_CustomScan:
			foreach(lc, ((CustomScan *) plan)->custom_plans)
				rpt_walk(stmt, mgr, (Plan *) lfirst(lc));
			break;
		default:
			break;
	}
}

/*
 * Extract the join graph of a finished statement: walk the main plan tree
 * and every subplan, create a vertex for each base scan a resolved equijoin
 * edge reaches, and record the edges in the (symmetric) neighbor matrix.
 * Only vertices, by_rti and neighbor_matrix are filled here; the
 * classification, equivalence classes and the transfer plan itself are
 * rptgraph.cpp's and rptplan.cpp's passes over the same manager.
 */
void
RptCollectGraph(PlannedStmt *stmt, RptTransferMgr *mgr)
{
	ListCell   *lc;

	if (stmt == NULL || stmt->planTree == NULL)
		return;

	rpt_walk(stmt, mgr, stmt->planTree);
	foreach(lc, stmt->subplans)
		rpt_walk(stmt, mgr, (Plan *) lfirst(lc));
}
