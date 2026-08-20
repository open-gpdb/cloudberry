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

	/*
	 * TODO(PR4 chunk 3): walk stmt->planTree, match the supported
	 * HashJoin(inner=Hash, outer=SeqScan) shape, and inject the producer /
	 * consumer CustomScan nodes.  Scaffolding only for now.
	 */
}
