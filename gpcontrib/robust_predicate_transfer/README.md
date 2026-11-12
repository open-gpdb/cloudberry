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
dispatch boundary through Anser's statement-scoped channels. Backward
transfer schedules are available for analysis; backward runtime injection
and dynamic abandonment are not implemented.

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
