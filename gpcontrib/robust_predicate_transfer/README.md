# RPT+ for Apache Cloudberry

RPT+ (Robust Predicate Transfer with Dynamic Execution) reduces the work of
multi-table joins by transferring runtime filters between related tables.
A selective predicate on one table can reduce another table before their
join executes, and that reduction can propagate through the join graph.
The goal is to make query performance less sensitive to join order and
inaccurate cardinality estimates.

A Bloom filter summarizes the join keys that survive filtering. A receiving
scan can discard keys that are definitely absent, while the join still
checks all remaining rows. Bloom-filter false positives can add work but do
not remove matching rows. RPT+ chooses transfer schedules and controls their
overhead; building and exchanging filters is useful only when the saved
query work exceeds that cost.

## Why it uses Anser

Cloudberry executes queries across multiple segments. For a global filter,
keys from every producer must be combined before a receiving scan can safely
use it. The [Anser extension](../anser/README.md) supplies this runtime
information-sharing mechanism: producers publish filter parts, the
coordinator merges them, and consumers receive the completed filter over
existing dispatch connections. Its channel identity, delivery protocol and
timeout/cancellation handling let RPT+ reuse the transport rather than build
another distributed filter service.

RPT+ decides **which tables exchange filters and in what order**; Anser
provides **global filter exchange and scan consumers**. RPT+ uses Anser's
transport independently of Anser's per-join injection policy. Enable RPT+
with `anser.runtime_filter=off` so only one policy injects filters.

The forward reducer runs as an InitPlan of the same statement, under its
snapshot, before the main query starts. Intermediate co-distributed transfers
stay inside the reducer process. Filters used by the main query cross the
dispatch boundary through Anser's statement-scoped channels.

The backward pass builds one filter per equivalence class on the largest
root's main-plan scan, above its forward consumers. Eligible class members
consume that filter in their main-plan scans. No extra root scan is added.
A consumer is injected only when the existing hash build/probe and slice
ordering proves it can wait safely. Local edges rely on core runtime-filter
pushdown; bridge-led backward classes and dynamic abandonment are deferred.

## References and attribution

