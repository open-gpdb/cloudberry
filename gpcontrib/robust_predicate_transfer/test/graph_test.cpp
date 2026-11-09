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
 * graph_test.cpp
 *    Deterministic tests of the production graph, planner, resolver and
 *    PostgreSQL/C++ error boundary, using synthetic plans and C-call stubs.
 *
 * IDENTIFICATION
 *	  gpcontrib/robust_predicate_transfer/test/graph_test.cpp
 *
 *-------------------------------------------------------------------------
 */
/* Standalone tests of the production graph/plan code. PostgreSQL calls are
 * stubbed below; no server or optimizer choices affect these graph fixtures. */
#include "../src/rptgraph.cpp"
#include "../src/rptplan.cpp"
#include "../src/rptregion.cpp"

#include <cstdlib>
#include <new>
#include <cstdarg>
#undef vsnprintf

/* Unlike assert(), checks and their side effects remain active with NDEBUG. */
#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			fputs("check failed: " #condition "\n", stderr); \
			abort(); \
		} \
	} while (0)

extern "C" {
sigjmp_buf *PG_exception_stack = NULL;
ErrorContextCallback *error_context_stack = NULL;
int dispatch_nest_level = 0;
Oid getBaseType(Oid type) { return type; }
bool get_typbyval(Oid) { return true; }
TypeCacheEntry *lookup_type_cache(Oid, int) { abort(); }
GpPolicy *GpPolicyFetch(Oid) { abort(); }
void pfree(void *) { abort(); }
bool AnserIsRuntimeFilterScan(const Plan *) { return false; }

void ExceptionalCondition(const char *, const char *, int) { abort(); }
void exec_init_plan_tree_base(plan_tree_base_prefix *, PlannedStmt *) {}
bool plan_tree_walker(Node *node, bool (*walker)(Node *, void *), void *ctx, bool)
{
	Plan *plan = (Plan *) node;
	return walker((Node *) outerPlan(plan), ctx) ||
		walker((Node *) innerPlan(plan), ctx);
}
void appendStringInfo(StringInfo str, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	int count = vsnprintf(str->data + str->len, str->maxlen - str->len, format, args);
	va_end(args);
	CHECK(count >= 0 && count < str->maxlen - str->len);
	str->len += count;
}
void appendStringInfoString(StringInfo str, const char *text)
{
	appendStringInfo(str, "%s", text);
}
void appendStringInfoChar(StringInfo str, char ch)
{
	appendStringInfo(str, "%c", ch);
}
}
bool rpt_enable = false;
bool rpt_debug = false;
int rpt_max_chain_depth = 4;

static void vertex(RptTransferMgr &mgr, int id, int rows, bool filtered)
{
	RptVertexInfo v = {};
	v.id = id;
	v.cardinality = rows;
	v.filtered = filtered;
	mgr.by_rti[id] = mgr.vertices.size();
	mgr.vertices.push_back(v);
}

static RptEdgeInfo edge(int a, int b, bool protect_a = false,
						bool protect_b = false, int col_a = 1, int col_b = 1)
{
	RptEdgeInfo e = {};
	e.left_v = a;
	e.right_v = b;
	e.key_type = 23;
	e.protect_left = protect_a;
	e.protect_right = protect_b;
	e.clauses.push_back(std::make_pair(RptKey{a, (AttrNumber) col_a},
									 RptKey{b, (AttrNumber) col_b}));
	return e;
}

static void connect(RptTransferMgr &mgr, const RptEdgeInfo &e)
{
	rpt_upsert_edge(&mgr, e);
	mgr.vertices[mgr.by_rti[e.left_v]].join_keys.insert(e.clauses[0].first);
	mgr.vertices[mgr.by_rti[e.right_v]].join_keys.insert(e.clauses[0].second);
}

