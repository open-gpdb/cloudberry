# Phase III experiment plan

Goal: find out whether the forward reducer is correct on a real cluster, what
it costs, where it pays off, and whether `c × n_producers × filter_MiB` is a
good enough gate. Experiments are ordered; each states what blocks the next.

Common setup for every run:

- `shared_preload_libraries = 'anser,robust_predicate_transfer'`; restart the
  cluster after installing new binaries on every host.
- Record: commit hash, number of hosts and primaries, interconnect type,
  `gp_vmem`/`work_mem`, both `optimizer` settings.
- Per query, keep the `rpt_transfer_plan()` dump, `EXPLAIN (ANALYZE, FORMAT
  JSON)` and wall time. Reject any run with an "RPT forward reducer skipped"
  WARNING in the log, unless the experiment is about that warning.
- Timings: 1 warm-up, then 10 runs; report median and p95.

## E0. Correctness gate (blocks everything)

1. `make installcheck` in `gpcontrib/anser` and in
   `gpcontrib/robust_predicate_transfer`, which now includes `rpt_forward`
   and `rpt_forward_faults` (the latter needs a build with fault injection).
2. `make installcheck` (core) with `rpt.enable=on`, `rpt.forward=on`,
   `rpt.forward_min_root_rows=0` and `rpt.forward_max_exchange_ms=-1` set in
   `PGOPTIONS`. Failures count only when they disappear with
   `rpt.enable=off`. This is the broad crash and wrong-result sweep.
3. The same with `rpt.forward_apply=all`.
4. Cancel and error paths: `pg_cancel_backend` during the reducer; a segment
   `gp_inject_fault` error inside an Anser producer; `statement_timeout`
   shorter than the reducer. Expect a clean error, no hung QEs and no leaked
   channels on the next statement.

Pass: zero result differences. Any wrong result stops the plan.

## E1. Cross-dispatch proof

On the `rpt_forward` star fixture, under both optimizers: in the main plan,
`Rows Removed by Bloom Filter` plus `Rows Removed by Pushdown Runtime Filter`
is greater than 0. Also check with `gp_log_interconnect`/Anser tracing that the
producer channels are created in the InitPlan's dispatch and consumed in the
main dispatch, with the same `gp_command_count`.

## E2. Fixed overhead of the InitPlan

`rpt.debug_noop_initplan` with `scripts/measure_initplan.py`
(see `phase3-measurements.md`). Measure on 3, 8 and 24 primaries, and on 1 vs
several hosts. Output: the extra dispatch latency per statement. This is the
floor any accepted chain must beat; it should become a constant term in the
gate if it exceeds a few milliseconds.

## E3. Calibrating c

Single-channel star (one dimension, filter sized to 1, 8 and 64 MiB) with
forward-only RPT. For each topology (3/8/24 primaries, 1 and 2+ hosts):

- measured exchange = reducer InitPlan time − reducer scan time;
- fit `measured = a + c × n_producers × MiB`.

Pass: the fit is within 25% of measured times, and `c` stays within 2× of 3.5
across topologies. Otherwise, make `c` per topology or change the model, for
example `c₁ × n_producers × MiB + c₂ × n_consumers × MiB` for delivery.

Small filters (64–512 KiB) need Anser's 1 MiB floor lowered. That requires a
debug-only GUC in Anser first; it is optional and should run only if E4 shows
many small filters.

## E4. Star benchmarks: where it pays off

Fixtures: 1 fact table (10M, 100M rows), 1–4 dimensions, selectivity of each
dimension 0.1%, 1%, 10%, 50%, 100%. Four configurations:

| Config | Settings |
| --- | --- |
| baseline | `anser.enable=off` |
| Anser RF | `anser.enable=on`, `anser.runtime_filter=on` |
| RPT root | `rpt.enable=on`, `anser.runtime_filter=off`, `rpt.forward_apply=root` |
| RPT all | as above, `rpt.forward_apply=all` |

Run first with the gates open (`min_root_rows=0`, `max_exchange_ms=-1`,
`max_selectivity=1`) to measure real gains and losses, then with default
gates to see what the gates keep. Record `est_sel` from the dump next to the
measured fraction of root rows removed, to check the selectivity estimate. Output per query: speedup against baseline and against Anser RF, plus the
gate decision. Expected: wins at low selectivity and large facts; losses at
50–100% selectivity, which the gates should refuse.

## E5. Chains and snowflakes

Snowflake schemas (fact → dim → subdim, depth 2–4) and two-fact queries,
where transitive transfer is the point of RPT+. Compare RPT root/all with
Anser RF. This checks that serial hops add up as the model assumes (sum of
channels), and gives data for `rpt.max_chain_depth`.

## E6. Workloads

JOB (IMDB), TPC-H SF100 and TPC-DS SF100, under both optimizers:

1. Survey: for every query, the dump's forward decision and refusal reason
   (e.g. "no forward edges", gates, non-Seq-Scan targets in preflight).
2. Benchmark: baseline / Anser RF / RPT root, using default gates.
3. Report regressions per query (> 5% slower than the better of baseline and
   Anser RF), each with its dump. These decide the default gate values.

## E7. Gate tuning

From E3–E6, choose defaults for `rpt.forward_ms_per_mib_part`,
`rpt.forward_max_exchange_ms`, `rpt.forward_min_root_rows` and
`rpt.forward_max_selectivity` (λ = 0.35 is the paper's value, not ours). A stronger gate
would compare the cost to estimated savings (removed root rows × per-row cost
above the root). Add that only if E6 shows cost-only gating either refuses
winners or admits losers.

## Open items found while implementing

- In-process hops are implemented for local hops consumed only inside the
  reducer. E5 should compare a colocated snowflake with a random one to
  measure what the in-process path saves.
- Reducer quals that are not provably error-free (non-leakproof functions,
  I/O casts) are dropped whole. E6's survey should count how often that
  happens; if it is frequent, keeping the provably safe subset is a
  follow-up.
- Only Seq Scan targets, one key per channel. E6's survey shows how many
  queries this excludes.
- Publish-start timestamp in Anser: needed for exact exchange timing in E3.
  The current proxy starts at serialization.
