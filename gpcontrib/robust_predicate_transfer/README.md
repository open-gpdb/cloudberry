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

## Call chain and data structures

### PostgreSQL planning entry point

At preload, `_PG_init()` registers RPT's local CustomScan methods and
installs `rpt_planner()` as a planner hook. The query follows this path:

```text
SQL → parse → analyze and rewrite → pg_plan_query() → planner()
  → rpt_planner()
    → previous planner hook (Anser)
      → standard_planner() → PostgreSQL planner or ORCA
      ← PlannedStmt
    → RPT processing
  ← final PlannedStmt
→ executor
```

RPT analyzes a finished distributed plan, after the previous hook returns.
Runtime injection runs on the coordinator for eligible SELECT statements
when `rpt.enable` and either `rpt.forward` or `rpt.backward` are enabled. It requires
`anser.enable=on` and `anser.runtime_filter=off`.

The hook calls `rpt_try_injection()` → `rpt_build_injection()` →
`RptInject(stmt, true, ...)`. Injection works on a private copy of
`PlannedStmt`. An ordinary construction error produces a warning and retains
the original plan; query cancellation still propagates. See
[rptinit.cpp](src/rptinit.cpp) and [rptforward.cpp](src/rptforward.cpp).

### Analysis and schedule selection

Inside `RptInject()`, the analysis proceeds as follows:

| Call | Product | Use |
| --- | --- | --- |
| `RptCollectGraph()` | Scan vertices and join edges in `RptTransferMgr` | Records relations, keys, predicates, distribution policies and allowed transfer directions |
| `RptBuildClasses()` | Key equivalence classes and table classifications | Finds relations connected through equal keys and possible bypass edges |
| `RptBuildForwardPlan()` | Largest-root schedule for both runtime passes | Retains forward and backward edges and their local/global classifications |
| `RptCheckBackwardConsumers()` | Per-edge eligibility proof | Records original parents, key positions, slice indexes, refusal reasons and the fan-out cap |
| `RptSelectForward()` | `vector<RptForwardComponent>` | Selects chains using cost, root-size and selectivity gates |
| `RptSelectBackward()` | `vector<RptBackwardClass>` | Groups proven consumers by class and applies sizing, selectivity and exchange-cost gates |

The implementations are in [rptregion.cpp](src/rptregion.cpp),
[rptgraph.cpp](src/rptgraph.cpp), [rptplan.cpp](src/rptplan.cpp),
[rpteligible.cpp](src/rpteligible.cpp) and
[rptforwardplan.cpp](src/rptforwardplan.cpp) and
[rptbackwardplan.cpp](src/rptbackwardplan.cpp).

The structures are declared in [rpt.h](include/rpt.h):

| Structure | Meaning |
| --- | --- |
| `RptVertexInfo` | One scan, its relation OID, row estimate, predicates and distribution properties; `scan` points into the working plan |
| `RptKey` | A column identified by `{rtindex, attno}` |
| `RptEdgeInfo` | Equality connection between two vertices, key pairs and directions prohibited by join semantics |
| `RptKeyGroup` | An equivalence class of join keys and its leader |
| `RptGraphNode` | Incoming and outgoing forward/backward transfers for one vertex |
| `RptGraphEdge` | One transfer; `other_rtindex` names the producer for an incoming edge and the consumer for an outgoing edge |
| `RptTransferMgr` | Temporary owner of the vertices, edges, classes, schedules and backward eligibility decisions |
| `RptForwardComponent` | One root, its ordered reducer chain, channels, estimates, and acceptance or refusal reason |
| `RptForwardChannel` | Source and target keys, filter sizing, channel identity and local/global execution decisions |
| `RptBackwardConsumer` | One survey proof or refusal for a class member |
| `RptBackwardClass` | One root producer, its sizing and selected consumers, estimated cost and decision |
| `RptInjectionState` | Shared channel/node allocators and original-parent/current-top maps, declared in [rptinject.h](include/rptinject.h) |