static void bypass_direction(bool reverse)
{
	RptTransferMgr mgr;
	vertex(mgr, 1, 100, true);
	vertex(mgr, 2, 10, true);
	vertex(mgr, 3, 50, false);
	/* 1 LEFT JOIN (2 JOIN 3): only 1 -> 3 -> 2 is legal. */
	connect(mgr, reverse ? edge(3, 1, false, true) : edge(1, 3, true));
	connect(mgr, reverse ? edge(3, 2) : edge(2, 3));
	mgr.vertices[2].unfiltered = true;
	rpt_skip_unfiltered(&mgr);
	const auto &e = mgr.neighbor_matrix.at(1).at(2);
	CHECK(rpt_can_flow(&e, 1, 2));
	CHECK(!rpt_can_flow(&e, 2, 1));
	CHECK(!rpt_can_flow(&mgr.neighbor_matrix.at(2).at(1), 2, 1));
}

static void protection_and_idempotence()
{
	RptTransferMgr mgr;
	CHECK(rpt_upsert_edge(&mgr, edge(1, 2, true)));
	CHECK(!rpt_upsert_edge(&mgr, edge(1, 2, true)));
	CHECK(!rpt_upsert_edge(&mgr, edge(2, 1)));
	CHECK(mgr.neighbor_matrix[1][2].protect_left);
	CHECK(rpt_upsert_edge(&mgr, edge(2, 1, true)));
	CHECK(mgr.neighbor_matrix[1][2].protect_right);
}

static RptTransferMgr star(int cap)
{
	RptTransferMgr mgr;
	rpt_max_chain_depth = cap;
	for (int id = 1; id <= 4; ++id)
		vertex(mgr, id, 100 / id, true);
	for (int id = 2; id <= 4; ++id)
		connect(mgr, edge(1, id));
	RptBuildClasses(&mgr);
	RptBuildTransferPlan(NULL, &mgr);
	return mgr;
}

static void asymmetric_shape()
{
	auto mgr = star(4);
	CHECK(mgr.groups.size() == 1);
	CHECK(mgr.transfer_order == std::vector<int>({1, 2, 3, 4}));
	CHECK(mgr.reducer_order == std::vector<int>({4, 3, 2}));
	for (int id = 2; id <= 4; ++id)
	{
		CHECK(mgr.graph[id].forward_stage.out.size() == 1);
		CHECK(mgr.graph[id].forward_stage.out[0].destination == id - 1);
		CHECK(mgr.graph[id].backward_stage.in.size() == 1);
		CHECK(mgr.graph[id].backward_stage.in[0].destination == 1);
	}
	CHECK(mgr.graph[1].forward_stage.out.empty());
	CHECK(mgr.graph[1].backward_stage.out.size() == 3);
	mgr = star(1);
	CHECK(mgr.graph[4].forward_stage.out.size() == 1);
	CHECK(mgr.graph[3].forward_stage.out.empty());
	CHECK(mgr.graph[2].forward_stage.in.empty());
}

static void unfiltered_cycle()
{
	RptTransferMgr mgr;
	for (int id = 1; id <= 4; ++id)
		vertex(mgr, id, 10, id == 1);
	connect(mgr, edge(1, 2));
	connect(mgr, edge(2, 3));
	connect(mgr, edge(3, 4));
	connect(mgr, edge(4, 2));
	RptBuildClasses(&mgr); /* Must terminate, with no unfiltered producers. */
	for (const auto &row : mgr.neighbor_matrix)
		for (const auto &entry : row.second)
			if (row.first != 1)
				CHECK(!rpt_can_flow(&entry.second, row.first, entry.first));
}

