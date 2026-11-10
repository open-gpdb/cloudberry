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
 * rptforward.cpp
 *	  Phase III: build the forward reducer as an InitPlan of the statement
 *	  and apply its filters to the root scans of the main plan.
 *
 * A transfer is an Anser channel, except a local hop consumed only inside
 * the reducer, which is handed over in-process (rptlocal.c).  The reducer
 * InitPlan is dispatched by
 * preprocess_initplans() before the main plan, under the same
 * gp_command_count, and the coordinator keeps a command's channels until
 * ExecutorEnd -- so every channel is complete before a main-plan consumer
 * subscribes, and main-plan consumers never wait on anything still running.
 *
 * Reducer scans are copies of the main plan's scans (same scanrelid, so the
 * same RTE, locks and permission checks) with a key-only targetlist.  Their
 * row set must be the main scan's or a superset (design rule 13): the quals
 * are copied whole, in order, or dropped whole unless every one of them is
 * proven unable to fail.  The reducer runs eagerly, over rows the main plan
 * may never visit (LIMIT 0, an empty outer side), so a qual that can raise an
 * error there -- 100 / a with a = 0 -- would turn a successful query into a
 * failing one, and the planner's fail-open cannot catch an executor error.
 *
 * Everything runs on a private copy of the statement; the caller publishes
 * it only when the whole pass succeeds.  PostgreSQL calls are made through
 * RptPgCall, because C++ containers are alive throughout.
 *
 * IDENTIFICATION
 *	  gpcontrib/robust_predicate_transfer/src/rptforward.cpp
 *
 *-------------------------------------------------------------------------
 */
extern "C"
{
#include "postgres.h"

#include "cdb/cdbllize.h"
#include "cdb/cdbutil.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/walkers.h"
#include "utils/lsyscache.h"

#include "catalog/pg_class.h"
#include "utils/syscache.h"

#include "anser.h"
#include "anserplan.h"
#include "rptlocal.h"
}

#include "rpt.h"
#include "rptqual.h"

#include <algorithm>

/* ---- finding where a main-plan scan hangs ---- */

typedef struct RptParentContext
{
	plan_tree_base_prefix base;		/* must be first */
	const std::set<Plan *> *targets;
	std::map<Plan *, Plan *> *parents;
	std::vector<Plan *> *stack;		/* enclosing plan nodes */
} RptParentContext;

static bool
rpt_parent_walker(Node *node, void *context)
{
	RptParentContext *ctx = (RptParentContext *) context;

	if (node == NULL)
		return false;
	if (!is_plan_node(node))
		return plan_tree_walker(node, rpt_parent_walker, context, false);

	if (ctx->targets->count((Plan *) node) && !ctx->stack->empty())
		(*ctx->parents)[(Plan *) node] = ctx->stack->back();

	ctx->stack->push_back((Plan *) node);
	bool		result = plan_tree_walker(node, rpt_parent_walker, context, false);

	ctx->stack->pop_back();
	return result;
}

/* Parents of the targets in the main plan tree only; SubPlans are not
 * entered, so a scan inside one is simply not found. */
static std::map<Plan *, Plan *>
rpt_find_parents(PlannedStmt *stmt, const std::set<Plan *> &targets)
{
	std::map<Plan *, Plan *> parents;
	std::vector<Plan *> stack;
	RptParentContext ctx;

	ctx.targets = &targets;
	ctx.parents = &parents;
	ctx.stack = &stack;
	RptPgCall([&]() {
		exec_init_plan_tree_base(&ctx.base, stmt);
		(void) rpt_parent_walker((Node *) stmt->planTree, &ctx);
	});
	return parents;
}

/* Replace `child` under `parent`.  Only the links key resolution can pass
 * through hold base scans here: the outer/inner trees and Anser's own nodes. */
static bool
rpt_relink(Plan *parent, Plan *child, Plan *replacement)
{
	if (parent->lefttree == child)
	{
		parent->lefttree = replacement;
		return true;
	}
	if (parent->righttree == child)
	{
		parent->righttree = replacement;
		return true;
	}
	if (IsA(parent, CustomScan))
	{
		ListCell   *lc;

		foreach(lc, ((CustomScan *) parent)->custom_plans)
		{
			if (lfirst(lc) == child)
			{
				lfirst(lc) = replacement;
				return true;
			}
		}
	}
	return false;
}

/* Output position of a base column in a scan's targetlist, or 0. */
static AttrNumber
rpt_scan_key_position(Plan *scan, const RptKey &key)
{
	ListCell   *lc;

	foreach(lc, scan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (IsA(tle->expr, Var) &&
			((Var *) tle->expr)->varno == (int) ((Scan *) scan)->scanrelid &&
			((Var *) tle->expr)->varattno == key.attno)
			return tle->resno;
	}
	return 0;
}

