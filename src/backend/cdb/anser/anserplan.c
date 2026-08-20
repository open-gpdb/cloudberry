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
 * anserplan.c
 *	  Post-planning transformation that injects Anser runtime bloom-filter
 *	  producer/consumer CustomScan nodes into a finished plan tree.
 *
 * The pass runs once from planner() (after both the Postgres planner and ORCA,
 * and after set_plan_references / the cdbllize slice passes), recognizes one
 * supported join shape, and inserts a producer on the hash build side and a
 * consumer above the probe scan.  See src/include/cdb/anserplan.h and the PR4
 * design notes for the full rationale.
 *
 * IDENTIFICATION
 *	  src/backend/cdb/anser/anserplan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/anser.h"
#include "cdb/anserplan.h"
#include "cdb/cdbvars.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "utils/lsyscache.h"

/* Runtime-filter bloom size clamp (realized bitset bytes). */
#define ANSER_RF_MIN_BYTES		(8 * 1024)
#define ANSER_RF_MAX_BYTES		(16 * 1024 * 1024)
#define ANSER_RF_HEADER_ROOM	64

typedef struct AnserInjectCtx
{
	uint32		next_condition_id;
	int			next_plan_node_id;
} AnserInjectCtx;

static int	anser_max_plan_node_id(Plan *plan);
static bool anser_rf_size(double est_rows, int64 *total_elems,
						  int64 *max_payload, int64 *planned_bytes);
static bool anser_hashjoin_keys(HashJoin *hj, AttrNumber *inner_attno,
								AttrNumber *outer_attno);
static void anser_try_inject(HashJoin *hj, AnserInjectCtx *ctx);
static void anser_inject_walk(Plan *plan, AnserInjectCtx *ctx);

void
AnserApplyRuntimeFilters(PlannedStmt *stmt)
{
	/*
	 * Opt-in and coordinator-only: the pass runs on the QD where the whole
	 * PlannedStmt is available, and only when the operator has enabled the
	 * Anser subsystem and the runtime-filter feature.  Anything else is left
	 * completely untouched.
	 */
	if (!gp_anser_enable || !gp_anser_runtime_filter)
		return;
	if (Gp_role != GP_ROLE_DISPATCH)
		return;
	if (stmt == NULL || stmt->commandType != CMD_SELECT || stmt->planTree == NULL)
		return;

	{
		AnserInjectCtx ctx;
		ListCell   *lc;
		int			maxid;

		/*
		 * Injected nodes need plan_node_ids unique across the whole statement.
		 * set_plan_references already numbered every existing node, so continue
		 * past the current maximum (planTree + subplans).
		 */
		maxid = anser_max_plan_node_id(stmt->planTree);
		foreach(lc, stmt->subplans)
			maxid = Max(maxid, anser_max_plan_node_id((Plan *) lfirst(lc)));

		ctx.next_condition_id = 0;
		ctx.next_plan_node_id = maxid + 1;

		anser_inject_walk(stmt->planTree, &ctx);
	}
}

/*
 * Largest plan_node_id in a plan subtree.  Recurses the spine plus CustomScan
 * children; sufficient for the supported (simple) plan shape.
 */
static int
anser_max_plan_node_id(Plan *plan)
{
	int			m;

	if (plan == NULL)
		return 0;

	m = plan->plan_node_id;
	m = Max(m, anser_max_plan_node_id(outerPlan(plan)));
	m = Max(m, anser_max_plan_node_id(innerPlan(plan)));
	if (IsA(plan, CustomScan))
	{
		ListCell   *lc;

		foreach(lc, ((CustomScan *) plan)->custom_plans)
			m = Max(m, anser_max_plan_node_id((Plan *) lfirst(lc)));
	}

	return m;
}

/*
 * Choose a bloom bitset size (a power of two, clamped to [8 KB, 16 MB] and to
 * the server payload cap) from the estimated build cardinality.  Returns the
 * total_elems / max_payload_bytes to hand the producer helper so it realizes
 * exactly `planned_bytes` (see the PR4 sizing note).
 */
static bool
anser_rf_size(double est_rows, int64 *total_elems, int64 *max_payload,
			  int64 *planned_bytes)
{
	int64		cap;
	int64		cap_pow2;
	int64		t;
	double		target;

	cap = Min((int64) ANSER_RF_MAX_BYTES,
			  (int64) gp_anser_max_info_size - ANSER_RF_HEADER_ROOM);
	if (cap < ANSER_RF_MIN_BYTES)
		return false;			/* server payload cap too small to bother */

	cap_pow2 = ANSER_RF_MIN_BYTES;
	while ((cap_pow2 << 1) <= cap)
		cap_pow2 <<= 1;

	target = (est_rows > 0 ? est_rows : 1.0) * 2.0;
	t = ANSER_RF_MIN_BYTES;
	while (t < target && t < cap_pow2)
		t <<= 1;

	*total_elems = t / 2;
	*max_payload = t + ANSER_RF_HEADER_ROOM;
	*planned_bytes = t;
	return true;
}

