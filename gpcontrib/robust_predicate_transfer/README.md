# robust_predicate_transfer

RPT+ (Robust Predicate Transfer with dynamic execution) on top of Anser:
plans a transfer schedule for Anser's runtime bloom filters over the query's
join graph — a forward pass that chains filters table-to-table and a backward
pass that broadcasts one merged filter per equivalence class (Qiao, Boncz,
Zhang, PVLDB 19(6), 2026).

Phase I (current) is a pure analysis pass over the finished plan: region
discovery, join graph, equivalence classes, and the asymmetric transfer plan,
ported from the paper's artifact (MIT license — see
`licenses/LICENSE-mit-duckdb-rpt.txt`).  `rpt_transfer_plan(text)` dumps the
analysis for a query; `rpt.debug=on` logs it during planning.

See `docs/design.md` for the architecture and the correctness rules.

Build: `./configure --with-anser --with-rpt` (needs Anser), then
`make -C gpcontrib/robust_predicate_transfer`.  Run tests with
`make -C gpcontrib/robust_predicate_transfer installcheck` against a demo
cluster (it preloads `anser,robust_predicate_transfer` itself).

Run `make unitcheck` in this directory after configuring the source tree to
exercise synthetic graph, stage, co-location, consumer-order and exception
fixtures without starting a server. `make installcheck` additionally needs a
running Cloudberry cluster and executes SQL cases under both optimizers.

Phase II runtime injection is gated by the benchmark eligibility survey in
`plans/rpt-plus-phase2-plan.md`. The stricter analysis and planning-only
`scripts/survey.py` are available; no backward executor nodes are injected yet.
See `docs/design.md` for the survey command and metrics. `make surveycheck`
checks the runner without needing a server.

The survey requires a superuser or explicit EXECUTE on `rpt_transfer_plan(text)`.
It counts SELECT statements: JOB 113, TPC-H 22 (Q15 in CTE form), TPC-DS 103
(99 templates, including the four two-SELECT variants). See the normalization
instructions in the design document. `rpt_backward_eligibility` supplies
production-walker/resolver regression coverage under both optimizers.