/* ---- rule 13: quals a reducer scan may evaluate ---- */

/* Only explicitly trusted built-in comparisons and their hash support may
 * run eagerly. LEAKPROOF prevents argument disclosure, not unconditional
 * errors. Every other call, coercion, Param or SubPlan drops the whole qual. */
static bool
rpt_unsafe_qual_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_List:
		case T_BoolExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_RelabelType:
			break;
		case T_FuncExpr:
			if (!RptQualFunctionSafe(((FuncExpr *) node)->funcid))
				return true;
			break;
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
			set_opfuncid((OpExpr *) node);
			if (!RptQualFunctionSafe(((OpExpr *) node)->opfuncid))
				return true;
			break;
		case T_ScalarArrayOpExpr:
		{
			ScalarArrayOpExpr *op = (ScalarArrayOpExpr *) node;

			set_sa_opfuncid(op);
			if (!RptQualFunctionSafe(op->opfuncid) ||
				(OidIsValid(op->negfuncid) && !RptQualFunctionSafe(op->negfuncid)) ||
				(OidIsValid(op->hashfuncid) && !RptQualHashFunctionSafe(op->hashfuncid)))
				return true;
			break;
		}
		default:
			return true;		/* Param, SubPlan, coercions, everything else */
	}
	return expression_tree_walker(node, rpt_unsafe_qual_walker, context);
}

/* Whether a reducer may evaluate these quals (all of them, or none). */
static bool
rpt_quals_safe(List *quals)
{
	return quals == NIL || RptPgCall([&]() {
		return !contain_volatile_functions((Node *) quals) &&
			!rpt_unsafe_qual_walker((Node *) quals, NULL);
	});
}

/* All of the main scan's quals, or none of them. */
static List *
rpt_reducer_quals(List *quals)
{
	if (quals == NIL || !rpt_quals_safe(quals))
		return NIL;
	return (List *) copyObjectImpl(quals);
}

/* Rows per segment before quals, from pg_class.reltuples; < 0 when unknown.
 * Partitioned tables spread them over numsegments, a replicated table keeps
 * them all on each segment (as costsize.c's adjust_reloptinfo). */
static double
rpt_base_rows(const RptVertexInfo &v)
{
	if (!OidIsValid(v.relid))
		return -1;
	return RptPgCall([&]() {
		HeapTuple	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(v.relid));
		double		rows;

		if (!HeapTupleIsValid(tuple))
			return -1.0;
		rows = ((Form_pg_class) GETSTRUCT(tuple))->reltuples;
		ReleaseSysCache(tuple);
		if (rows < 0)
			return -1.0;
		if (!v.replicated && v.numsegments > 0)
			rows /= v.numsegments;
		return rows;
	});
}

/* What each vertex's reducer scan is expected to keep. */
static std::map<int, RptForwardVertexEst>
rpt_vertex_estimates(const RptTransferMgr &forward)
{
	std::map<int, RptForwardVertexEst> est;

	for (const RptVertexInfo &v : forward.vertices)
	{
		RptForwardVertexEst e;
		double		base = rpt_base_rows(v);

		e.quals_dropped = !rpt_quals_safe(v.scan->qual);
		e.rows = base > 0 ? base : (double) v.cardinality;
		if (v.scan->qual != NIL && !e.quals_dropped && base > 0)
			e.selectivity = std::min(1.0, std::max(0.0, v.scan->plan_rows / base));
		est[v.id] = e;
	}
	return est;
}

/* ---- node construction ---- */

static void
rpt_reset_copied_plan(Plan *plan, int *next_id)
{
	plan->plan_node_id = (*next_id)++;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	plan->initPlan = NIL;
	plan->extParam = NULL;
	plan->allParam = NULL;
	plan->parallel_aware = false;
	plan->async_capable = false;
	plan->parallel = 0;
	plan->flow = NULL;
}

static Plan *
rpt_reducer_scan(Plan *main_scan, Oid relid, const std::vector<AttrNumber> &attnos,
				 bool *quals_dropped, int *next_id)
{
	Plan	   *scan = (Plan *) copyObjectImpl(main_scan);
	List	   *tlist = NIL;
	Index		varno = ((Scan *) main_scan)->scanrelid;
	AttrNumber	resno = 0;

	for (AttrNumber attno : attnos)
	{
		Oid			typid;
		int32		typmod;
		Oid			collid;

		get_atttypetypmodcoll(relid, attno, &typid, &typmod, &collid);
		tlist = lappend(tlist,
						makeTargetEntry((Expr *) makeVar(varno, attno, typid, typmod, collid, 0),
										++resno, NULL, false));
	}
	scan->targetlist = tlist;
	scan->qual = rpt_reducer_quals(main_scan->qual);
	*quals_dropped = (main_scan->qual != NIL && scan->qual == NIL);
	rpt_reset_copied_plan(scan, next_id);
	return scan;
}

