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
 * anserplanexec.c
 *	  CustomScan providers that carry the Anser runtime bloom filter through
 *	  the executor: a pass-through "producer" above the hash build input that
 *	  observes the build join key and publishes a bloom filter, and a
 *	  pass-through "consumer" above the probe scan that prunes rows whose key is
 *	  definitely absent from the received (unioned) filter.
 *
 * Both are thin drivers over the PR2 helper library
 * (executor/nodeAnserBloomFilter.h), which already selects transport by role
 * (segment -> libpq to QD; coordinator -> direct shmem) and unions parts.  The
 * plan-injection pass (anserplan.c) builds the nodes and stashes the parameters
 * in CustomScan.custom_private using the layout in AnserRfPrivateIndex below.
 *
 * IDENTIFICATION
 *	  src/backend/cdb/anser/anserplanexec.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/anser.h"
#include "cdb/anserfilter.h"
#include "cdb/anserplan.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "executor/nodeAnserBloomFilter.h"
#include "lib/bloomfilter.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/value.h"

/*
 * Positional layout of CustomScan.custom_private shared with anserplan.c.
 * Integer nodes except the last, which is a String.  Both providers read the
 * same list (each ignores fields it does not need).
 */
typedef enum AnserRfPrivateIndex
{
	ANSER_RF_PRIV_CONDITION_ID = 0,		/* Integer: channel condition_id */
	ANSER_RF_PRIV_KEY_ATTNO,			/* Integer: build/probe key attno */
	ANSER_RF_PRIV_TOTAL_ELEMS,			/* Integer: bloom sizing (producer) */
	ANSER_RF_PRIV_MAX_PAYLOAD,			/* Integer: bloom sizing (producer) */
	ANSER_RF_PRIV_PLANNED_BYTES,		/* Integer: planned bitset bytes (EXPLAIN) */
	ANSER_RF_PRIV_CONDITION_KEY,		/* String:  channel condition_key */
	ANSER_RF_PRIV__COUNT
} AnserRfPrivateIndex;

/* Registration-timeout for the consumer's wait-for-registration phase. */
#define ANSER_RF_REGISTRATION_TIMEOUT_MS	((long) gp_anser_timeout_ms)

typedef struct AnserBloomProduceScanState
{
	CustomScanState csstate;
	AnserBloomFilterProduceState *produce;
	AttrNumber	key_attno;
	int64		planned_bytes;
	bool		published;
} AnserBloomProduceScanState;

typedef struct AnserBloomConsumeScanState
{
	CustomScanState csstate;
	AnserBloomFilterConsumeState *consume;
	bloom_filter *filter;		/* NULL => fail open (pass everything) */
	AttrNumber	key_attno;
	int64		planned_bytes;
	bool		received;		/* have we run the receive/union yet? */
} AnserBloomConsumeScanState;

/* Provider callbacks. */
static Node *anser_produce_create_state(CustomScan *cscan);
static void anser_produce_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *anser_produce_exec(CustomScanState *node);
static void anser_produce_end(CustomScanState *node);
static void anser_produce_rescan(CustomScanState *node);
static void anser_produce_explain(CustomScanState *node, List *ancestors,
								  ExplainState *es);

static Node *anser_consume_create_state(CustomScan *cscan);
static void anser_consume_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *anser_consume_exec(CustomScanState *node);
static void anser_consume_end(CustomScanState *node);
static void anser_consume_rescan(CustomScanState *node);
static void anser_consume_explain(CustomScanState *node, List *ancestors,
								  ExplainState *es);

static const CustomScanMethods anser_produce_scan_methods =
{
	.CustomName = "Anser Bloom Producer",
	.CreateCustomScanState = anser_produce_create_state,
};

static const CustomScanMethods anser_consume_scan_methods =
{
	.CustomName = "Anser Bloom Consumer",
	.CreateCustomScanState = anser_consume_create_state,
};

static const CustomExecMethods anser_produce_exec_methods =
{
	.CustomName = "Anser Bloom Producer",
	.BeginCustomScan = anser_produce_begin,
	.ExecCustomScan = anser_produce_exec,
	.EndCustomScan = anser_produce_end,
	.ReScanCustomScan = anser_produce_rescan,
	.ExplainCustomScan = anser_produce_explain,
};

static const CustomExecMethods anser_consume_exec_methods =
{
	.CustomName = "Anser Bloom Consumer",
	.BeginCustomScan = anser_consume_begin,
	.ExecCustomScan = anser_consume_exec,
	.EndCustomScan = anser_consume_end,
	.ReScanCustomScan = anser_consume_rescan,
	.ExplainCustomScan = anser_consume_explain,
};

