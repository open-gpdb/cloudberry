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
 * inject_test.cpp
 *    Deterministic tests of the production graph, planner, resolver and
 *    PostgreSQL/C++ error boundary, using synthetic plans and C-call stubs.
 *
 * IDENTIFICATION
 *	  gpcontrib/robust_predicate_transfer/test/inject_test.cpp
 *
 *-------------------------------------------------------------------------
 */
#include "../src/rptinject.cpp"
#include "../src/rptforward.cpp"
#include "../src/rptbackwardplan.cpp"
#include "../src/rptbackward.cpp"
#include <cstdlib>
#include <cstdarg>
#include <set>
#include <vector>
#undef vsnprintf
#undef fprintf
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)

static std::vector<void *> allocations;
struct BuiltFilter
{
	bool producer;
	Plan *child;
	AttrNumber attno;
	uint32 condition_id;
	int processes;
	bool defer_first;
};
static std::vector<BuiltFilter> built_filters;

extern "C" {
sigjmp_buf *PG_exception_stack = NULL;
ErrorContextCallback *error_context_stack = NULL;
MemoryContext CurrentMemoryContext = NULL;
int dispatch_nest_level = 0;
void *palloc(Size n) { void *p = malloc(n); CHECK(p); allocations.push_back(p); return p; }
void *palloc0(Size n) { void *p = palloc(n); memset(p, 0, n); return p; }
void *MemoryContextAllocZero(MemoryContext, Size n) { return palloc0(n); }
void *MemoryContextAllocZeroAligned(MemoryContext, Size n) { return palloc0(n); }
bool errstart_cold(int, const char *) { abort(); }
void ExceptionalCondition(const char *, const char *, int) { abort(); }
bool errstart(int, const char *) { abort(); }
void errfinish(const char *, int, const char *) { abort(); }
void errmsg_internal(const char *, ...) { abort(); }
char *psprintf(const char *fmt, ...)
{
	char *p = (char *) palloc(128);
	va_list args; va_start(args, fmt); vsnprintf(p, 128, fmt, args); va_end(args);
	return p;
}
static List *append_cell(List *list, NodeTag tag, ListCell cell)
{
	int n = list_length(list);
	List *out = (List *) palloc0(sizeof(List));
	out->type = tag; out->length = out->max_length = n + 1;
	out->elements = (ListCell *) palloc(sizeof(ListCell) * (n + 1));
	if (n) memcpy(out->elements, list->elements, sizeof(ListCell) * n);
	out->elements[n] = cell;
	return out;
}
List *list_make1_impl(NodeTag tag, ListCell c) { return append_cell(NIL, tag, c); }
List *lappend(List *l, void *p) { return append_cell(l, T_List, list_make_ptr_cell(p)); }
List *lappend_oid(List *l, Oid o) { return append_cell(l, T_OidList, list_make_oid_cell(o)); }
List *list_copy(const List *l)
{
	List *out = NIL;
	for (int i = 0; i < list_length(l); ++i) out = append_cell(out, l->type, l->elements[i]);
	return out;
}
Node *makeBoolConst(bool value, bool isnull)
{
	Const *c = makeNode(Const); c->consttype = BOOLOID; c->constvalue = BoolGetDatum(value);
	c->constisnull = isnull; return (Node *) c;
}
static int max_plan_node_id(Plan *p)
{
	if (!p) return 0;
	int m = Max(p->plan_node_id, Max(max_plan_node_id(p->lefttree), max_plan_node_id(p->righttree)));
	if (IsA(p, Sequence))
	{
		ListCell *lc;
		foreach(lc, ((Sequence *) p)->subplans) m = Max(m, max_plan_node_id((Plan *) lfirst(lc)));
	}
	return m;
}
int AnserSliceProducers(PlannedStmt *, int slice_index)
{
	return slice_index >= 0 ? 3 : 0;
}
CustomScan *AnserBuildBloomProducerScan(Plan *child, AttrNumber attno, uint32 condition_id,
	const char *, int64, Size, int64, int processes)
{
	built_filters.push_back({true, child, attno, condition_id, processes, false});
	CustomScan *cs = makeNode(CustomScan);
	cs->custom_plans = list_make1(child);
	return cs;
}
CustomScan *AnserBuildBloomConsumerScan(Plan *child, AttrNumber attno, uint32 condition_id,
	const char *, int64, Size, int64, int processes, bool defer_first)
{
	built_filters.push_back({false, child, attno, condition_id, processes, defer_first});
	CustomScan *cs = makeNode(CustomScan);
	cs->custom_plans = list_make1(child);
	return cs;
}
int AnserMaxPlanNodeId(PlannedStmt *s)
{
	int m = max_plan_node_id(s->planTree); ListCell *lc;
	foreach(lc, s->subplans) m = Max(m, max_plan_node_id((Plan *) lfirst(lc)));
	return m;
}
}

