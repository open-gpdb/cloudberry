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
 * rptinject.cpp
 *    Finished-plan construction of the distributed forward InitPlan.
 *
 *-------------------------------------------------------------------------
 */
extern "C"
{
#include "postgres.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "anserplan.h"
}

#include "rpt.h"

/* The caller owns a private copy of the statement. This function may throw;
 * publish that copy only after the entire injection pass has succeeded.
 * Children must be non-parallel reducer producers that return NULL at EOF.
 * The final empty Result makes EXISTS drain the Sequence completely.
 * NIL children builds the no-op shape used to measure dispatch overhead. */
void
RptAttachReducerInitPlan(PlannedStmt *stmt, List *children, int width)
{
	RptPgCall([&]() {
		if (!stmt->planTree || width <= 0 ||
			stmt->numSlices <= 0 || !stmt->slices ||
			(stmt->subplans != NIL && !stmt->subplan_sliceIds))
			elog(ERROR, "invalid RPT reducer InitPlan input");

		int max_id = AnserMaxPlanNodeId(stmt);
		ListCell *lc;
		foreach(lc, children)
		{
			PlannedStmt child_stmt = {};
			child_stmt.planTree = (Plan *) lfirst(lc);
			max_id = Max(max_id, AnserMaxPlanNodeId(&child_stmt));
		}
		int next_id = max_id + 1;
		int root_slice = stmt->numSlices;
		int sender_slice = root_slice + 1;
		int old_subplans = list_length(stmt->subplans);
		int param_id = list_length(stmt->paramExecTypes);
		Flow *distributed = makeNode(Flow);
		distributed->flotype = FLOW_PARTITIONED;
		distributed->locustype = CdbLocusType_Strewn;
		distributed->numsegments = width;
		Result *empty = makeNode(Result);
		empty->plan.plan_node_id = next_id++;
		empty->plan.flow = distributed;
		/* ExecInitResult passes this field to ExecInitQual as implicit AND. */
		empty->resconstantqual = (Node *) list_make1(makeBoolConst(false, false));
		Sequence *sequence = makeNode(Sequence);
		sequence->plan.plan_node_id = next_id++;
		sequence->plan.flow = distributed;
		sequence->subplans = lappend(list_copy(children), empty);
		Motion *gather = makeNode(Motion);
		gather->plan.plan_node_id = next_id++;
		gather->plan.lefttree = (Plan *) sequence;
		gather->motionType = MOTIONTYPE_GATHER;
		gather->motionID = sender_slice;
		Flow *singleton = makeNode(Flow);
		singleton->flotype = FLOW_SINGLETON;
		singleton->locustype = CdbLocusType_Entry;
		singleton->segindex = -1;
		singleton->numsegments = 1;
		gather->plan.flow = singleton;

		PlanSlice *slices = (PlanSlice *) palloc0(
			 sizeof(PlanSlice) * (stmt->numSlices + 2));
		memcpy(slices, stmt->slices, sizeof(PlanSlice) * stmt->numSlices);
		slices[root_slice].sliceIndex = root_slice;
		slices[root_slice].parentIndex = -1;
		slices[root_slice].gangType = GANGTYPE_UNALLOCATED;
		slices[root_slice].segindex = -1;
		slices[sender_slice].sliceIndex = sender_slice;
		slices[sender_slice].parentIndex = root_slice;
		slices[sender_slice].gangType = GANGTYPE_PRIMARY_READER;
		slices[sender_slice].segindex = 0;
		slices[sender_slice].numsegments = width;
		int *slice_ids = (int *) palloc(sizeof(int) * (old_subplans + 1));
		if (old_subplans)
			memcpy(slice_ids, stmt->subplan_sliceIds, sizeof(int) * old_subplans);
		slice_ids[old_subplans] = root_slice;

		SubPlan *subplan = makeNode(SubPlan);
		subplan->subLinkType = EXISTS_SUBLINK;
		subplan->plan_id = old_subplans + 1;
		subplan->plan_name = psprintf("InitPlan %d", subplan->plan_id);
		subplan->firstColType = VOIDOID;
		subplan->firstColTypmod = -1;
		subplan->is_initplan = true;
		subplan->setParam = list_make1_int(param_id);
		stmt->subplans = lappend(stmt->subplans, gather);
		stmt->paramExecTypes = lappend_oid(stmt->paramExecTypes, BOOLOID);
		stmt->planTree->initPlan = lappend(stmt->planTree->initPlan, subplan);
		stmt->subplan_sliceIds = slice_ids;
		stmt->slices = slices;
		stmt->numSlices += 2;
	});
}