void
AnserRegisterRuntimeFilterMethods(void)
{
	RegisterCustomScanMethods(&anser_produce_scan_methods);
	RegisterCustomScanMethods(&anser_consume_scan_methods);
}

/* ---- node builders (called from the injection pass in anserplan.c) ---- */

/*
 * Identity output targetlist for a pass-through CustomScan: a Var per child
 * column referencing the scan tuple via INDEX_VAR, so ExecScan projects the
 * child tuple through unchanged.  (Post-setrefs we build this by hand rather
 * than relying on set_customscan_references.)
 */
static List *
anser_rf_identity_tlist(List *child_tlist)
{
	List	   *tlist = NIL;
	ListCell   *lc;
	AttrNumber	attno = 0;

	foreach(lc, child_tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Var		   *var;

		attno++;
		var = makeVar(INDEX_VAR, attno,
					  exprType((Node *) tle->expr),
					  exprTypmod((Node *) tle->expr),
					  exprCollation((Node *) tle->expr),
					  0);
		tlist = lappend(tlist,
						makeTargetEntry((Expr *) var, attno,
										tle->resname ? pstrdup(tle->resname) : NULL,
										tle->resjunk));
	}

	return tlist;
}

static CustomScan *
anser_build_rf_scan(const CustomScanMethods *methods, Plan *child,
					AttrNumber key_attno, uint32 condition_id,
					const char *condition_key, int64 total_elems,
					Size max_payload_bytes, int64 planned_bytes)
{
	CustomScan *cs = makeNode(CustomScan);
	List	   *priv = NIL;

	/* custom_private, in AnserRfPrivateIndex order. */
	priv = lappend(priv, makeInteger((int) condition_id));
	priv = lappend(priv, makeInteger((int) key_attno));
	priv = lappend(priv, makeInteger((int) total_elems));
	priv = lappend(priv, makeInteger((int) max_payload_bytes));
	priv = lappend(priv, makeInteger((int) planned_bytes));
	priv = lappend(priv, makeString(pstrdup(condition_key)));

	cs->scan.plan.targetlist = anser_rf_identity_tlist(child->targetlist);
	cs->scan.plan.qual = NIL;
	cs->scan.plan.lefttree = NULL;
	cs->scan.plan.righttree = NULL;
	cs->scan.plan.startup_cost = child->startup_cost;
	cs->scan.plan.total_cost = child->total_cost;
	cs->scan.plan.plan_rows = child->plan_rows;
	cs->scan.plan.plan_width = child->plan_width;
	cs->scan.plan.parallel_aware = false;
	cs->scan.plan.parallel_safe = child->parallel_safe;
	cs->scan.plan.flow = (Flow *) copyObject(child->flow);
	cs->scan.scanrelid = 0;
	cs->flags = 0;
	cs->custom_plans = list_make1(child);
	cs->custom_exprs = NIL;
	cs->custom_private = priv;
	cs->custom_scan_tlist = copyObject(child->targetlist);
	cs->methods = methods;

	return cs;
}

CustomScan *
AnserBuildBloomProducerScan(Plan *child, AttrNumber key_attno,
							uint32 condition_id, const char *condition_key,
							int64 total_elems, Size max_payload_bytes,
							int64 planned_bytes)
{
	return anser_build_rf_scan(&anser_produce_scan_methods, child, key_attno,
							   condition_id, condition_key, total_elems,
							   max_payload_bytes, planned_bytes);
}

CustomScan *
AnserBuildBloomConsumerScan(Plan *child, AttrNumber key_attno,
							uint32 condition_id, const char *condition_key,
							int64 total_elems, Size max_payload_bytes,
							int64 planned_bytes)
{
	return anser_build_rf_scan(&anser_consume_scan_methods, child, key_attno,
							   condition_id, condition_key, total_elems,
							   max_payload_bytes, planned_bytes);
}

/* ---- shared helpers ---- */

static void
anser_rf_build_key(CustomScan *cscan, AnserChannelKey *key)
{
	List	   *priv = cscan->custom_private;
	int			condition_id = intVal(list_nth(priv, ANSER_RF_PRIV_CONDITION_ID));
	char	   *condition_key = strVal(list_nth(priv, ANSER_RF_PRIV_CONDITION_KEY));

	MemSet(key, 0, sizeof(*key));
	key->gp_session_id = gp_session_id;
	key->gp_command_count = gp_command_count;
	key->condition_id = (uint32) condition_id;
	strlcpy(key->condition_key, condition_key, ANSER_CONDITION_KEY_SIZE);
}