`RptTransferMgr.vertex_index_by_rtindex` maps a range-table index to a
position in `vertices`. `neighbor_matrix` holds the forward graph after skip rules;
`logical_neighbor_matrix` retains the logical edges for backward scheduling;
`uf_parent` represents the key union-find; `groups` contains the resulting
classes. `graph` maps each range-table index to its transfer node.
`root_rtindexes`, `bridge_rtindexes`, `reducer_order_rtindexes` and
`transfer_order_rtindexes` describe the schedule. These are planning
structures; the executor receives the resulting plan nodes instead.

Keep the identities distinct when reading or extending the code:

| Name | Identity |
| --- | --- |
| `rtindex` | Relation occurrence in `PlannedStmt.rtable`; distinguishes self-join aliases |
| `relid` | Relation OID; identifies the underlying relation |
| `attno` | Column number within the relation or the relevant node's output |
| `plan_node_id` | Executable plan node |
| `slice_index` | Distributed execution slice |
| `condition_id` | Runtime filter channel connecting a producer and its consumers |

After selection, preflight checks that the scans and required key outputs
can be used. A rejected component adds no executable nodes.

### Constructing the executable plan

For an accepted chain `A → B → C`, where C is the largest relation, RPT
builds this conceptual plan:

```text
Reducer InitPlan:
  Sequence
    Producer(filter A)
      copied scan A + safe restriction quals
    Producer(filter B)
      Consumer(filter A)
        copied scan B + safe restriction quals
    empty Result

Main plan:
  original joins
    Producer(backward filter C)
      Consumer(filter B)
        original scan C
    Consumer(backward filter C)     [only on proven class members]
      original member scan
```

Reducer scans output the required key columns. Restriction quals that
cannot be proven safe for this extra execution are omitted, reducing filter
selectivity. Consumers sit below producers, so B's outgoing filter contains
keys from rows that have already passed A's filter.

In-process reducer channels use `RptBuildLocalProducerScan()` and
`RptBuildLocalConsumerScan()` from [rptlocal.c](src/rptlocal.c). Channels
requiring Anser use `AnserBuildBloomProducerScan()` and
`AnserBuildBloomConsumerScan()`. Policy locality and in-process execution
are separate decisions: a filter consumed by the main plan must cross the
InitPlan dispatch boundary even when its keys are co-distributed.

By default, only the main-plan root receives forward filters.
`rpt.forward_apply=all` also attempts to wrap applicable chain scans.
Backward producers and consumers are added by
[`RptInjectBackward()`](src/rptbackward.cpp) after forward wrappers. A class
is preflighted before any of its nodes are built: every endpoint must occur
once, remain under its proven original parent, and expose the required key.
Each wrapper uses the current stack top and relinks only the original parent.
One allocator supplies condition IDs across both passes, and one supplies
plan-node IDs. If forward is disabled or refused, backward may still run on
the plain root scan.

`RptAttachReducerInitPlan()` in [rptinject.cpp](src/rptinject.cpp) attaches
the reducer:

```text
Coordinator InitPlan slice
  Gather Motion
    Segment sender slice
      Sequence
        reducer subtree 1
        reducer subtree 2
        ...
        Result with constant false
```

It appends the reducer tree to `PlannedStmt.subplans`, attaches a `SubPlan`
descriptor to the main tree's `initPlan`, adds a `PARAM_EXEC` slot in
`paramExecTypes`, and extends `slices`, `subplan_sliceIds` and `numSlices`.
New nodes receive unique `plan_node_id` values. The modified `PlannedStmt`
is the lasting output of RPT planning.

### Execution

Cloudberry initializes the plan and processes distributed InitPlans through
`preprocess_initplans()` before main execution. The reducer's `Sequence`
drains its children in producer-before-consumer order. The final empty
`Result` returns no query rows; the reducer's useful output is its filters.

For the example above, execution scans A and builds its filter, scans B
through A's filter and builds B's filter, then executes the main plan with
B's filter applied to C. C's backward producer summarizes the rows that
pass these consumers and publishes to the proven class-member consumers.
Anser's `defer_first` preserves the probe tuple a hash join may fetch before
building its hash table. The original joins still check the exact SQL
conditions. Abandoned producers cancel their channels, and exchange failure
or timeout makes consumers run unfiltered.

An in-process producer stores its Bloom filter in executor-owned state,
identified by `EState` and `condition_id`. A local consumer uses only a
complete filter; otherwise it passes rows through. Anser-backed nodes
handle filter publication, transport and consumption across dispatches.

