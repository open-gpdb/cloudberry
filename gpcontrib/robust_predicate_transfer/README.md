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