/*
 * Number of producing segments for this slice, and this backend's part index.
 * On a segment the slice runs on the whole gang; on the coordinator the filter
 * is produced locally as a single part.
 */
static void
anser_rf_part_info(uint32 *part_index, uint32 *total_parts)
{
	if (Gp_role == GP_ROLE_EXECUTE)
	{
		*part_index = (uint32) GpIdentity.segindex;
		*total_parts = (uint32) getgpsegmentCount();
	}
	else
	{
		*part_index = 0;
		*total_parts = 1;
	}
}

/* Move the child tuple into our scan slot (positional pass-through). */
static TupleTableSlot *
anser_rf_child_slot(CustomScanState *node, TupleTableSlot *childslot)
{
	ExecCopySlot(node->ss.ss_ScanTupleSlot, childslot);
	return node->ss.ss_ScanTupleSlot;
}

static bool
anser_rf_recheck(CustomScanState *node, TupleTableSlot *slot)
{
	return true;
}

/* ---- producer ---- */

static Node *
anser_produce_create_state(CustomScan *cscan)
{
	AnserBloomProduceScanState *st = (AnserBloomProduceScanState *)
		newNode(sizeof(AnserBloomProduceScanState), T_CustomScanState);

	st->csstate.methods = &anser_produce_exec_methods;
	return (Node *) st;
}

static void
anser_produce_begin(CustomScanState *node, EState *estate, int eflags)
{
	AnserBloomProduceScanState *st = (AnserBloomProduceScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *priv = cscan->custom_private;
	Plan	   *child = (Plan *) linitial(cscan->custom_plans);
	AnserChannelKey key;
	uint32		part_index;
	uint32		total_parts;
	int64		total_elems = intVal(list_nth(priv, ANSER_RF_PRIV_TOTAL_ELEMS));
	Size		max_payload = (Size) intVal(list_nth(priv, ANSER_RF_PRIV_MAX_PAYLOAD));

	st->key_attno = (AttrNumber) intVal(list_nth(priv, ANSER_RF_PRIV_KEY_ATTNO));
	st->planned_bytes = intVal(list_nth(priv, ANSER_RF_PRIV_PLANNED_BYTES));
	st->published = false;

	anser_rf_build_key(cscan, &key);
	anser_rf_part_info(&part_index, &total_parts);

	st->produce = ExecInitAnserBloomFilterProduce(&key, total_elems, max_payload,
												  part_index, total_parts);

	node->custom_ps = list_make1(ExecInitNode(child, estate, eflags));
}

/*
 * Pass-through that feeds the build key of every child tuple into the filter,
 * and publishes once the child is exhausted.
 */
static TupleTableSlot *
anser_produce_next(CustomScanState *node)
{
	AnserBloomProduceScanState *st = (AnserBloomProduceScanState *) node;
	PlanState  *child = (PlanState *) linitial(node->custom_ps);
	TupleTableSlot *slot = ExecProcNode(child);

	if (TupIsNull(slot))
	{
		if (!st->published)
		{
			(void) ExecAnserBloomFilterProducePublish(st->produce);
			st->published = true;
		}
		return NULL;
	}

	if (st->produce != NULL)
	{
		bool		isnull;
		Datum		value = slot_getattr(slot, st->key_attno, &isnull);

		ExecAnserBloomFilterProduceAddDatum(st->produce, value, isnull);
	}

	return anser_rf_child_slot(node, slot);
}

static TupleTableSlot *
anser_produce_exec(CustomScanState *node)
{
	return ExecScan(&node->ss,
					(ExecScanAccessMtd) anser_produce_next,
					(ExecScanRecheckMtd) anser_rf_recheck);
}

static void
anser_produce_end(CustomScanState *node)
{
	AnserBloomProduceScanState *st = (AnserBloomProduceScanState *) node;

	if (st->produce != NULL)
		ExecEndAnserBloomFilterProduce(st->produce);
	st->produce = NULL;
	if (node->custom_ps != NIL)
		ExecEndNode((PlanState *) linitial(node->custom_ps));
}

static void
anser_produce_rescan(CustomScanState *node)
{
	if (node->custom_ps != NIL)
		ExecReScan((PlanState *) linitial(node->custom_ps));
}

static void
anser_produce_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	AnserBloomProduceScanState *st = (AnserBloomProduceScanState *) node;

	ExplainPropertyInteger("Bloom Filter Size", "bytes", st->planned_bytes, es);
}

/* ---- consumer ---- */

static Node *
anser_consume_create_state(CustomScan *cscan)
{
	AnserBloomConsumeScanState *st = (AnserBloomConsumeScanState *)
		newNode(sizeof(AnserBloomConsumeScanState), T_CustomScanState);

	st->csstate.methods = &anser_consume_exec_methods;
	return (Node *) st;
}

