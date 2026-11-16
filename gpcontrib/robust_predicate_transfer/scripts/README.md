<!--
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing,
  software distributed under the License is distributed on an
  "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
  KIND, either express or implied.  See the License for the
  specific language governing permissions and limitations
  under the License.
-->

# RPT scripts

Run these tools against a development cluster with the extensions installed
and [preloaded in the required order](../README.md#preload-order). Use your
normal libpq connection environment and run commands from the repository
root unless a procedure below changes directory.

| Script | Purpose |
| --- | --- |
| [collect_star_example.sql](collect_star_example.sql) | Create the four-table star and capture its decisions, plans, answers and timings |
| [diagnose_rpt.py](diagnose_rpt.py) | Run an RPT regression workload with step markers, decision summaries and an external watchdog |
| [diagnose_forward.py](diagnose_forward.py) | Compatibility entry point for the diagnostic runner; defaults to the forward suite |
| [survey.py](survey.py) | Plan benchmark queries and compare RPT eligibility/runtime decisions with Anser injection |

## Collect the four-table star example

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

[`collect_star_example.sql`](collect_star_example.sql) creates the
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

## Diagnose a regression workload

Use an isolated test database. The selected regression creates fixture
tables and the RPT SQL extension, runs assertions, then removes its
fixtures. The `backward_faults` suite also requires `gp_inject_fault` and
`anser_test`; RPT's `make installcheck` installs `anser_test`.

```sh
python3 gpcontrib/robust_predicate_transfer/scripts/diagnose_rpt.py \
  --suite backward --database rpt_test --output /tmp/rpt-backward
```

`--suite` accepts `forward` (the default), `backward`, or `backward_faults`.
Use `--emit-only` to save the instrumented SQL without executing it.
`--psql` selects the client executable. `--wall-timeout` (180 seconds) and
`--cancel-grace` (15 seconds) control the external watchdog.

The output directory contains:

- `workload.sql`: the instrumented SQL actually submitted.
- `client.log`: step markers, notices, answers and errors.
- `decisions.json`: per-query forward decisions and backward class,
  consumer, refusal and estimated-cost summaries.
- `result.json`: client status, elapsed time and whether the watchdog fired.
- On timeout, activity snapshots and the result of `pg_cancel_backend()`.

The watchdog attempts cancellation, then stops its client if it remains
unresponsive. It leaves server backends available for inspection and does
not restart the cluster. Keep partial artifacts when a run fails.

The legacy `diagnose_forward.py` command accepts the same arguments.

## Survey benchmark plans

The survey plans queries without executing them, under both optimizer
settings. It records the filtered-root survey and largest-root runtime
decisions alongside Anser's independently injected plan. Run as a superuser
or a role granted EXECUTE on `rpt_transfer_plan(text)`.

```sh
python3 gpcontrib/robust_predicate_transfer/scripts/survey.py \
  --job /path/to/job-sql --job-dbname imdb \
  --tpch /path/to/tpch-sql --tpch-dbname tpch \
  --tpcds /path/to/tpcds-sql --tpcds-dbname tpcds \
  --output /tmp/rpt-survey
```

Each directory must contain instantiated SELECT/WITH queries. TPC-DS
14/23/24/39 contain two SELECTs each: split them into a/b variants or keep
two SELECT statements per file. Use the CTE form of TPC-H Q15. The expected
counts are SELECT statements: 113 for JOB, 22 for TPC-H and 103 for TPC-DS.

`--dbname` and `--search-path` set defaults for all suites; the per-suite
`--job-search-path`, `--tpch-search-path` and `--tpcds-search-path` options
override the schema path. `--statement-timeout-ms` defaults to 30000.
`--allow-partial` permits a pilot with fewer queries but never marks the
survey complete. `--psql` selects the client executable.

The collector writes `results.json`, `results.csv`, `summary.md`, and raw
RPT dumps/Anser JSON plans under `raw/`. Failed queries remain in the report
denominator. Keep the raw artifacts when interpreting eligibility,
optimizer fallback and gate refusals.