/*
 * Match a single-column equijoin over plain (by-value) Vars and return the
 * inner (build) and outer (probe) key attnos.  After set_plan_references the
 * operands are INNER_VAR / OUTER_VAR references into the join's child tlists.
 */
static bool
anser_hashjoin_keys(HashJoin *hj, AttrNumber *inner_attno, AttrNumber *outer_attno)
{
	OpExpr	   *op;
	Node	   *l;
	Node	   *r;
	Var		   *outer_var;
	Var		   *inner_var;

	if (list_length(hj->hashclauses) != 1)
		return false;
	op = (OpExpr *) linitial(hj->hashclauses);
	if (!IsA(op, OpExpr) || list_length(op->args) != 2)
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
		outer_var = (Var *) l;
		inner_var = (Var *) r;
	}
	else if (((Var *) l)->varno == INNER_VAR && ((Var *) r)->varno == OUTER_VAR)
	{
		outer_var = (Var *) r;
		inner_var = (Var *) l;
	}
	else
		return false;

	/*
	 * Producer and consumer hash the raw Datum bytes, which is only meaningful
	 * for a pass-by-value key type (the value lives in the Datum, not behind a
	 * pointer).  Restrict to those.
	 */
	if (!get_typbyval(inner_var->vartype))
		return false;

	*inner_attno = inner_var->varattno;
	*outer_attno = outer_var->varattno;
	return true;
}

/*
 * If this HashJoin is the supported shape, inject a producer on the hash build
 * input and a consumer above the probe SeqScan.
 */
static void
anser_try_inject(HashJoin *hj, AnserInjectCtx *ctx)
{
	Plan	   *hash = innerPlan(hj);	/* build side */
	Plan	   *probe = outerPlan(hj);	/* probe side */
	Plan	   *build_input;
	AttrNumber	inner_attno;
	AttrNumber	outer_attno;
	int64		total_elems;
	int64		max_payload;
	int64		planned_bytes;
	uint32		condition_id;
	char		condition_key[ANSER_CONDITION_KEY_SIZE];
	CustomScan *producer;
	CustomScan *consumer;

	if (hj->join.jointype != JOIN_INNER && hj->join.jointype != JOIN_RIGHT)
		return;
	if (hash == NULL || !IsA(hash, Hash))
		return;
	if (probe == NULL || !IsA(probe, SeqScan))
		return;

	build_input = outerPlan(hash);
	if (build_input == NULL)
		return;

	if (!anser_hashjoin_keys(hj, &inner_attno, &outer_attno))
		return;
	if (!anser_rf_size(hash->plan_rows, &total_elems, &max_payload, &planned_bytes))
		return;

	condition_id = ctx->next_condition_id++;
	snprintf(condition_key, sizeof(condition_key), "anser_rf_%u", condition_id);

	/* Producer wraps the hash build input; keyed by the inner (build) attno. */
	producer = AnserBuildBloomProducerScan(build_input, inner_attno, condition_id,
										   condition_key, total_elems, max_payload,
										   planned_bytes);
	producer->scan.plan.plan_node_id = ctx->next_plan_node_id++;
	outerPlan(hash) = (Plan *) producer;

	/* Consumer wraps the probe scan; keyed by the outer (probe) attno. */
	consumer = AnserBuildBloomConsumerScan(probe, outer_attno, condition_id,
										   condition_key, total_elems, max_payload,
										   planned_bytes);
	consumer->scan.plan.plan_node_id = ctx->next_plan_node_id++;
	outerPlan(hj) = (Plan *) consumer;
}

/*
 * Manual in-place traversal (recurse spine + custom_plans).  We do not use
 * plan_tree_mutator, whose CustomScan arm does not descend into custom_plans.
 */
static void
anser_inject_walk(Plan *plan, AnserInjectCtx *ctx)
{
	if (plan == NULL)
		return;

	if (IsA(plan, HashJoin))
		anser_try_inject((HashJoin *) plan, ctx);

	anser_inject_walk(outerPlan(plan), ctx);
	anser_inject_walk(innerPlan(plan), ctx);
	if (IsA(plan, CustomScan))
	{
		ListCell   *lc;

		foreach(lc, ((CustomScan *) plan)->custom_plans)
			anser_inject_walk((Plan *) lfirst(lc), ctx);
	}
}