static void
anser_consume_begin(CustomScanState *node, EState *estate, int eflags)
{
	AnserBloomConsumeScanState *st = (AnserBloomConsumeScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *priv = cscan->custom_private;
	Plan	   *child = (Plan *) linitial(cscan->custom_plans);
	AnserChannelKey key;
	uint32		part_index;
	uint32		expected_parts;

	st->key_attno = (AttrNumber) intVal(list_nth(priv, ANSER_RF_PRIV_KEY_ATTNO));
	st->planned_bytes = intVal(list_nth(priv, ANSER_RF_PRIV_PLANNED_BYTES));
	st->filter = NULL;
	st->received = false;

	anser_rf_build_key(cscan, &key);
	anser_rf_part_info(&part_index, &expected_parts);

	st->consume = ExecInitAnserBloomFilterConsume(&key, expected_parts);

	node->custom_ps = list_make1(ExecInitNode(child, estate, eflags));
}

/*
 * On first call, block to receive and union the bloom parts.  A NULL filter
 * (channel cancelled / unreachable / feature degraded) means fail open: pass
 * every row through unfiltered.
 */
static void
anser_consume_receive(AnserBloomConsumeScanState *st)
{
	if (st->received)
		return;
	st->received = true;

	if (st->consume == NULL)
		return;

	if (ExecAnserBloomFilterConsume(st->consume, ANSER_RF_REGISTRATION_TIMEOUT_MS))
		st->filter = ExecAnserBloomFilterConsumerGetFilter(st->consume);
	else
		st->filter = NULL;

	/*
	 * Diagnostic: a NULL filter means we fail open (no pruning).  Log why --
	 * cancelled delivery vs. too few bloom parts unioned -- so runtime-filter
	 * misbehavior is visible in the server log.
	 */
	if (st->filter == NULL)
		elog(LOG,
			 "anser bloom consumer: no filter, failing open (cancelled=%d, received_parts=%u)",
			 ExecAnserBloomFilterConsumerWasCancelled(st->consume),
			 ExecAnserBloomFilterConsumerReceivedParts(st->consume));
}

static TupleTableSlot *
anser_consume_next(CustomScanState *node)
{
	AnserBloomConsumeScanState *st = (AnserBloomConsumeScanState *) node;
	PlanState  *child = (PlanState *) linitial(node->custom_ps);

	anser_consume_receive(st);

	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(child);
		bool		isnull;
		Datum		value;

		if (TupIsNull(slot))
			return NULL;

		/* Fail open: no usable filter -> pass everything. */
		if (st->filter == NULL)
			return anser_rf_child_slot(node, slot);

		value = slot_getattr(slot, st->key_attno, &isnull);

		/* NULLs never match an equijoin key; let the join handle them. */
		if (!isnull &&
			bloom_lacks_element(st->filter, (unsigned char *) &value,
								sizeof(Datum)))
		{
			/* Definitely absent from the build side -> prune. */
			InstrCountFiltered2(node, 1);
			continue;
		}

		return anser_rf_child_slot(node, slot);
	}
}

static TupleTableSlot *
anser_consume_exec(CustomScanState *node)
{
	return ExecScan(&node->ss,
					(ExecScanAccessMtd) anser_consume_next,
					(ExecScanRecheckMtd) anser_rf_recheck);
}

static void
anser_consume_end(CustomScanState *node)
{
	AnserBloomConsumeScanState *st = (AnserBloomConsumeScanState *) node;

	if (st->consume != NULL)
		ExecEndAnserBloomFilterConsume(st->consume);
	st->consume = NULL;
	st->filter = NULL;
	if (node->custom_ps != NIL)
		ExecEndNode((PlanState *) linitial(node->custom_ps));
}

static void
anser_consume_rescan(CustomScanState *node)
{
	if (node->custom_ps != NIL)
		ExecReScan((PlanState *) linitial(node->custom_ps));
}

static void
anser_consume_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	AnserBloomConsumeScanState *st = (AnserBloomConsumeScanState *) node;

	ExplainPropertyInteger("Bloom Filter Size", "bytes", st->planned_bytes, es);

	if (es->analyze && node->ss.ps.instrument != NULL)
	{
		double		nloops = node->ss.ps.instrument->nloops;
		double		nfiltered = node->ss.ps.instrument->nfiltered2;

		if (nloops > 0)
			ExplainPropertyFloat("Rows Removed by Bloom Filter", NULL,
								 nfiltered / nloops, 0, es);
	}
}