static Plan *
rpt_wrap_consumer(Plan *child, AttrNumber position, const RptForwardChannel &ch,
				  int width, int *next_id)
{
	CustomScan *cs = AnserBuildBloomConsumerScan(child, position, ch.condition_id,
												 ch.condition_key.c_str(),
												 ch.total_elems, (Size) ch.max_payload,
												 ch.planned_bytes, width,
												 false /* channels are complete */ );

	cs->scan.plan.plan_node_id = (*next_id)++;
	return (Plan *) cs;
}

static Plan *
rpt_wrap_producer(Plan *child, AttrNumber position, const RptForwardChannel &ch,
				  int width, int *next_id)
{
	CustomScan *cs = AnserBuildBloomProducerScan(child, position, ch.condition_id,
												 ch.condition_key.c_str(),
												 ch.total_elems, (Size) ch.max_payload,
												 ch.planned_bytes, width);

	cs->scan.plan.plan_node_id = (*next_id)++;
	return (Plan *) cs;
}

static bool
rpt_anser_size(double est_rows, int64 *total_elems, int64 *max_payload,
			   int64 *planned_bytes)
{
	return RptPgCall([&]() {
		return AnserRuntimeFilterSize(est_rows, total_elems, max_payload,
									  planned_bytes);
	});
}

static bool
rpt_plan_has_anser_nodes_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (is_plan_node(node) &&
		(AnserIsRuntimeFilterScan((Plan *) node) || RptIsLocalScan((Plan *) node)))
		return true;
	return plan_tree_walker(node, rpt_plan_has_anser_nodes_walker, context, true);
}

/* Main-plan scans of `vertex`, wrapped in consumers of every channel into it.
 * False when the scan cannot be found or keyed; nothing is changed then. */
static bool
rpt_apply_in_main(const RptVertexInfo &vertex,
				  const std::vector<RptForwardChannel> &channels,
				  const std::map<Plan *, Plan *> &parents, int width,
				  int *next_id, bool dry_run)
{
	auto		parent = parents.find(vertex.scan);

	if (!IsA(vertex.scan, SeqScan) || parent == parents.end())
		return false;
	for (const RptForwardChannel &ch : channels)
		if (ch.to == vertex.id && !ch.in_process &&
			rpt_scan_key_position(vertex.scan, ch.to_key) == 0)
			return false;
	if (dry_run)
		return true;

	Plan	   *node = vertex.scan;

	for (const RptForwardChannel &ch : channels)
		if (ch.to == vertex.id && !ch.in_process)
			node = RptPgCall([&]() {
				return rpt_wrap_consumer(node, rpt_scan_key_position(vertex.scan, ch.to_key),
										 ch, width, next_id);
			});
	return node != vertex.scan && rpt_relink(parent->second, vertex.scan, node);
}