static void all_producers_required()
{
	RptTransferMgr mgr;
	Plan scans[3] = {};
	for (int id = 1; id <= 3; ++id)
	{
		vertex(mgr, id, 10, true);
		mgr.vertices.back().scan = &scans[id - 1];
	}
	HashJoin join = {};
	NodeSetTag(&join, T_HashJoin);
	outerPlan(&join) = &scans[0];
	innerPlan(&join) = &scans[1];
	PlannedStmt stmt = {};
	stmt.planTree = (Plan *) &join;
	RptGraphEdge in = {};
	in.destination = 2;
	mgr.graph[1].backward_stage.in.push_back(in);
	rpt_check_consumers(&stmt, &mgr);
	CHECK(!mgr.consumer_ok[1]);
	Motion build_motion = {};
	NodeSetTag(&build_motion, T_Motion);
	build_motion.motionID = 1;
	outerPlan(&build_motion) = &scans[1];
	innerPlan(&join) = (Plan *) &build_motion;
	rpt_check_consumers(&stmt, &mgr);
	CHECK(mgr.consumer_ok[1]);
	build_motion.motionID = -1;
	rpt_check_consumers(&stmt, &mgr);
	CHECK(!mgr.consumer_ok[1]);
	build_motion.motionID = 1;
	in.destination = 3; /* This producer is not ordered before the probe. */
	mgr.graph[1].backward_stage.in.push_back(in);
	rpt_check_consumers(&stmt, &mgr);
	CHECK(!mgr.consumer_ok[1]);
	mgr.graph[1].backward_stage.in[1].local = true;
	rpt_check_consumers(&stmt, &mgr);
	CHECK(mgr.consumer_ok[1]); /* The unsafe local edge does not wait. */
	mgr.graph[1].backward_stage.in[0].local = true;
	rpt_check_consumers(&stmt, &mgr);
	CHECK(!mgr.consumer_ok[1]); /* All-local means no blocking consumer. */
	for (auto &edge : mgr.graph[1].backward_stage.in)
		edge.local = false;
	/* Different hash joins can independently prove the two producers. */
	HashJoin upper = {};
	NodeSetTag(&upper, T_HashJoin);
	outerPlan(&upper) = (Plan *) &join;
	Motion upper_motion = {};
	NodeSetTag(&upper_motion, T_Motion);
	upper_motion.motionID = 2;
	outerPlan(&upper_motion) = &scans[2];
	innerPlan(&upper) = (Plan *) &upper_motion;
	stmt.planTree = (Plan *) &upper;
	rpt_check_consumers(&stmt, &mgr);
	CHECK(mgr.consumer_ok[1]);
}

static void bridges_and_locality()
{
	RptTransferMgr mgr;
	vertex(mgr, 1, 100, true);
	vertex(mgr, 2, 50, false);
	vertex(mgr, 3, 20, true);
	connect(mgr, edge(1, 2));
	connect(mgr, edge(2, 3, false, false, 2));
	RptBuildClasses(&mgr);
	rpt_max_chain_depth = 4;
	RptBuildTransferPlan(NULL, &mgr);
	CHECK(mgr.groups.size() == 2);
	CHECK(mgr.bridges == std::set<int>({2}));
	CHECK(mgr.reducer_order == std::vector<int>({3, 2}));
	for (auto &v : mgr.vertices)
	{
		v.numsegments = 3;
		v.distkeys = {1};
		v.distopclasses = {1978};
	}
	RptGraphEdge hop = {};
	hop.destination = 2;
	hop.left_keys = {{1, 1}};
	hop.right_keys = {{2, 1}};
	CHECK(rpt_local_hop(&mgr, 1, hop));
	mgr.vertices[1].numsegments = 2;
	CHECK(!rpt_local_hop(&mgr, 1, hop));
	mgr.vertices[1].numsegments = 3;
	mgr.vertices[1].distopclasses = {1};
	CHECK(!rpt_local_hop(&mgr, 1, hop));
	mgr.vertices[1].distopclasses = {1978};
	mgr.vertices[1].distkeys = {2};
	CHECK(!rpt_local_hop(&mgr, 1, hop));
}

static void hash_policy(RptVertexInfo &v, int segments = 3)
{
	v.numsegments = segments;
	v.distkeys = {1};
	v.distopclasses = {1978};
}

