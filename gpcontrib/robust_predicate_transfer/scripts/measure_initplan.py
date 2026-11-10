#!/usr/bin/env python3
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements. See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership. The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License. You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied. See the License for the
# specific language governing permissions and limitations
# under the License.

"""Emit a psql workload comparing the exact empty reducer InitPlan off/on."""
import argparse

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--samples", type=int, default=100)
parser.add_argument("--warmups", type=int, default=10)
args = parser.parse_args()
if args.samples < 1 or args.warmups < 0:
    parser.error("samples must be positive and warmups nonnegative")

print(r"\set ON_ERROR_STOP on")
print("SET anser.runtime_filter = off;")
print("SET anser.debug = off;")
print("SET rpt.enable = off;")
print("SET rpt.debug = off;")
print("SET rpt.debug_noop_initplan = off;")
print("SET statement_timeout = '120s';")
print("CREATE TEMP TABLE rpt_initplan_bench (k int) DISTRIBUTED RANDOMLY;")
print("INSERT INTO rpt_initplan_bench SELECT i FROM generate_series(1, 3000) g(i);")
print("ANALYZE rpt_initplan_bench;")
query = "SELECT count(*) FROM rpt_initplan_bench"
for optimizer in ("off", "on"):
    print(f"SET optimizer = {optimizer};")
    print("SET rpt.debug_noop_initplan = on;")
    print(f"EXPLAIN (COSTS OFF) {query};")
    # Also exercise a pre-existing InitPlan and its PARAM_EXEC numbering.
    print("SELECT count(*) FROM rpt_initplan_bench "
          "WHERE k <= (SELECT max(k) FROM rpt_initplan_bench);")
    print(r"\timing on")
    for phase, count in (("warmup", args.warmups), ("measured", args.samples)):
        for index in range(1, count + 1):
            # Alternate which arm goes first to reduce systematic drift.
            arms = ("off", "on") if index % 2 else ("on", "off")
            for arm in arms:
                print(f"SET rpt.debug_noop_initplan = {arm};")
                print(f"\\echo SAMPLE initplan {optimizer} {phase} {index} {arm}")
                print(f"EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) {query};")
    print(r"\timing off")
print("SET rpt.debug_noop_initplan = off;")
print("DROP TABLE rpt_initplan_bench;")