PlannedStmt *
RptInjectForward(PlannedStmt *stmt, bool materialize, std::string *trace)
{
	if (!gp_anser_enable || gp_anser_runtime_filter)
	{
		*trace += "forward: needs anser.enable=on and anser.runtime_filter=off\n";
		return stmt;
	}
	if (RptPgCall([&]() {
		return rpt_plan_has_anser_nodes_walker((Node *) stmt->planTree, NULL);
	}))
	{
		*trace += "forward: plan already carries Anser nodes\n";
		return stmt;
	}

	PlannedStmt *work = materialize
		? RptPgCall([&]() { return (PlannedStmt *) copyObjectImpl(stmt); })
		: stmt;

	RptTransferMgr analysis;

	RptCollectGraph(work, &analysis);
	if (analysis.vertices.empty())
	{
		*trace += "forward: no transfer regions\n";
		return stmt;
	}
	RptBuildClasses(&analysis);
	RptBuildTransferPlan(work, &analysis);

	RptTransferMgr forward;

	RptBuildForwardPlan(&analysis, &forward);

	RptForwardParams params;

	params.width = RptPgCall([]() { return getgpsegmentCount(); });
	params.ms_per_mib_part = rpt_forward_ms_per_mib_part;
	params.max_exchange_ms = rpt_forward_max_exchange_ms;
	params.min_root_rows = rpt_forward_min_root_rows;
	params.max_selectivity = rpt_forward_max_selectivity;
	params.apply_all = (rpt_forward_apply == RPT_FORWARD_APPLY_ALL);

	std::map<int, RptForwardVertexEst> est = rpt_vertex_estimates(forward);
	std::vector<RptForwardComponent> comps;

	RptSelectForward(forward, params, est, rpt_anser_size, RptEstimateDistinct, &comps);

	/* Preflight against the plan: everything that could refuse a component
	 * is checked before any node is built, so a refusal changes nothing. */
	std::set<Plan *> targets;

	for (const RptVertexInfo &v : forward.vertices)
		targets.insert(v.scan);

	std::map<Plan *, Plan *> parents = rpt_find_parents(work, targets);
	int			next_id = 0;	/* unused by dry runs */

	for (RptForwardComponent &comp : comps)
	{
		if (!comp.accepted)
			continue;
		for (int id : comp.chain)
		{
			const RptVertexInfo &v = forward.vertices[forward.by_rti.at(id)];

			if (!IsA(v.scan, SeqScan) || !OidIsValid(v.relid))
			{
				comp.accepted = false;
				comp.reason = "chain table " + std::to_string(id) + " is not a plain SeqScan";
				break;
			}
		}
		if (comp.accepted &&
			!rpt_apply_in_main(forward.vertices[forward.by_rti.at(comp.root)],
							   comp.channels, parents, params.width, &next_id, true))
		{
			comp.accepted = false;
			comp.reason = "root scan not found, not a SeqScan, or key not in its output";
		}
	}

	RptDumpForward(comps, trace);
	for (const RptForwardComponent &comp : comps)
		for (int id : comp.chain)
			if (est[id].quals_dropped)
				*trace += "forward: quals of " + std::to_string(id) +
					" dropped in the reducer (not provably error-free)\n";

	bool		any = false;

	for (const RptForwardComponent &comp : comps)
		any = any || comp.accepted;
	if (!materialize || !any)
		return stmt;

	next_id = RptPgCall([&]() { return AnserMaxPlanNodeId(work); }) + 1;

	List	   *children = NIL;

	for (const RptForwardComponent &comp : comps)
	{
		if (!comp.accepted)
			continue;

		for (int id : comp.chain)
		{
			const RptVertexInfo &v = forward.vertices[forward.by_rti.at(id)];
			std::vector<AttrNumber> attnos;

			for (const RptForwardChannel &ch : comp.channels)
			{
				AttrNumber	a = 0;

				if (ch.to == id)
					a = ch.to_key.attno;
				else if (ch.from == id)
					a = ch.from_key.attno;
				if (a != 0 && std::find(attnos.begin(), attnos.end(), a) == attnos.end())
					attnos.push_back(a);
			}

			bool		dropped = false;	/* reported with the dump */
			Plan	   *node = RptPgCall([&]() {
				return rpt_reducer_scan(v.scan, v.relid, attnos, &dropped, &next_id);
			});

			/* Consumers below producers: a filter is built from rows the
			 * inbound filters already reduced (design rule 4). */
			for (const RptForwardChannel &ch : comp.channels)
			{
				if (ch.to != id)
					continue;
				AttrNumber	pos = (AttrNumber) (std::find(attnos.begin(), attnos.end(),
														   ch.to_key.attno) - attnos.begin()) + 1;

				node = RptPgCall([&]() {
					if (!ch.in_process)
						return rpt_wrap_consumer(node, pos, ch, params.width, &next_id);
					CustomScan *cs = RptBuildLocalConsumerScan(node, pos, ch.condition_id,
															   ch.total_elems);

					cs->scan.plan.plan_node_id = next_id++;
					return (Plan *) cs;
				});
			}
			for (const RptForwardChannel &ch : comp.channels)
			{
				if (ch.from != id)
					continue;
				AttrNumber	pos = (AttrNumber) (std::find(attnos.begin(), attnos.end(),
														   ch.from_key.attno) - attnos.begin()) + 1;

				node = RptPgCall([&]() {
					if (!ch.in_process)
						return rpt_wrap_producer(node, pos, ch, params.width, &next_id);
					CustomScan *cs = RptBuildLocalProducerScan(node, pos, ch.condition_id,
															   ch.total_elems);

					cs->scan.plan.plan_node_id = next_id++;
					return (Plan *) cs;
				});
			}
			children = RptPgCall([&]() { return lappend(children, node); });
		}

		if (!rpt_apply_in_main(forward.vertices[forward.by_rti.at(comp.root)],
							   comp.channels, parents, params.width, &next_id, false))
			RptPgCall([]() { elog(ERROR, "RPT forward: root relink failed after preflight"); });

		if (rpt_forward_apply == RPT_FORWARD_APPLY_ALL)
			for (int id : comp.chain)
				(void) rpt_apply_in_main(forward.vertices[forward.by_rti.at(id)],
										 comp.channels, parents, params.width,
										 &next_id, false);
	}

	RptAttachReducerInitPlan(work, children, params.width);
	return work;
}