static void local_star_and_backward()
{
	auto mgr = star(4);
	for (auto &v : mgr.vertices)
		hash_policy(v);
	rpt_classify_hops(&mgr);
	int forward = 0;
	int backward = 0;
	for (const auto &node : mgr.graph)
	{
		for (const auto &hop : node.second.forward_stage.out)
		{
			++forward;
			CHECK(hop.local); /* Includes synthetic 4->3 and 3->2. */
			CHECK(mgr.graph.at(hop.destination).forward_stage.in[0].local);
		}
		for (const auto &hop : node.second.backward_stage.out)
		{
			++backward;
			CHECK(hop.local);
			CHECK(mgr.graph.at(hop.destination).backward_stage.in[0].local);
		}
	}
	CHECK(forward == 3 && backward == 3);
	char buffer[16384] = {};
	StringInfoData dump = {};
	dump.data = buffer;
	dump.maxlen = sizeof(buffer);
	RptDumpTransferPlan(&mgr, &dump);
	std::string text(buffer);
	CHECK(text.find("merge=global") == std::string::npos);
	CHECK(text.find("forward hop 3->2 merge=local\n") != std::string::npos);
	CHECK(text.find("forward hop 4->3 merge=local\n") != std::string::npos);
	CHECK(text.find("backward hop 1->4 merge=local\n") != std::string::npos);

	/* A class can mix local and global broadcast recipients. */
	mgr.vertices[mgr.by_rti[4]].numsegments = 2;
	rpt_classify_hops(&mgr);
	for (const auto &hop : mgr.graph[1].backward_stage.out)
		CHECK(hop.local == (hop.destination != 4));

	/* Bypassing an unfiltered table preserves policy-local transfer too. */
	RptTransferMgr bypass;
	vertex(bypass, 1, 100, true);
	vertex(bypass, 2, 10, true);
	vertex(bypass, 3, 50, false);
	for (auto &v : bypass.vertices)
		hash_policy(v);
	connect(bypass, edge(1, 3, true));
	connect(bypass, edge(2, 3));
	bypass.vertices[2].unfiltered = true;
	rpt_skip_unfiltered(&bypass);
	const auto &e = bypass.neighbor_matrix.at(1).at(2);
	RptGraphEdge hop = {};
	hop.destination = 2;
	hop.left_keys = {e.clauses[0].first};
	hop.right_keys = {e.clauses[0].second};
	CHECK(rpt_local_hop(&bypass, 1, hop));
}

static void replicated_and_compound_policies()
{
	RptTransferMgr mgr;
	vertex(mgr, 1, 100, true);
	vertex(mgr, 2, 10, true);
	for (auto &v : mgr.vertices)
		hash_policy(v);
	RptGraphEdge hop = {};
	hop.destination = 2;
	hop.left_keys = {{1, 1}};
	hop.right_keys = {{2, 1}};
	/* No neighbor-matrix edge is needed to prove policy locality. */
	CHECK(rpt_local_hop(&mgr, 1, hop));
	auto &src = mgr.vertices[0];
	auto &dst = mgr.vertices[1];
	src.distkeys = {1, 2};
	src.distopclasses = {1978, 1978};
	dst.distkeys = {1, 2};
	dst.distopclasses = {1978, 1978};
	CHECK(!rpt_local_hop(&mgr, 1, hop)); /* Missing a distribution column. */
	hop.left_keys.push_back({1, 2});
	hop.right_keys.push_back({2, 2});
	CHECK(rpt_local_hop(&mgr, 1, hop));
	std::swap(dst.distkeys[0], dst.distkeys[1]);
	CHECK(!rpt_local_hop(&mgr, 1, hop)); /* Hash order matters. */

	src.replicated = true;
	src.distkeys.clear();
	src.distopclasses.clear();
	CHECK(rpt_local_hop(&mgr, 1, hop));
	src.numsegments = 4;
	CHECK(rpt_local_hop(&mgr, 1, hop));
	src.numsegments = 2;
	CHECK(!rpt_local_hop(&mgr, 1, hop));
	src.numsegments = 3;
	dst.replicated = true;
	CHECK(!rpt_local_hop(&mgr, 1, hop)); /* Into replicated always global. */
	src.replicated = false;
	CHECK(!rpt_local_hop(&mgr, 1, hop));
	dst.replicated = false;
	CHECK(!rpt_local_hop(&mgr, 1, hop)); /* Random/unknown source. */
}

