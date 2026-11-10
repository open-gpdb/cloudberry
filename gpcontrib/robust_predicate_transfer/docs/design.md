# Robust Predicate Transfer (RPT+) over Anser — design

The `robust_predicate_transfer` extension plans *transfer schedules* for the
bloom filters Anser exchanges: instead of one filter per hash join, the query's
join graph gets a forward pass that chains filters table-to-table and a
backward pass that broadcasts one merged filter per equivalence class — the
scheme of Qiao, Boncz and Zhang, *Robust Predicate Transfer with Dynamic
Execution* (PVLDB 19(6):1278-1290, 2026), ported from the paper's artifact
(`embryo-labs/dynamic-predicate-transfer`, MIT license, Copyright 2018-2025
Stichting DuckDB Foundation; see `licenses/LICENSE-mit-duckdb-rpt.txt`).

**Phase I (this code) is a pure analysis pass.** It runs over a finished
`PlannedStmt`, discovers transfer regions, builds the join graph and
equivalence classes, and computes the asymmetric transfer plan. Nothing is
injected or executed; `rpt_transfer_plan(text)` dumps the result, and the
planner hook logs the same dump at `DEBUG1` when `rpt.debug` is on.

## Architecture at a glance

```
Phase II candidate — one statement, one dispatch (survey gate pending)
   class-root scan: produce the global class broadcast filter
   eligible global members: consume it, with bounded prefetch
   local members: core runtime-filter pushdown only

Phase III candidate — inside that same statement (not implemented)
   Sequence(reducer Append, main plan); no cross-command filter cache
```

Phase I answers the question everything downstream depends on: given this
plan, what are the regions, the classes, the chain order, the broadcast roots,
and which consumers may safely wait on the broadcast.

## Transfer regions

A transfer region is a connected component of the join graph over base scans:
two scans are connected when an equijoin clause between them resolves on both
sides to a column of that scan.  The resolution (rptregion.cpp) descends the
operators that pass a column through unchanged — Hash, Motion, the three join
types, Sort, Material, and Anser's own filter nodes — and terminates only at
SeqScan, IndexScan, IndexOnlyScan or BitmapHeapScan, the scans a reducer pass
could replay.  Everything else is a region boundary: Agg, WindowAgg, SetOp,
RecursiveUnion, Append/MergeAppend, ProjectSet (that is what `UNNEST` becomes,
so the inject-before-or-after-unnest question is moot — filters never cross
it), SubqueryScan, CteScan, Unique, Limit, Result, DynamicSeqScan.

Because region membership falls out of the graph, a clause whose operands do
not resolve to base scans simply produces no edge, and the scans it would have
connected land in different regions.  This is a correctness gate, not a missed
optimization: a filter built on a scan that does not hold the whole key column
would drop rows that can join.

## Join graph and equivalence classes

Edges come from HashJoin hashclauses, MergeJoin mergeclauses, and unparameterized
col=col clauses of NestLoop joinquals.  A clause qualifies only when both
sides are plain Vars of the same type, the type is by-value, is not float, and
the operator is the type's own equality (rule 6 below).  Clauses of one join
over one vertex pair merge into one edge; the clause count is the edge weight.

Union-find over the keys (rtindex, attno) gives the equivalence classes; the
ports of the artifact's union-find, `ClassifyTables` (filtered / intermediate /
unfiltered) and `SkipUnfilteredTable` (bypass unfiltered tables with direct
edges, block their outgoing edges) are in rptgraph.cpp.

## Asymmetric transfer plan

rptplan.cpp uses one Algorithm 1 insertion loop for both trees. Frontier
selection prefers the largest shared-column count, then the largest relation;
each stage chooses among edges open in its transfer direction; among its
highest-weight open edges, parents use greatest forward depth and smallest
backward depth,
with vertex id making remaining ties deterministic. Unprotected equivalence
classes expose their implied equalities, so a SQL star and a SQL chain can
produce the same chain within a class. Root selection retains the artifact's
largest filtered-or-intermediate preference, falling back to the largest table.

The backward stage redirects each class to its earliest-inserted member. A
non-component-root class leader is a **bridge table**, reported in the dump.
Only filtered or bridge tables can produce forward filters. Component roots
are final destinations in the forward graph, but never reducer scans: they
would consume their inbound chain filter in the main query in Phase III. `reducer order` lists
only scans that actually produce forward filters, in dependency order.

