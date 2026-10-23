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
dispatch 1 — reducer (Phase III, not implemented yet)
   forward chain over predicated + bridge tables, root excluded

dispatch 2 — the real query (Phase II, not implemented yet)
   root scan: consume inbound chain filter from the session cache,
              produce the class broadcast filter
   others:    consume the broadcast filter
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

rptplan.cpp ports the artifact's `LargestRootUpdated` + `FindEdge` +
`CreateTransferPlanUpdated`: a greedy Prim forest (one root per component,
chosen as the largest filtered-or-intermediate table, falling back to the
largest), forward stage assigned smaller→larger along the insertion order,
backward stage redirected through each class's leader — the single vertex per
class the broadcast filter is built on.  `rpt.max_chain_depth` caps the
forward chain length once the reducer exists; the default (4) is a placeholder
until hop latency is measured (work item 0 of the phase plan).

## Correctness rules

Each of these is a silent-wrong-answer rule, not a performance rule.  A bloom
filter may only ever *over*-approximate the build side's key set.

1. **Per-hop global merge.** Segment *s* holds only part of table *j*'s keys,
   so filtering table *j+1* with *s*'s local filter drops rows whose key lives
   on another segment.  Each hop must be a global publish → merge → deliver,
   unless the two tables are provably co-distributed on the join key (same
   distkey, same segment count, no redistributing motion between scan and
   join).  A local hop is an optimization on top, never the default.
2. **Snapshot identity.** The reducer (Phase III) must run under the main
   query's snapshot and parameters: a row visible to the query but not to the
   reducer would be silently missing from the filter.
3. **Outer joins are directed edges.** LEFT protects the left side, RIGHT and
   MARK protect the right; ANTI and FULL produce no edge.  The preserved side
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
7. **Plan-order safety.** A backward consumer may only wait on a producer the
   plan order guarantees runs first: the consumer's scan must sit on the probe
   side of a hash join whose build side transitively contains its class
   leader's scan.  `consumer_ok` in the dump reports exactly this; ineligible
   vertices fail open (no backward filter).
8. **Fail open, always.** Timeout, malformed part, oversized filter, squelched
   producer, missing cache entry — all degrade to "no filter", never to an
   error and never to a filtered result.

## Runtime contract with Anser (Phases II/III)

- The reducer never joins — scan, filter, build — so it fits one slice, one
  gang, one dispatch, with the forward chain as an ordered `Append`.
- The backward pass is ordinary Anser injection into the main plan.
- A session-scoped filter cache per QE, keyed by condition_key alone, carries
  chain filters across the dispatch boundary and lets the broadcast be
  delivered once per segment rather than once per consuming scan.
- Condition keys derive from the semantic identity `(region_id, class_id,
  build_rel)`, not a per-query counter.

## GUCs

| GUC | Default | Meaning |
| --- | --- | --- |
| `rpt.enable` | off | Master switch for the analysis hook. |
| `rpt.debug` | off | Log the transfer plan of every analyzed query at DEBUG1. |
| `rpt.max_chain_depth` | 4 | Cap on the forward chain length (placeholder until hop latency is measured). |

Setup: `shared_preload_libraries = 'anser,robust_predicate_transfer'`, in that
order (the extension resolves Anser symbols at load time).  The analysis UDF
needs `CREATE EXTENSION robust_predicate_transfer`.