static void index_only_endpoint()
{
	IndexOnlyScan scan = {};
	NodeSetTag(&scan, T_IndexOnlyScan);
	scan.scan.scanrelid = 7;
	TargetEntry output = {};
	Var index_var = {};
	NodeSetTag(&index_var, T_Var);
	index_var.varno = INDEX_VAR;
	index_var.varattno = 1;
	output.expr = (Expr *) &index_var;
	Var base_var = {};
	NodeSetTag(&base_var, T_Var);
	base_var.varno = 7;
	base_var.varattno = 3;
	TargetEntry index_column = {};
	index_column.expr = (Expr *) &base_var;
	/* A one-cell PostgreSQL list, without requiring palloc in the driver. */
	ListCell out_cell = {};
	out_cell.ptr_value = &output;
	List out_list = {};
	out_list.type = T_List;
	out_list.length = out_list.max_length = 1;
	out_list.elements = &out_cell;
	ListCell index_cell = {};
	index_cell.ptr_value = &index_column;
	List index_list = out_list;
	index_list.elements = &index_cell;
	scan.scan.plan.targetlist = &out_list;
	scan.indextlist = &index_list;
	Plan *resolved = NULL;
	AttrNumber attno = 0;
	CHECK(rpt_resolve_key_scan((Plan *) &scan, 1, &resolved, &attno));
	CHECK(resolved == (Plan *) &scan && attno == 3);
}

static void join_protection()
{
	for (JoinType type : {JOIN_INNER, JOIN_SEMI, JOIN_LEFT, JOIN_RIGHT,
						  JOIN_FULL, JOIN_ANTI, JOIN_LASJ_NOTIN})
	{
		RptTransferMgr mgr;
		SeqScan scans[2] = {};
		Var vars[2] = {};
		TargetEntry entries[2] = {};
		ListCell cells[2] = {};
		List lists[2] = {};
		for (int i = 0; i < 2; ++i)
		{
			NodeSetTag(&scans[i], T_SeqScan);
			scans[i].scan.scanrelid = i + 1;
			scans[i].scan.plan.plan_rows = 100 - i;
			NodeSetTag(&vars[i], T_Var);
			vars[i].varno = i + 1;
			vars[i].varattno = 1;
			entries[i].expr = (Expr *) &vars[i];
			cells[i].ptr_value = &entries[i];
			lists[i].type = T_List;
			lists[i].length = lists[i].max_length = 1;
			lists[i].elements = &cells[i];
			scans[i].scan.plan.targetlist = &lists[i];
			scans[i].scan.plan.qual = &lists[i]; /* mark both scans filtered */
		}
		PlannedStmt stmt = {};
		rpt_add_edge(&stmt, &mgr, type, (Plan *) &scans[0], 1,
					 (Plan *) &scans[1], 1, 23);
		if (type == JOIN_FULL || type == JOIN_ANTI || type == JOIN_LASJ_NOTIN)
		{
			CHECK(mgr.vertices.empty() && mgr.neighbor_matrix.empty());
			continue;
		}
		const auto &e = mgr.neighbor_matrix.at(1).at(2);
		CHECK(e.protect_left == (type == JOIN_LEFT));
		CHECK(e.protect_right == (type == JOIN_RIGHT));
		RptBuildClasses(&mgr);
		RptBuildTransferPlan(NULL, &mgr);
		if (type == JOIN_LEFT || type == JOIN_RIGHT)
		{
			int preserved = type == JOIN_LEFT ? 1 : 2;
			CHECK(mgr.graph[preserved].forward_stage.in.empty());
			CHECK(mgr.graph[preserved].backward_stage.in.empty());
		}
	}
}