### Diagnostic entry point

`rpt_transfer_plan(text)` follows a separate path:

```text
rpt_transfer_plan()
  → pg_parse_query()
  → pg_analyze_and_rewrite_fixedparams()
  → pg_plan_query()                 [RPT injection suppressed]
  → rpt_analyze()
    → RptCollectGraph()
    → RptBuildClasses()
    → RptBuildTransferPlan()
    → RptDumpTransferPlan()
    → RptInject(materialize=false)
  → text containing analysis and forward decisions
```

This function plans the supplied query but does not execute it, run reducer
scans or build filters.

The diagnostic dump retains the filtered-root survey schedule for comparison.
The `forward component`, `backward class` and `backward consumer ... root=`
lines describe runtime decisions on the shared largest-root schedule.

## Backward controls and cost model

`rpt.backward` defaults to `on`; `rpt.enable` remains `off` by default.
`rpt.forward=off` enables a backward-only comparison. The backward producer
is sized from the root's estimated per-segment rows multiplied by the
accepted forward component's estimated selectivity, or by 1 when no forward
component was accepted. These estimates affect selection and sizing, not
SQL correctness.

Both passes use the exchange model:

```text
collection_ms_per_MiB × producer_processes × filter_MiB
  + delivery_ms_per_MiB × consumer_processes × filter_MiB
```

`rpt.forward_ms_per_mib_part` (3.5) controls collection, and
`rpt.delivery_ms_per_mib` (0.8) controls delivery. The defaults come from the
2026-11-10 measurement on one host with three primaries. The existing
`rpt.forward_max_exchange_ms` and `rpt.forward_max_selectivity` names are
retained and apply to both passes. The exchange limit is per forward chain
or backward class; `-1` disables it. A selectivity threshold of 1 disables
the benefit gate. Unknown backward NDV is treated as a kept fraction of 1.
`rpt.max_backward_consumers` caps each class after the proof, preferring
larger estimated rows removed.

Backward process counts come from the actual producer and consumer slices,
including direct dispatch, singleton gangs and parallel workers. Reducer
scans remain nonparallel; a worker's partial filter must never be treated as
a complete segment-local filter.

## Validation and experiments

Run native checks without a cluster:

```sh
make -C gpcontrib/robust_predicate_transfer unitcheck surveycheck diagnosticcheck
```

With both extensions installed and preloaded on a running cluster:

```sh
make -C gpcontrib/anser installcheck
make -C gpcontrib/robust_predicate_transfer installcheck
```

`rpt_backward` checks runtime shape, row reduction, gates and result equality
under both optimizers. `rpt_backward_faults` requires `gp_inject_fault` and
checks held delivery, consumer walk-away, oversized-producer cancellation,
query cancellation and a succeeding next command. Assertions use dynamic
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

Before choosing production defaults, measure:

| Experiment | Measurement |
| --- | --- |
| B0 | Largest-root eligibility and runtime refusal reasons on JOB/TPC-H/TPC-DS, under both optimizers |
| B1 | Delivery scaling with 1/2/4/8 consumers, 1/8/64 MiB filters and available cluster widths |
| B2 | Probe first-tuple time and consumer wait cost with backward off/on |
| B3 | No filters / Anser filters / RPT forward / RPT forward plus backward on snowflake and multi-fact queries |
| B4 | Per-query correctness and latency changes across JOB and TPC-DS SF100 |

Use `scripts/survey.py` for B0; it plans queries without executing them and
reports both the original survey and largest-root runtime decisions. Run as
a superuser or a role granted EXECUTE on `rpt_transfer_plan(text)`. TPC-DS
14/23/24/39 contain two SELECTs each: split them into a/b variants or retain
two SELECT statements per file. Use the CTE form of TPC-H Q15. Expected query
counts are statements (103 for TPC-DS, 22 for TPC-H), not SQL file counts.

A reducer-side backward fallback, bridge-chained backward filters, dynamic
abandonment and sharing delivered filters between consumers in one process
remain deferred. The reducer fallback would scan the largest table again;
it is not enabled or selected automatically.