static void collect_plan_node_ids(Plan *p, std::set<int> &plan_node_ids)
{
	if (!p) return;
	CHECK(plan_node_ids.insert(p->plan_node_id).second);
	collect_plan_node_ids(p->lefttree, plan_node_ids); collect_plan_node_ids(p->righttree, plan_node_ids);
	if (IsA(p, Sequence))
	{
		ListCell *lc;
		foreach(lc, ((Sequence *) p)->subplans) collect_plan_node_ids((Plan *) lfirst(lc), plan_node_ids);
	}
}

static void layout(bool existing, bool noop)
{
	PlannedStmt stmt = {};
	Result main = {}; main.plan.type = T_Result; main.plan.plan_node_id = 5;
	stmt.planTree = &main.plan;
	PlanSlice initial[2] = {};
	initial[0].sliceIndex = 0; initial[0].parentIndex = -1;
	initial[1].sliceIndex = 1; initial[1].parentIndex = -1;
	stmt.slices = initial; stmt.numSlices = existing ? 2 : 1;
	Result prior = {}; prior.plan.type = T_Result; prior.plan.plan_node_id = 90;
	int prior_slice = 1;
	SubPlan old_init = {};
	if (existing)
	{
		stmt.subplans = list_make1(&prior);
		stmt.subplan_sliceIds = &prior_slice;
		stmt.paramExecTypes = list_make1_oid(INT4OID);
		stmt.planTree->initPlan = list_make1(&old_init);
	}
	Result child = {}; child.plan.type = T_Result; child.plan.plan_node_id = 110;
	List *children = noop ? NIL : list_make1(&child);
	int old_slices = stmt.numSlices;
	RptAttachReducerInitPlan(&stmt, children, 8);
	CHECK(stmt.numSlices == old_slices + 2);
	CHECK(memcmp(stmt.slices, initial, sizeof(PlanSlice) * old_slices) == 0);
	for (int i = 0; i < stmt.numSlices; ++i) CHECK(stmt.slices[i].sliceIndex == i);
	PlanSlice &r = stmt.slices[old_slices], &s = stmt.slices[old_slices + 1];
	CHECK(r.parentIndex == -1 && r.gangType == GANGTYPE_UNALLOCATED && r.segindex == -1);
	CHECK(s.parentIndex == old_slices && s.gangType == GANGTYPE_PRIMARY_READER);
	CHECK(s.numsegments == 8 && s.segindex == 0 && s.parallel_workers == 0);
	CHECK(!s.directDispatch.isDirectDispatch && s.directDispatch.contentIds == NIL);
	CHECK(list_length(stmt.subplans) == (existing ? 2 : 1));
	CHECK(list_length(stmt.paramExecTypes) == (existing ? 2 : 1));
	CHECK(llast_oid(stmt.paramExecTypes) == BOOLOID);
	CHECK(list_length(stmt.planTree->initPlan) == (existing ? 2 : 1));
	if (existing)
	{
		CHECK(stmt.subplan_sliceIds[0] == 1 && linitial(stmt.subplans) == &prior);
		CHECK(linitial_oid(stmt.paramExecTypes) == INT4OID);
		CHECK(linitial(stmt.planTree->initPlan) == &old_init);
	}
	SubPlan *sp = (SubPlan *) llast(stmt.planTree->initPlan);
	CHECK(sp->is_initplan && sp->subLinkType == EXISTS_SUBLINK);
	CHECK(sp->plan_id == list_length(stmt.subplans));
	CHECK(list_length(sp->setParam) == 1 && linitial_int(sp->setParam) == (existing ? 1 : 0));
	CHECK(sp->parParam == NIL && sp->args == NIL);
	CHECK(stmt.subplan_sliceIds[sp->plan_id - 1] == old_slices);
	Motion *motion = (Motion *) llast(stmt.subplans);
	CHECK(IsA(motion, Motion) && motion->motionID == old_slices + 1);
	CHECK(motion->motionType == MOTIONTYPE_GATHER);
	CHECK(motion->plan.flow->flotype == FLOW_SINGLETON);
	Sequence *seq = (Sequence *) motion->plan.lefttree;
	CHECK(IsA(seq, Sequence) && list_length(seq->subplans) == (noop ? 1 : 2));
	CHECK(list_length(children) == (noop ? 0 : 1));
	if (!noop) CHECK(linitial(seq->subplans) == &child);
	CHECK(seq->plan.flow->flotype == FLOW_PARTITIONED && seq->plan.flow->numsegments == 8);
	Result *empty = (Result *) llast(seq->subplans);
	CHECK(IsA(empty, Result) && IsA(empty->resconstantqual, List));
	List *quals = (List *) empty->resconstantqual;
	CHECK(list_length(quals) == 1 && IsA(linitial(quals), Const));
	Const *qual = (Const *) linitial(quals);
	CHECK(qual->consttype == BOOLOID && !qual->constisnull);
	CHECK(!DatumGetBool(qual->constvalue));
	std::set<int> plan_node_ids;
	collect_plan_node_ids(stmt.planTree, plan_node_ids);
	ListCell *lc; foreach(lc, stmt.subplans) collect_plan_node_ids((Plan *) lfirst(lc), plan_node_ids);
	CHECK(plan_node_ids.size() == (existing ? 6U : 5U) - (noop ? 1U : 0U));
	int previous_max = noop ? (existing ? 90 : 5) : 110;
	CHECK(empty->plan.plan_node_id > previous_max &&
		  seq->plan.plan_node_id > previous_max && motion->plan.plan_node_id > previous_max);
}
static void backward_stacks(bool refused, bool materialize, bool with_forward)
{
	RptTransferMgr schedule;
	SeqScan scans[2] = {};
	TargetEntry entries[2] = {};
	Var keys[2] = {};
	Hash hash = {}; hash.plan.type = T_Hash;
	Motion motion = {}; motion.plan.type = T_Motion;
	HashJoin join = {}; join.join.plan.type = T_HashJoin;
	join.join.plan.righttree = &hash.plan;
	join.join.plan.lefttree = &motion.plan;
	PlannedStmt stmt = {}; stmt.type = T_PlannedStmt;
	stmt.planTree = &join.join.plan;
	PlanSlice slices[3] = {}; stmt.slices = slices; stmt.numSlices = 3;
	for (int i = 0; i < 2; ++i)
	{
		scans[i].scan.plan.type = T_SeqScan;
		scans[i].scan.scanrelid = i + 1;
		keys[i].xpr.type = T_Var;
		keys[i].varno = i + 1; keys[i].varattno = 2;
		entries[i].xpr.type = T_TargetEntry; entries[i].expr = (Expr *) &keys[i];
		entries[i].resno = 1; /* physical attno differs from output position */
		scans[i].scan.plan.targetlist = list_make1(&entries[i]);
		RptVertexInfo vertex = {};
		vertex.rtindex = i + 1; vertex.scan = &scans[i].scan.plan;
		vertex.cardinality = i == 0 ? 1000 : 10000;
		schedule.vertex_index_by_rtindex[i + 1] = i;
		schedule.vertices.push_back(vertex);
	}
	hash.plan.lefttree = &scans[0].scan.plan;
	motion.plan.lefttree = &scans[1].scan.plan;
	schedule.root_rtindexes.insert(1);
	RptBackwardConsumer decision;
	decision.class_key = decision.producer_key = {1, 2};
	decision.consumer_key = {2, 2};
	decision.producer_rtindex = 1; decision.consumer_rtindex = 2;
	decision.producer_parent = &hash.plan; decision.consumer_parent = &motion.plan;
	decision.producer_attno = decision.consumer_attno = 1;
	decision.producer_slice_index = 1; decision.consumer_slice_index = 2;
	decision.eligible = true; decision.defer_first = true;
	decision.estimated_rows_removed = 9000;
	schedule.backward_consumers.push_back(decision);
	/* A second class on the same scans exercises repeated stacking/relinking. */
	decision.class_key.attno = 3;
	schedule.backward_consumers.push_back(decision);
	RptInjectionState state;
	state.parents[&scans[0].scan.plan] = &hash.plan;
	state.parents[&scans[1].scan.plan] = &motion.plan;
	for (auto &scan : scans) state.tops[&scan.scan.plan] = &scan.scan.plan;
	state.next_plan_node_id = 100;
	std::vector<RptForwardComponent> forward_components(1);
	forward_components[0].accepted = true;
	for (int i = 0; i < 7; ++i)
	{
		RptForwardChannel channel;
		channel.source_rtindex = i + 1; channel.target_rtindex = i + 2;
		forward_components[0].channels.push_back(channel);
	}
	rpt_assign_forward_channels(forward_components, state);
	CHECK(state.next_condition_id == 7);
	for (int i = 0; i < 7; ++i)
		CHECK(forward_components[0].channels[i].condition_id == uint32(i));
	CustomScan forward[2] = {};
	if (with_forward)
		for (int i = 0; i < 2; ++i)
		{
			forward[i].scan.plan.type = T_CustomScan;
			forward[i].custom_plans = list_make1(&scans[i]);
			RptRelinkStack(&state, &scans[i].scan.plan, &forward[i].scan.plan);
		}
	if (refused)
		schedule.backward_consumers[0].consumer_attno =
			schedule.backward_consumers[1].consumer_attno = 99;
	Hash hash_before = hash; Motion motion_before = motion;
	auto tops_before = state.tops;
	PlanSlice slices_before[3]; memcpy(slices_before, slices, sizeof(slices));
	size_t allocations_before = allocations.size();
	built_filters.clear();
	RptForwardParams params; params.max_exchange_ms = -1;
	std::string trace;
	bool injected = false;
	RptInjectBackward(&stmt, schedule, {}, params,
		[](double, int64 *elems, int64 *payload, int64 *bytes) {
			*elems = 1000; *payload = *bytes = 1024; return true;
		}, materialize, &state, &trace, &injected);
	CHECK(stmt.slices == slices && stmt.numSlices == 3);
	CHECK(memcmp(slices_before, slices, sizeof(slices)) == 0);
	CHECK(stmt.subplans == NIL && stmt.paramExecTypes == NIL);
	if (refused || !materialize)
	{
		CHECK(!injected && built_filters.empty());
		CHECK(memcmp(&hash_before, &hash, sizeof(hash)) == 0);
		CHECK(memcmp(&motion_before, &motion, sizeof(motion)) == 0);
		CHECK(state.tops == tops_before && allocations.size() == allocations_before);
		CHECK(state.next_plan_node_id == 100);
		CHECK(trace.find(refused ? "placement changed" : "decision=inject") != std::string::npos);
	}
	else
	{
		CHECK(injected && built_filters.size() == 4);
		CHECK(state.next_condition_id == 9 && state.next_plan_node_id == 104);
		for (int i = 0; i < 2; ++i)
		{
			CHECK(built_filters[2*i].producer && !built_filters[2*i+1].producer);
			CHECK(built_filters[2*i].condition_id == uint32(7+i));
			CHECK(built_filters[2*i+1].condition_id == uint32(7+i));
			CHECK(built_filters[2*i].processes == 3 && built_filters[2*i+1].defer_first);
			CHECK(built_filters[2*i].attno == 1 && built_filters[2*i+1].attno == 1);
		}
		Plan *producer = hash.plan.lefttree, *consumer = motion.plan.lefttree;
		CHECK(producer == state.tops.at(&scans[0].scan.plan));
		CHECK(consumer == state.tops.at(&scans[1].scan.plan));
		CHECK(producer->plan_node_id == 102 && consumer->plan_node_id == 103);
		producer = (Plan *) linitial(((CustomScan *) producer)->custom_plans);
		consumer = (Plan *) linitial(((CustomScan *) consumer)->custom_plans);
		CHECK(producer->plan_node_id == 100 && consumer->plan_node_id == 101);
		CHECK(linitial(((CustomScan *) producer)->custom_plans) == tops_before.at(&scans[0].scan.plan));
		CHECK(linitial(((CustomScan *) consumer)->custom_plans) == tops_before.at(&scans[1].scan.plan));
	}
}

int main()
{
	backward_stacks(false, true, true);
	backward_stacks(false, true, false);
	backward_stacks(true, true, true);
	backward_stacks(false, false, true);
	layout(false, false); layout(true, false);
	layout(false, true); layout(true, true);
	for (void *p : allocations) free(p);
	return 0;
}