static void directed_parent_choice()
{
	RptTransferMgr mgr;
	for (int id = 1; id <= 3; ++id)
		vertex(mgr, id, 100 / id, true);
	std::set<int> constructed = {1, 2};
	std::set<int> remaining = {3};
	std::map<int, int> forward_depth = {{1, 0}, {2, 3}};
	std::map<int, int> backward_depth = forward_depth;
	int forward_from, backward_from, to;

	connect(mgr, edge(1, 3));
	connect(mgr, edge(2, 3, true)); /* deepest parent blocks 3 -> 2 */
	CHECK(rpt_find_edge(&mgr, constructed, remaining, forward_depth, backward_depth,
						&forward_from, &backward_from, &to));
	CHECK(to == 3 && forward_from == 1 && backward_from == 1);

	mgr.neighbor_matrix.clear();
	connect(mgr, edge(1, 3, false, true)); /* shallowest blocks 1 -> 3 */
	connect(mgr, edge(2, 3));
	CHECK(rpt_find_edge(&mgr, constructed, remaining, forward_depth, backward_depth,
						&forward_from, &backward_from, &to));
	CHECK(forward_from == 2 && backward_from == 2);

	mgr.neighbor_matrix.clear();
	RptEdgeInfo heavy = edge(1, 3, true);
	heavy.clauses.push_back(std::make_pair(RptKey{1, 2}, RptKey{3, 2}));
	connect(mgr, heavy);
	connect(mgr, edge(2, 3));
	CHECK(rpt_find_edge(&mgr, constructed, remaining, forward_depth, backward_depth,
						&forward_from, &backward_from, &to));
	CHECK(forward_from == 2 && backward_from == 1); /* lower-weight open edge */

	mgr.neighbor_matrix.clear();
	connect(mgr, edge(1, 3, true));
	connect(mgr, edge(2, 3, true));
	CHECK(rpt_find_edge(&mgr, constructed, remaining, forward_depth, backward_depth,
						&forward_from, &backward_from, &to));
	CHECK(forward_from == -1 && backward_from == 1);

	mgr.neighbor_matrix.clear();
	connect(mgr, edge(1, 3, false, true));
	connect(mgr, edge(2, 3, false, true));
	CHECK(rpt_find_edge(&mgr, constructed, remaining, forward_depth, backward_depth,
						&forward_from, &backward_from, &to));
	CHECK(forward_from == 2 && backward_from == -1);
}

static void error_unwinds()
{
	int destroyed = 0;
	struct Owner {
		int &count;
		~Owner() { ++count; }
	};
	try
	{
		Owner owner{destroyed};
		RptPgCall([]() { siglongjmp(*PG_exception_stack, 1); });
		abort();
	}
	catch (const RptPgError &) {}
	CHECK(destroyed == 1);
	CHECK(PG_exception_stack == NULL);
	/* A C++ callback exception must restore PG's exception stack too. */
	try
	{
		RptPgCall([]() { throw std::bad_alloc(); });
		abort();
	}
	catch (const std::bad_alloc &) {}
	CHECK(PG_exception_stack == NULL);
}

int main()
{
	bypass_direction(false);
	bypass_direction(true);
	protection_and_idempotence();
	asymmetric_shape();
	unfiltered_cycle();
	all_producers_required();
	bridges_and_locality();
	local_star_and_backward();
	replicated_and_compound_policies();
	index_only_endpoint();
	join_protection();
	directed_parent_choice();
	error_unwinds();
	return 0;
}
