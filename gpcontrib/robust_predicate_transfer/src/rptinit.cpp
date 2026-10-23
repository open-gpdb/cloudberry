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
 * rptinit.cpp
 *	  Module entry point: GUCs, the planner hook, and the SQL-visible
 *	  transfer-plan dump function.
 *
 * The extension rides on Anser (gpcontrib/anser), which must be preloaded
 * first: the AnserXxx entry points we call are resolved at load time from
 * the global symbol table (dfmgr loads modules RTLD_GLOBAL).
 *
 * Phase I is analysis-only: the planner hook computes the transfer plan and
 * logs it at DEBUG1 when rpt.debug is on, and rpt_transfer_plan() exposes
 * the same dump to SQL for the regression tests.  Nothing is injected into
 * the plan tree.
 *
 * IDENTIFICATION
 *	  gpcontrib/robust_predicate_transfer/src/rptinit.cpp
 *
 *-------------------------------------------------------------------------
 */
extern "C"
{
#include "postgres.h"

#include "cdb/cdbvars.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(rpt_transfer_plan);

void		_PG_init(void);
}

#include "rpt.h"

bool		rpt_enable = false;
bool		rpt_debug = false;
int			rpt_max_chain_depth = 4;

static planner_hook_type prev_planner_hook = NULL;

static void rpt_define_gucs(void);
static PlannedStmt *rpt_planner(Query *parse, const char *query_string,
								int cursorOptions, ParamListInfo boundParams,
								OptimizerOptions *optimizer_options);

/*
 * Run the Phase I analysis over a finished statement and return the dump.
 * The manager is plain C++ in the caller's memory context; on any error the
 * whole context goes away with the statement.
 */
static void
rpt_analyze(PlannedStmt *stmt, StringInfo out)
{
	RptTransferMgr mgr;

	RptCollectGraph(stmt, &mgr);
	if (mgr.vertices.empty())
	{
		appendStringInfoString(out, "no transfer regions\n");
		return;
	}

	RptBuildClasses(&mgr);
	RptBuildTransferPlan(stmt, &mgr);
	RptDumpTransferPlan(&mgr, out);
}

void
_PG_init(void)
{
	rpt_define_gucs();

	/*
	 * Only a preloaded library can be relied on to have installed its hooks
	 * in every backend.  Loaded any other way the extension stays inert: the
	 * GUCs exist (so a stray setting is not an error) and the dump function
	 * works, but nothing is wired into planning.
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	prev_planner_hook = planner_hook;
	planner_hook = rpt_planner;

	MarkGUCPrefixReserved("rpt");
}

/*
 * The extension's GUCs are "rpt.*"-qualified because they belong to a
 * loadable module; "anser" is already reserved by the extension we ride on.
 */
static void
rpt_define_gucs(void)
{
	DefineCustomBoolVariable("rpt.enable",
							 "Enables RPT+ transfer-plan analysis over Anser.",
							 "When disabled the planner hook is a pass-through.",
							 &rpt_enable,
							 false,
							 PGC_USERSET,
							 GUC_GPDB_NEED_SYNC,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("rpt.debug",
							 "Logs the RPT+ transfer plan of every analyzed query.",
							 "Dumps regions, equivalence classes and the forward/backward transfer plan at DEBUG1.",
							 &rpt_debug,
							 false,
							 PGC_USERSET,
							 GUC_GPDB_NEED_SYNC,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("rpt.max_chain_depth",
							"Sets the maximum forward-chain length RPT+ may plan.",
							"Every chain hop is a global publish/merge/deliver barrier in MPP, so the chain depth is capped; the default is a placeholder until hop latency is measured.",
							&rpt_max_chain_depth,
							4, 1, INT_MAX,
							PGC_USERSET,
							GUC_GPDB_NEED_SYNC,
							NULL, NULL, NULL);
}

/*
 * Plan as usual, then run the analysis over the finished tree.  Chaining
 * prev first means that when Anser is preloaded before us its injection pass
 * has already run on the tree we analyze, which is what we want: the region
 * discovery sees through Anser's own filter nodes.
 */
static PlannedStmt *
rpt_planner(Query *parse, const char *query_string, int cursorOptions,
			ParamListInfo boundParams, OptimizerOptions *optimizer_options)
{
	PlannedStmt *result;

	if (prev_planner_hook)
		result = prev_planner_hook(parse, query_string, cursorOptions,
								   boundParams, optimizer_options);
	else
		result = standard_planner(parse, query_string, cursorOptions,
								  boundParams, optimizer_options);

	if (rpt_enable && rpt_debug &&
		Gp_role == GP_ROLE_DISPATCH &&
		result != NULL && result->commandType == CMD_SELECT &&
		result->planTree != NULL)
	{
		StringInfoData buf;

		initStringInfo(&buf);
		rpt_analyze(result, &buf);
		elog(DEBUG1, "rpt transfer plan:\n%s", buf.data);
	}

	return result;
}

/*
 * rpt_transfer_plan(query text) -> text
 *
 * Plan the query and dump its RPT+ transfer plan.  Analysis only: the plan
 * is discarded afterwards.  This is the regression-test driver.
 */
Datum
rpt_transfer_plan(PG_FUNCTION_ARGS)
{
	char	   *query_string = text_to_cstring(PG_GETARG_TEXT_PP(0));
	List	   *raw_parsetree_list;
	List	   *query_list;
	Query	   *query;
	PlannedStmt *stmt;
	StringInfoData buf;

	raw_parsetree_list = pg_parse_query(query_string);
	if (list_length(raw_parsetree_list) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("rpt_transfer_plan expects a single statement")));

	query_list = pg_analyze_and_rewrite_fixedparams(
										 (RawStmt *) linitial(raw_parsetree_list),
										 query_string, NULL, 0, NULL);
	if (list_length(query_list) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("rpt_transfer_plan expects a single query")));

	query = (Query *) linitial(query_list);
	if (query->commandType != CMD_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("rpt_transfer_plan supports only SELECT")));

	/*
	 * pg_plan_query runs the hook chain, so with anser preloaded and its
	 * runtime_filter GUC on the tree we analyze already carries Anser's
	 * injected nodes -- exactly what production planning will hand us.
	 */
	stmt = pg_plan_query(query, query_string, 0, NULL);

	initStringInfo(&buf);
	rpt_analyze(stmt, &buf);

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}