`rpt.max_chain_depth` limits the number of dependency hops, including the
last delivery to the root. When another hop would exceed the cap, that edge
is omitted from both endpoint lists. This loses selectivity, not correctness.
The dump records the cap, reducer order, bridges, and each forward and
backward hop's merge mode. The default (4) remains provisional pending measured transport latency.

### C/C++ errors and diagnostic access

STL containers allocate outside PostgreSQL memory contexts. Calls that can
raise PostgreSQL errors go through `RptPgCall`, which catches the longjmp at
the call site and throws a C++ marker. C++ unwinding destroys the containers
before the analysis boundary rethrows the original PostgreSQL error. The
wrapper restores both PostgreSQL exception stacks on either kind of unwind.
C++ allocation and other exceptions are converted to PostgreSQL errors only
after leaving the C++ catch blocks. Both the analysis and SQL entry points
contain C++ exceptions.

The dump function plans without executing the query's executor permission
checks. Its default PUBLIC execution privilege is revoked; administrators
should grant it only to roles allowed to inspect relation estimates.

### Scope and validation still outstanding

This implementation computes schedules and dumps them; it does not inject
filters, dispatch reducers, or add EXPLAIN annotations. No runtime behavior
or performance improvement is claimed. The Phase I latency benchmark and
JOB/TPC-DS co-location coverage report have not been run. Multi-class runtime
backward chaining remains deferred to later phases. Cloudberry MARK-like
subplan forms are not DuckDB MARK joins: unsupported join kinds contribute
no transfer edge. SQL tests run each case with optimizer off and on;
standalone synthetic tests pin graph shape and error cleanup independently
of SQL optimizer transformations.

## Correctness rules

Each of these is a silent-wrong-answer rule, not a performance rule.  A bloom
filter may only ever *over*-approximate the build side's key set.

1. **Merge where the source keys are complete.** Decide each forward and
   backward edge from the base relations' distribution policies and that
   edge's key pairs. Equal ordered hash distribution columns, hash opclasses
   and segment counts permit a segment-local transfer when every distribution
   column is covered by the transfer equality. Main-plan Motions do not change
   this proof: the reducer reads the base scans. Implied equivalence-class
   and unfiltered-bypass edges use exactly the same policy test as original
   join edges, so a co-distributed star remains local after becoming a chain.
   A replicated source is complete on every covered segment: it may transfer
   locally if its segment count covers the destination's. A replicated scan
   may execute on only one segment: Phase II must place the producer on every
   consuming segment or classify that broadcast as global; policy coverage
   alone does not guarantee producer execution coverage. A transfer into a
   replicated destination stays global, including replicated-to-replicated.
   Other cases, including unknown policies and random-to-hash transfers, need
   global publish → merge → deliver. A local hop never publishes to the QD.
2. **Snapshot identity.** The reducer (Phase III) must run under the main
   query's snapshot and parameters: a row visible to the query but not to the
   reducer would be silently missing from the filter.
3. **Outer joins are directed edges.** LEFT protects the left side, RIGHT and
   MARK protect the right in the artifact; unsupported Cloudberry join kinds,
   ANTI and FULL produce no edge.  The preserved side
   of an outer join must never be reduced by the nullable side's keys.
4. **Transitive reduction is sound by induction.** Filtering table *t* with
   filter A before building filter B from *t* is correct because A only
   removes rows that provably cannot join.  This licenses the forward chain.