- **RPT+ paper:** Yiming Qiao, Peter Boncz and Huanchen Zhang,
  [*Robust Predicate Transfer with Dynamic Execution*](https://doi.org/10.14778/3797919.3797934),
  PVLDB 19(6):1278–1290, 2026.
- **RPT+ artifact:**
  [embryo-labs/dynamic-predicate-transfer](https://github.com/embryo-labs/dynamic-predicate-transfer).
  The transfer-graph code in this extension is adapted from this artifact.
- **Earlier RPT implementation:**
  [robust-sql/robust](https://github.com/robust-sql/robust), the baseline
  implementation extended by the RPT+ work.
- **Anser paper:** Liang Lin et al.,
  [*Anser: Adaptive Information Sharing Framework of AnalyticDB*](https://doi.org/10.14778/3611540.3611553),
  PVLDB 16(12):3636–3648, 2023.

The adapted artifact code is MIT-licensed, Copyright 2018–2025 Stichting
DuckDB Foundation. See [NOTICE](../../NOTICE), [LICENSE](../../LICENSE) and
[the artifact license](../../licenses/LICENSE-mit-duckdb-rpt.txt).

## Building and enabling

Configure Cloudberry with both extensions:

```sh
./configure --with-anser --with-rpt
```

Install the extensions on every host and preload
`anser,robust_predicate_transfer` in that order. Set `anser.enable=on` in the
server configuration and restart the cluster. In a database, create the
`robust_predicate_transfer` extension and set `rpt.enable=on` and
`anser.runtime_filter=off` for queries using RPT+.

`rpt_transfer_plan(text)` reports the transfer analysis without injecting
filters into the query it describes. `rpt.debug=on` logs planning decisions.

## Planning workflow and call stack

RPT transforms a finished distributed `PlannedStmt` on the coordinator.
PostgreSQL or ORCA chooses the joins, scans and Motions first. RPT then
adds filter nodes and, when forward transfer is selected, a reducer InitPlan.
It does not replace the optimizer or execute queries during planning.

With `anser,robust_predicate_transfer` preloaded in that order, the planning
call sequence is:

```text
pg_plan_query()
  planner()
    rpt_planner()                              [RPT planner hook]
      anser_planner()                          [previous hook]
        standard_planner()                     [PostgreSQL planner or ORCA]
        AnserApplyRuntimeFilters()             [inactive with runtime_filter=off]
      rpt_log_analysis()                      [only with rpt.enable + rpt.debug]
      rpt_try_injection()                     [eligible coordinator SELECT]
        rpt_build_injection()                 [C++ exception boundary]
          RptInject(stmt, materialize=true)
    return final PlannedStmt
```

The hook injects only when `rpt.enable` is on and at least one of
`rpt.forward` or `rpt.backward` is on. It excludes SELECT INTO, COPY INTO and
refresh plans. `RptInject()` also requires `anser.enable=on`,
`anser.runtime_filter=off`, and a plan without existing Anser or RPT filter
nodes. See [rptinit.cpp](src/rptinit.cpp) and
[Anser's hooks](../anser/src/anserinit.c).

`rpt_try_injection()` creates a memory context for construction.
`RptInject()` copies the statement and edits that copy. If construction
fails, the hook discards the context, reports a warning and returns the
original plan. Query cancellation propagates. `RptPgCall()` translates
PostgreSQL errors into C++ exceptions while STL owners unwind; the entry
boundary then returns the error to PostgreSQL's handler.

### Inside the shared injector

[`RptInject()`](src/rptforward.cpp) runs the following sequence once for
runtime injection. Forward and backward use the same largest-root forest
and the same scan pointers into the copied statement.

```text
RptInject()
  check Anser settings and existing filter nodes
  copy PlannedStmt
  RptCollectGraph()                            → analysis: RptTransferMgr
  RptBuildClasses(analysis)
  RptBuildForwardPlan(analysis)                 → runtime schedule: RptTransferMgr
  RptCheckBackwardConsumers(stmt, schedule)     → backward_consumers proofs
  rpt_find_parents()                           → original parents and scan slices
  initialize RptInjectionState                 → IDs and current scan-stack tops
  collect process counts and row/selectivity estimates

  RptInjectForward()                           [when rpt.forward=on]
    RptSelectForward()                         → RptForwardComponent[]
    preflight scans and key output positions
    rpt_assign_forward_channels()              → IDs from shared state
    build reducer scans and producer/consumer wrappers
    rpt_apply_in_main()                        → forward consumers in main plan
    return reducer children and selected components

  RptInjectBackward()                          [when rpt.backward=on]
    AnserSliceProducers()                      → actual slice process counts
    RptSelectBackward()                        → RptBackwardClass[]
    preflight each accepted class against original parents/current stack tops
    assign channel ID from shared state
    AnserBuildBloomProducerScan()              → wrap root's current top
    AnserBuildBloomConsumerScan()              → wrap each accepted member's top
    RptRelinkStack()                           → update original parent and top map

  RptAttachReducerInitPlan()                   [only if reducer children exist]
  return copied statement if anything was injected, otherwise original
```

`RptInjectForward()` is an internal function in
[rptforward.cpp](src/rptforward.cpp). Backward placement lives in
[rptbackward.cpp](src/rptbackward.cpp); its selector is in
[rptbackwardplan.cpp](src/rptbackwardplan.cpp). A dry run follows selection
and preflight without constructing executable nodes.

### Graphs, schedules and placement state

The analysis structures are declared in [rpt.h](include/rpt.h):

| Structure | Produced by | Used for |
| --- | --- | --- |
| `RptVertexInfo` | `RptCollectGraph()` | One base scan, relation OID, row estimate, restriction/distribution properties and join keys; `scan` points into the working statement |
| `RptKey` | Graph collection | Physical column identity `{rtindex, attno}` |
| `RptEdgeInfo` | Graph collection and class building | Equality clauses between two vertices and transfer directions prohibited by join semantics |
| `RptKeyGroup` | `RptBuildClasses()` | An equivalence class, its members and selected leader/key |
| `RptGraphNode` / `RptGraphEdge` | `RptBuildForwardPlan()` | Per-vertex incoming/outgoing transfers for both passes and their local/global classification |
| `RptBackwardConsumer` | `RptCheckBackwardConsumers()` | Per-edge proof or refusal, original parents, output key positions, slice indexes, `defer_first` and estimated benefit |
| `RptForwardComponent` / `RptForwardChannel` | `RptSelectForward()` | One root's ordered reducer chain, channels, sizing, estimates and acceptance decision |
| `RptBackwardClass` / `RptBackwardTarget` | `RptSelectBackward()` | One root producer and its selected/refused members, sizing and exchange-cost decision |
| `RptTransferMgr` | Collection, class building and scheduling | Owns the temporary graph, classes, forest and eligibility decisions |

`RptBuildClasses()` forms unprotected key equivalence classes and makes
implied equality edges explicit. It retains those logical edges in
`logical_neighbor_matrix`, then applies `SkipUnfilteredTable` to the
forward view, `neighbor_matrix`. Forward-only skipping therefore cannot
erase a useful backward edge from an unfiltered root.

Despite its name, `RptBuildForwardPlan()` constructs the shared runtime
forest for **both** passes. It selects the largest scan by estimated
cardinality as each component's root. Frontier selection considers both
graph views; forward parents use the pruned forward edges, and backward
parents use the logical edges. Original join-direction protections remain
in force. `graph`, `root_rtindexes`, `bridge_rtindexes`,
`transfer_order_rtindexes` and `reducer_order_rtindexes` hold the result.
See [rptgraph.cpp](src/rptgraph.cpp) and [rptplan.cpp](src/rptplan.cpp).

Executable endpoints must be plain SeqScans exposing the selected key in
their output. Join keys must pass the supported built-in, canonical by-value
type/equality checks used for raw-Datum Bloom filtering.

The backward proof runs before wrapping any scan. It permits a blocking
consumer only on a supported hash probe path whose producer is in the
build subtree and in a different slice. It checks intervening blocking
nodes, outer-join paths, scans inside SubPlans and external producer
parameters, then
applies the per-class consumer cap. Local edges use core runtime filtering;
bridge-led classes are deferred. See [rpteligible.cpp](src/rpteligible.cpp).

[`RptInjectionState`](include/rptinject.h) holds the condition/node counters
and two maps keyed by the original scan pointer: `parents` records the
original relink point, and `tops` records the outermost wrapper so far.
Forward consumers are added first. Backward producers and consumers wrap
these current tops, preserving forward filters below the backward producer.
Preflight checks that each original parent still holds that top exactly
once and that the key remains available in the scan's output.

These identities have distinct meanings:

| Name | Meaning |
| --- | --- |
| `rtindex` | Relation occurrence in `PlannedStmt.rtable`; self-join aliases have different indexes |
| `relid` | Underlying relation OID; aliases of one table share it |
| `RptKey.attno` | Physical column number in that relation |
| `producer_attno` / `consumer_attno` | Key position in the scan's projected output tuple |
| `class_key` | Representative `RptKey` identifying an equivalence class; distinct from its leader's key |
| `plan_node_id` | Executable plan-node identity |
| `slice_index` | Distributed execution slice |
| `condition_id` | Filter-channel identity, allocated across both runtime passes |

`vertex_index_by_rtindex` maps a relation occurrence to its position in
`vertices`. Relation aliases are obtained from the statement's range table
when dumping, rather than stored in each vertex. The executor receives the
modified `PlannedStmt` and its CustomScan parameters; it does not receive
these C++ analysis containers.

## Executable plan and execution workflow

For a selected forward chain `A → B → C`, with C as the root, the conceptual
plan is:

```text
Reducer InitPlan:
  Gather Motion to coordinator
    Sequence on segments
      Producer(filter A)
        copied scan A + safe restriction quals
      Producer(filter B)
        Consumer(filter A)
          copied scan B + safe restriction quals
      empty Result

Main plan, under the original joins and Motions:
  root subtree:
    Producer(backward filter C)                 [one per accepted root-led class]
      Consumer(filter B)
        original scan C

  accepted class-member subtree:
    Consumer(backward filter C)
      original member scan
```

The reducer excludes the root. Its scans output the required key columns
and copy restriction quals only when they are proven safe for this extra,
eager execution; otherwise the whole qual list is dropped. An intermediate
consumer sits below its producer, so the outgoing filter summarizes rows
that survived incoming filters.

`RptAttachReducerInitPlan()` in [rptinject.cpp](src/rptinject.cpp) adds the
Gather/Sequence/empty-Result tree to `PlannedStmt.subplans`, attaches a
`SubPlan` to the main tree's `initPlan`, adds a `PARAM_EXEC` slot and extends
`slices`, `subplan_sliceIds` and `numSlices`. It runs after both injection
passes. Backward-only injection adds no reducer or extra scan. If forward
is refused, backward can still build a filter from the plain root scan.

### Core executor and RPT callbacks

RPT installs a planner hook, not an executor hook. Its execution work is
performed by the injected CustomScan callbacks registered at preload.
The coordinator's core execution path is:

```text
ExecutorStart()
  standard_ExecutorStart()
    InitPlan()                                 [initialize plan states]
    preprocess_initplans()
      ExecSetParamPlan()                       [dispatch/run reducer InitPlan]
    CdbDispatchPlan()                          [dispatch main plan]
ExecutorRun()
  anser_executor_run()                         [Anser executor hook]
    standard_ExecutorRun()
      execute main plan
ExecutorFinish()
  anser_executor_finish()
    standard_ExecutorFinish()
ExecutorEnd()
  anser_executor_end()                         [channel cleanup]
    standard_ExecutorEnd()
```

The reducer's segment `ExecSequence()` drains its children in order before
returning the final empty Result. Its useful output is the filters, not
query rows. Forward filters are produced before the main plan starts;
Anser keeps them available across the InitPlan/main-plan dispatch boundary.

During main execution, the root scan passes through its forward consumers
and then its backward producers. Each backward producer accumulates keys
from those surviving rows and publishes on exhaustion. A member consumer
uses the existing proof's `defer_first` flag: where the hash join can fetch
an outer tuple before building, it passes that first tuple unfiltered and
receives the filter on the next call. Backward adds no scan of either root
or member. The original joins still enforce the exact SQL conditions.

### Local handoff and Anser exchange

A policy-local hop consumed only inside the reducer uses
`RptBuildLocalProducerScan()` / `RptBuildLocalConsumerScan()` and the
[rptlocal.c](src/rptlocal.c) callbacks. The producer stores a Bloom filter
under `(EState, condition_id)` and marks it complete when drained. The
consumer uses only a complete filter; otherwise it passes rows through.

Other hops use Anser's producer/consumer builders. Policy locality alone
does not permit in-process handoff across separate dispatches: forward
filters consumed by the main-plan root use Anser. Intermediate main-plan
scans do not consume forward filters; eligible members can receive the
root's backward filter.

```text
QE ExecProcNode()
  anser_produce_exec() → anser_produce_next()
    ExecProcNode(child), then ExecAnserBloomFilterProduceAddDatum()
    on exhaustion: ExecAnserBloomFilterProducePublish()
      AnserSidebandPublish()
        coordinator AnserDispatchNotifyHandler()
          combine producer parts, then deliver the completed filter

QE ExecProcNode()
  anser_consume_exec() → anser_consume_next()
    anser_consume_receive() → ExecAnserBloomFilterConsume()
    push filter into the scan when supported, otherwise probe returned tuples
```

These are distributed call paths, not one process's stack. The transport
uses existing dispatch connections. Cancelled, unavailable or timed-out
exchanges leave consumers unfiltered; local backward edges are not given
blocking Anser consumers. See [Anser's scan callbacks](../anser/src/anserplanexec.c).

Both Anser and local wrappers keep identity `INDEX_VAR` output lists for
execution. Their deparse lists are built through
[`AnserFilterScanTlist()`](../anser/include/ansertlist.h), which resolves a
child CustomScan's output expressions before copying them. This allows
`EXPLAIN`, including `VERBOSE`, to describe stacked filters without a
self-referencing deparse list.

## Diagnostic workflow

`rpt_transfer_plan(text)` plans one SELECT and returns its analysis without
executing the supplied query or injecting filters into its plan:

```text
rpt_transfer_plan()
  rpt_transfer_plan_impl()
    pg_parse_query()
    pg_analyze_and_rewrite_fixedparams()
    pg_plan_query()                            [automatic RPT injection suppressed]
    rpt_analyze()
      RptCollectGraph() + RptBuildClasses()
      RptBuildTransferPlan()                   [filtered-root survey schedule]
      RptDumpTransferPlan()
      RptInject(materialize=false)             [shared largest-root runtime schedule]
    return text
```

The survey schedule retains its filtered-root heuristic for comparison;
it is not the schedule used for runtime injection. `forward component`,
`runtime survey`, `backward class` and `backward consumer ... root=` lines
show current runtime decisions on the largest-root forest, including
refusal reasons. Dry runs honor the forward/backward switches and Anser
settings even when `rpt.enable` is off.

With `rpt.debug=on`, the planner also calls `rpt_log_analysis()` before
actual injection. That log builds its own survey and dry-run analysis;
runtime injection does not reuse those objects. Debug mode therefore adds
planning work.

## Runtime controls and cost model

`rpt.enable` is the normal user-facing switch and defaults to `off`.
`rpt.forward` and `rpt.backward` are developer controls for testing,
measurement and diagnosis. Both default to `on`, remain session-settable
(`PGC_USERSET`), and are marked `GUC_NOT_IN_SAMPLE`. Normal use leaves both
enabled. With the master switch enabled, developers can isolate either pass:

| Settings | Runtime work |
| --- | --- |
| `rpt.forward=on`, `rpt.backward=on` | Selected forward reducer, then selected in-plan backward filters |
| `rpt.forward=on`, `rpt.backward=off` | Forward reducer and main-plan consumers |
| `rpt.forward=off`, `rpt.backward=on` | In-plan backward filters; no reducer |
| Both off, or `rpt.enable=off` | No runtime injection |

`rpt.forward_min_root_rows` (10000, per segment) gates only the forward
reducer, and `rpt.max_chain_depth` (4) bounds forward dependency hops.
Forward filters are always applied only to the root in the main plan.
Intermediate forward consumers exist only inside the reducer.

The backward producer is sized from the root's estimated per-segment rows
multiplied by the accepted forward component's estimated selectivity, or by
1 when no forward component was accepted. These estimates affect selection and sizing, not
SQL correctness.

Both passes use the exchange model:

```text
collection_ms_per_MiB × producer_processes × filter_MiB
  + delivery_ms_per_MiB × consumer_processes × filter_MiB
```

`rpt.forward_ms_per_mib_part` (3.5) controls collection, and
`rpt.delivery_ms_per_mib` (0.8) controls delivery. The defaults come from the
2026-11-10 measurement on one host with three primaries. The existing
`rpt.forward_max_exchange_ms` (100 ms) and
`rpt.forward_max_selectivity` (0.35) names are retained and apply to both
passes. The exchange limit is per forward chain or backward class; `-1` disables it. A selectivity threshold of 1 disables
the benefit gate. Unknown backward NDV is treated as a kept fraction of 1.
`rpt.max_backward_consumers` (4) caps each class after the proof, preferring
larger estimated rows removed.

Backward process counts come from the actual producer and consumer slices,
including direct dispatch, singleton gangs and parallel workers. Reducer
scans remain nonparallel; a worker's partial filter must never be treated as
a complete segment-local filter.

## Worked example: collect a four-table star

The worked example will use real output from your development cluster.
The procedure below gathers the evidence for its graph, selection,
injection and final `EXPLAIN ANALYZE`; the annotated execution plan and
measured timings will be added from those artifacts.

### 1. Build the current code and prepare the run

Build and install both Anser and RPT on all hosts, and restart the cluster
so the preloaded libraries include the latest changes. Use an otherwise
idle cluster and a dedicated database, with a role allowed to create the
extension and tables and call `rpt_transfer_plan(text)`.

Use your normal cluster connection environment. Run the collector from an
empty output directory and use a fresh database name. Set `rpt_repo` to
this checkout's absolute path on the development machine:

```sh
rpt_repo=/path/to/cloudberry-leborchuk
rpt_output=/tmp/rpt-star-example
mkdir -p "$rpt_output"
createdb rpt_readme_example
cd "$rpt_output"
git -C "$rpt_repo" rev-parse HEAD > revision.txt
git -C "$rpt_repo" diff -- gpcontrib/anser gpcontrib/robust_predicate_transfer > source.diff
cp "$rpt_repo/gpcontrib/robust_predicate_transfer/scripts/collect_star_example.sql" collector.sql
set -o pipefail
psql -X -q -d rpt_readme_example \
  -v fact_rows=1000000 -v warmups=2 -v samples=7 \
  -f collector.sql \
  2>&1 | tee collection.log
```

[`collect_star_example.sql`](scripts/collect_star_example.sql) creates the
`rpt_readme_star` schema and four randomly distributed tables. It stops
rather than overwrite an existing fixture schema. It keeps the fixture for
inspection after collection. The fact has one million rows by default;
customer, product and store have 1009, 503 and 211 rows. Fact keys cycle
through those three domains, and each dimension has a separate restriction
attribute cycling from 0 through 9.

The script disables Anser's independent injection policy and debug logging.
It keeps the core runtime-filter settings and RPT cost/selectivity gates
at their current values and records them. The baseline therefore includes
whatever core filtering those settings enable. Each statement has a
120-second timeout. If the run stops, keep the partial directory and log.

### 2. Inspect the query and input data

The query is saved as `query.sql`:

```sql
SELECT count(*) AS matched_rows,
       coalesce(sum(f.amount_cents), 0) AS total_amount_cents
FROM rpt_readme_star.fact f
JOIN rpt_readme_star.customer c ON f.customer_key = c.k
JOIN rpt_readme_star.product p ON f.product_key = p.k
JOIN rpt_readme_star.store s ON f.store_key = s.k
WHERE c.region < 2 AND p.category < 2 AND s.zone < 2;
```

`cardinalities.csv` records the dimensions' full/selected row counts and
key NDVs. Their predicates select 202 customer keys, 102 product keys and
43 store keys. `fact-cardinalities.csv` records actual fact rows and key
NDVs, while `statistics.csv` contains the catalog estimates used to explain
planner decisions. `distribution.csv` records actual segment counts and
policies. `environment.txt` records the build, topology and effective
settings.

### 3. Read the graph and forward selection

For each optimizer, inspect `off-full.transfer.txt` or
`on-full.transfer.txt`. Here `off`/`on` names the requested **optimizer**
setting; `full` means both RPT passes are enabled.

Resolve `vertex` numbers using aliases `f`, `c`, `p`, `s`. Range-table
indexes can differ between optimizers. The logical graph is:

```text
customer c.k ── fact f.customer_key
product  p.k ── fact f.product_key
store    s.k ── fact f.store_key
```

These are three separate equivalence classes, each containing a dimension
key and its corresponding fact key. The filtered dimensions are forward
candidates; the fact is the expected runtime root because its scan has the
largest estimated cardinality. Read the root from `forward component`,
not from the filtered-root survey's earlier `leader=` lines.

`RptBuildClasses()` preserves the logical graph and prepares its forward
view. `RptBuildForwardPlan()` builds the shared forest, and
`RptSelectForward()` evaluates the dimension-to-fact channels using scan
estimates, NDVs, filter sizing, selectivity and exchange cost. Record:

- The runtime root, reducer order and three source/target key pairs.
- `est_sel`, `est_cost_ms`, filter bytes and the final `decision`.
- Each backward class/member decision and its refusal reason, if any.

The classes do not equate customer, product and store keys to one another,
so this example should not be illustrated as a dimension-to-dimension
chain. The dump's `chain=[...]` lists the reducer scans; the selected
channels show the actual dependencies.

Do not force acceptance for the first run. A gate refusal is useful data,
including on wider clusters where collection/delivery costs increase.
Send that dump before changing the budget or selectivity threshold.

### 4. Match selection to the injected plan

For accepted forward transfers, RPT creates one copied, key-only scan per
selected dimension in the reducer. Each copied scan retains its safe
restriction and gets a producer. The main-plan fact scan gets the three
corresponding consumers; the dimension scans in the main plan are not
wrapped in forward consumers.

The injector proves backward eligibility before adding these wrappers.
It then adds any accepted root producers above the fact's forward
consumers and member consumers at proven relink points. Finally,
`RptAttachReducerInitPlan()` attaches the reducer's Gather/Sequence tree.

In a common star plan the fact is a hash probe input and the dimensions
are build inputs. In that case the current in-plan backward proof can
refuse all three members. Record that outcome: this example should explain
the optimizer's real plan, including why a transfer is absent.

Compare the captured plan files for the same optimizer:

| Arm | Settings | Purpose |
| --- | --- | --- |
| `baseline` | `rpt.enable=off` | Original optimizer plan and answer |
| `forward` | RPT enabled, forward on, backward off | Reducer and root-consumer placement |
| `backward` | RPT enabled, forward off, backward on | In-plan backward decisions without a reducer |
| `full` | RPT enabled, both passes on | Combined selected plan |

For example, `on-full.explain.txt` is the actual text
`EXPLAIN (ANALYZE, VERBOSE, BUFFERS, SETTINGS, SUMMARY)` for the requested
ORCA run, and `on-full.explain.json` is its structured counterpart. Check
the reported optimizer to identify any fallback. These are separate
executions, so their timing and segment-level row counts can differ.

### 5. Verify answers and measure the effect

The collector compares count and integer sum for all eight optimizer/arm
combinations against the first baseline and stops on a mismatch. Each
answer is also saved in `*.result.csv`.

After plan collection it runs two warmups and seven measured queries per
arm per optimizer. Arm order rotates between rounds. These are fresh,
unprepared query executions without EXPLAIN or debug logging; all arms run
in the same psql session. `collection.log` contains markers such as:

```text
RPT_SAMPLE optimizer=on arm=full phase=sample run=1
```

Use the `Time:` immediately following the two-column aggregate result as
the query sample. The log also contains timings for SET and marker
statements; exclude those and all `phase=warmup` queries. Report median
and minimum/maximum for each arm. These are warm-cache psql elapsed times,
including planning, execution and client round-trip; EXPLAIN's planning
and execution times are recorded separately, not used as substitutes.

In the plans, inspect reducer producers, stacked fact consumers, actual
rows entering joins, filter-removal counters, buffers and any backward
producer/consumer nodes. A tuple-level filter can reduce join work while
the underlying scan still reads all heap pages; use the actual counters
rather than infer an I/O saving from fewer join rows.

### 6. Return the evidence for the final README example

Send the complete output directory, including `collector.sql`, `revision.txt`,
`source.diff`, `collection.log`, metadata CSVs, transfer dumps, result CSVs and both plan
formats. Also note CPU/RAM, storage type, number of physical hosts, whether
other workloads ran, and whether the data fit in cache.

We can then annotate the real vertex/class IDs, candidate decisions,
producer/consumer placement, final `EXPLAIN ANALYZE`, row reduction and
measured overhead or benefit. After inspection, remove the dedicated
database when its fixture is no longer needed.

## Build, test and diagnostic workflow

Run native checks without a cluster:

```sh
make -C gpcontrib/robust_predicate_transfer unitcheck surveycheck diagnosticcheck
```

After changing either preloaded library, build and install both extensions
on every host, then restart the cluster so backends load the new code:

```sh
make -C gpcontrib/anser install
make -C gpcontrib/robust_predicate_transfer install
```

With the updated libraries preloaded on a running cluster:

```sh
make -C gpcontrib/anser installcheck
make -C gpcontrib/robust_predicate_transfer installcheck
```

RPT's `installcheck` installs Anser's `anser_test` extension before running
regressions. The fault tests also require `gp_inject_fault`.

`rpt_backward` checks runtime shape, row reduction, gates and result equality
under both optimizers. `rpt_backward_faults` requires `gp_inject_fault` and
checks held delivery, consumer walk-away, oversized-producer cancellation,
query cancellation and a succeeding next command. Cancellation cases prepare
and verify an injected plan before starting the short timeout. Assertions use dynamic
SQL so changes to RPT settings cannot silently reuse an earlier plan.

For a step-by-step run in an isolated test database:

```sh
python3 gpcontrib/robust_predicate_transfer/scripts/diagnose_rpt.py \
  --suite backward --database rpt_test --output /tmp/rpt-backward
```

The runner saves SQL, client output, per-query decisions in `decisions.json`,
and watchdog/cancellation evidence. `--suite forward` and
`--suite backward_faults` run the other workloads. The legacy
`diagnose_forward.py` entry point remains supported.

For benchmark planning surveys, use [scripts/survey.py](scripts/survey.py).
It plans queries without executing them and reports both the filtered-root
survey and largest-root runtime decisions. Run as a superuser or a role
granted EXECUTE on `rpt_transfer_plan(text)`. TPC-DS 14/23/24/39 contain two
SELECTs each: split them into a/b variants or retain two SELECT statements
per file. Use the CTE form of TPC-H Q15. Expected query counts are statements
(103 for TPC-DS, 22 for TPC-H), not SQL file counts.

A reducer-side backward fallback, bridge-chained backward filters, dynamic
abandonment and sharing delivered filters between consumers in one process
remain deferred. The reducer fallback would scan the largest table again;
it is not enabled or selected automatically.
