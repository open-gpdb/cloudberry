# Phase III measurement status

## Exchange measurements, 2026-11-10

The supplied `anser-20261110` results cover three primaries on one host.
Eight- and 24-primary topologies were unavailable. The fixture was one
cross-Motion Anser channel under GPORCA, with 10,000 build keys and a
1,000,000-row probe. All 90 measured exchanges completed with three producer
parts, one complete merge and three complete consumers; traced query results
were 10,000. Anser regressions passed 4/4.

| Bitset | Exchange proxy median / p95, ms | Delivery tail median / p95, ms | Untraced execution median off / on, ms |
| --- | --- | --- | --- |
| 1 MiB | 11.188 / 17.082 | 1.197 / 1.711 | 89.382 / 33.613 |
| 8 MiB | 82.946 / 87.325 | 6.223 / 7.770 | 87.834 / 121.425 |
| 64 MiB | 765.530 / 789.889 | 48.387 / 58.273 | 89.446 / 880.313 |

The proxy begins at the trace immediately before serialization, not an exact
publish-start timestamp. Trace timings include logging overhead. Off/on
execution differences combine construction, transport and filtering; they
are not network timings. The sample CSV medians and nearest-rank p95 values
were checked against the supplied summary. These observations do not establish
multi-host scaling or Phase III performance.

Payload size must participate in the forward cost estimate. A depth cap alone
cannot distinguish an approximately 11 ms exchange from a 766 ms exchange.
Keep the current chain-depth default explicitly provisional. Dispatch cost,
reducer scan cost and representative root-scan savings remain unmeasured.

RPT regressions passed 2/3. The nullable-side fixture in
`rpt_backward_eligibility`, with optimizer off, expected an eligible producer
but received `producer is not on a qualifying hash build side`. The supplied physical
EXPLAIN confirms the LEFT join simplifies to INNER, but the root is the
upper hash join's probe and the outer/member subtree is its build. Rejection
is correct. The regression now checks the named scans' physical build/probe
placement before expecting eligibility; this test correction still needs a
cluster rerun.

## Exact no-op InitPlan harness

`rpt.debug_noop_initplan` is a superuser-only, QD-only diagnostic switch,
default off and independent of `rpt.enable`. It clones the finished statement
and appends the production builder's empty reducer:

```text
InitPlan (EXISTS, unused boolean PARAM_EXEC)
  Gather Motion
    Sequence
      Result (One-Time Filter: false)
```

Its reader gang covers every primary segment, with no parallel workers or
direct dispatch. It builds no filters and tests no cross-dispatch channels.
Construction errors warn and retain the original plan; cancellation propagates.
**Reject measurements with a skipped-InitPlan warning or missing plan shape.**

After rebuilding and installing the extension on all hosts, restart the test
cluster so all backends use the same binary. Use a fresh superuser connection
and an idle cluster, with the existing Anser/RPT preload order. From the
repository root:

```sh
python3 gpcontrib/robust_predicate_transfer/scripts/measure_initplan.py \
  > /tmp/rpt-initplan.sql
psql -X -v ON_ERROR_STOP=1 -d rpt_exchange_bench \
  -f /tmp/rpt-initplan.sql > /tmp/rpt-initplan.out 2>&1
```

The script uses a temporary, randomly distributed 3,000-row table. It emits a
shape EXPLAIN and a query with an existing scalar subquery for both optimizers,
then 10 warmups and 100 measured pairs per optimizer. The existing-subquery
query must return 3,000. Each pair runs the same count with the diagnostic
switch off and on; first-arm order alternates. Runtime filters and tracing
are disabled. `SAMPLE initplan <optimizer> <phase> <index> <off|on>` labels
precede each measured EXPLAIN ANALYZE.

Check that both shape plans contain the new InitPlan, Gather, Sequence and
false Result. Record planning time, execution time and psql elapsed time for
each labeled EXPLAIN ANALYZE; ignore times printed for SET commands. Client
elapsed time matters because distributed InitPlans run during ExecutorStart,
not necessarily inside the reported main-plan execution interval.

Report median and p95 of each arm, plus the median and p95 of paired on-minus-off
client elapsed times, separately by optimizer. Preserve raw output and relevant
QD/QE logs. The harness has passed native layout tests and C++ syntax checks;
cluster execution remains unvalidated. This measurement is the next executor
smoke test, not proof of filter delivery between dispatches.