5. **Cycles cost completeness, not correctness.** The spanning-forest loop
   runs on cyclic queries, losing the full-reduction guarantee but never
   correctness (the RPT paper's position, which we follow).
6. **Key equality must be bitwise Datum equality.** Anser hashes raw Datum
   bytes, so a clause qualifies only when both sides share a by-value,
   non-float type and the operator is that type's own equality
   (`-0.0 = 0.0` is true but not bitwise equal; cross-type Datums for equal
   values are not bitwise equal either).
7. **Blocking consumers only across slices.** `consumer_ok` describes
   eligibility for a blocking Anser consumer on **global** backward edges.
   Every global producer must be on a hash join's build side, the consumer
   on its probe side, and the scans in different slices; different ancestors
   can prove different producers. Same-slice exchanges are refused, matching
   Anser. Local edges never participate in this check, and a vertex with only
   local in-edges reports `consumer_ok no`: it needs no blocking consumer.
   Core's `gp_enable_runtime_filter_pushdown` covers eligible same-process
   HashJoin/SeqScan pairs after building the hash table. Phase II should rely
   on that path for local backward edges, or use a non-blocking local filter
   whose absence means no filtering. A HashJoin can fetch its first probe
   tuple before starting the build; waiting there can deadlock even if the
   producer is on its build side. A local edge must never become an Anser
   exchange merely because its consumer passes the structural hash-join test.
8. **Fail open, always.** Timeout, malformed part, oversized filter, squelched
   producer, missing cache entry — all degrade to "no filter", never to an
   error and never to a filtered result. The optional planner-hook analysis
   runs in a disposable memory context: analysis failures warn and return the
   original finished plan. User cancellation still propagates. The explicit
   diagnostic UDF reports errors to its caller rather than masking them.

## Runtime contract with Anser (Phases II/III)

- Phase II has no reducer: its root builds from its own predicates only,
  inside the main statement. Runtime injection is gated by the survey below.
- Anser transport is reused, while its per-join injection is disabled. Plans
  carrying Anser per-join nodes are left untouched rather than mixed.
- Global backward edges may use Anser only where strict eligibility permits
  it. Local members use core runtime-filter pushdown; RPT injects no local
  consumers, QD channels or blocking waits for them.
- Channels are statement-scoped. The broadcast is delivered once per
  consuming QE; fan-out is capped per class. There is no session filter cache.
- A future forward reducer must run inside the same statement and dispatch,
  preserving the snapshot, parameters and Anser command/channel identity.

### Parallel workers and segment completeness

`merge=local` is a distribution-policy decision, not permission to use a
single parallel worker's partial filter. With parallel scans, each QE worker
reads only part of its segment's keys. Even a replicated source then needs
all workers' parts combined before its filter is complete on that segment.

Phase III's initial reducer must be **non-parallel**, with one scanning QE
per segment. Supporting parallel reducers requires a segment-local merge of
all worker filters before delivery or probing. Anser's `docs/local-fold.md`
describes such a rendezvous, but it is an idea, not an implemented facility;
Phase III must not assume it exists. Missing or failed worker parts must
abandon the filter and fail open, never expose an incomplete filter. The
same completeness requirement applies to any custom local backward producer.
Core currently disables its pushdown path in MPP parallel mode, so relying
on core there simply means no local filter, rather than a global exchange.

## Phase II eligibility survey gate (in progress)

The agreed next phase is backward broadcast inside the main statement. It
uses no session-scoped cache and injects no local filters: local edges stay
with core pushdown. Anser transport must be enabled and Anser's per-join
injection disabled; the two injection strategies are alternatives. A class
led by a bridge is deferred. Runtime injection remains **unimplemented**
until Work item 0 of `plans/rpt-plus-phase2-plan.md` has been completed and
its result justifies proceeding.

The analysis now records a decision for each class/member pair. In addition
to different slices and a producer on a hash-build side, it rejects
multi-tuple prefetch paths and nullable-side crossings. If the qualifying
join builds first (`prefetch_inner` or RIGHT), the prefetch blocker is lifted;
the shared Anser key resolver must still accept the path. This conservatively
rejects unsupported resolver shapes even when scheduling alone is safe.
Every placement under `stmt->subplans` is refused, even below a Motion and
even for an uncorrelated InitPlan, matching Anser's conservative scope. A
non-empty build-side `extParam` is also refused regardless of the parameter
source; publication has no rescan generation protocol yet. Successful decisions record
parents, output-tuple key positions, slice IDs and `defer_first`; physical
column numbers are never assumed to be tuple positions.

`consumer_ok` is an aggregate over all non-local decisions for a vertex.
Per-edge lines report refusals separately, including local edges, bridge-led
classes, unsupported key paths and the fan-out cap. The `survey` line counts
classes, component-root-led classes, global class/member pairs, structural
rule-9 passes, strict passes before the cap, and eligible consumers after it.
NDV estimates come from `pg_statistic`, with negative fractions scaled by
`pg_class.reltuples`. Their intersection estimate assumes uniform key overlap,
caps root NDV by estimated filtered root rows, and reports missing statistics
as unknown. Estimated removed rows use consumer scan-plan-row units, not a
cluster-total or a correctness claim. A table in multiple classes can be
counted more than once. Unknown-benefit consumers are ranked by their scan
row count as an upper-bound fallback when applying the per-class cap.

Run the planning-only survey after installing this analysis build and loading
benchmark schemas, data and statistics. `rpt_transfer_plan(text)` has EXECUTE
revoked from PUBLIC, so connect as a superuser or a role explicitly granted
EXECUTE on that function (and SELECT on the benchmark tables). For example,
an administrator can grant `EXECUTE ON FUNCTION public.rpt_transfer_plan(text)
TO rpt_survey`; adjust the function schema if installed elsewhere:

```sh
python3 scripts/survey.py \
  --job /bench/job/queries --job-dbname imdb \
  --tpch /bench/tpch/queries --tpch-dbname tpch \
  --tpcds /bench/tpcds/queries --tpcds-dbname tpcds \
  --output /tmp/rpt-phase2-survey
```

Normalize the benchmark input before running the gate:

- Use instantiated SQL with the benchmark's original parameters, not templates.
- TPC-DS templates 14, 23, 24 and 39 each contain two SELECT statements. The
  runner splits them automatically and records `filename#1` / `filename#2`;
  alternatively supply separate `14a.sql` / `14b.sql`, etc. Both layouts
  represent **103 SELECT statements from 99 templates**.
- Rewrite TPC-H Q15's `CREATE VIEW revenue... AS ...; SELECT ...; DROP VIEW ...`
  as `WITH revenue... AS (...) SELECT ...;`, preserving its view column names,
  SELECT body and date parameters. The runner rejects view DDL instead of
  executing it or silently excluding Q15. The normalized suite has 22 SELECTs.
- Statement splitting respects quoted strings, identifiers, dollar quotes
  and nested comments. The server still parses and validates each statement.

The tool uses read-only transactions, `rpt.enable=off`, `rpt.backward=off`,
and EXPLAIN without ANALYZE to compare RPT eligibility with Anser's actual
producer-node count. It records per-statement JSON/CSV, source file and
statement ordinal, raw dumps/JSON plans, optimizer identity and suite
summaries. Failures stay in the denominator. Completion is counted by
statements, never by files: 113 JOB, 22 TPC-H and 103 TPC-DS, each under both
optimizer settings. `--allow-partial` is a pilot and cannot satisfy the gate.

| Suite | SELECT statements | Optimizers | Eligibility vs. Anser |
| --- | --- | --- | --- |
| JOB | 113 | off/on | Not run: benchmark cluster and query set not supplied |
| TPC-H | 22 (Q15 CTE form) | off/on | Not run: benchmark cluster and query set not supplied |
| TPC-DS | 103 (99 templates) | off/on | Not run: benchmark cluster and query set not supplied |

`rpt_backward_eligibility` is a server regression under both optimizer
settings, using the production plan walker and Anser resolver. It asserts
qualification, local/same-slice and root-on-probe refusals, bridge-led classes,
cap enforcement, parameterized builds and a correlated SubPlan containing a
Motion. Sort and nullable-side expectations follow the actual retained plan
shape: removing those operators is a legitimate optimizer transformation.
Aggregate/window/set-operation boundaries also have server coverage in
`rpt_regions`. Native fixtures additionally cover the individual prefetch
blocker tags and defensive states that valid SQL cannot reliably force.
The SQL suite has been added but has not been executed on a local cluster.

No eligibility percentages or go/no-go conclusion are available yet. This
section must be updated with measured results before adding runtime nodes.
Phase III's reducer must likewise live inside the same statement (for example
Sequence plus Append); no cross-dispatch state is planned.

## GUCs

| GUC | Default | Meaning |
| --- | --- | --- |
| `rpt.enable` | off | Master switch for the analysis hook. |
| `rpt.debug` | off | Log the transfer plan of every analyzed query at DEBUG1. |
| `rpt.backward` | on | QD-only runtime switch reserved for Phase II; disable during the survey. |
| `rpt.max_backward_consumers` | 4 | QD-only cap on consumers per class, applied to analysis decisions. |
| `rpt.max_chain_depth` | 4 | Enforced maximum dependency hops; default provisional until hop latency is measured. |

Setup: `shared_preload_libraries = 'anser,robust_predicate_transfer'`, in that
order (the extension resolves Anser symbols at load time).  The analysis UDF
needs `CREATE EXTENSION robust_predicate_transfer`.
