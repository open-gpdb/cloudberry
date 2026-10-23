-- Region discovery: transfer regions are connected components of the join
-- graph over base scans, and every operator the key resolver cannot descend
-- (aggregate, set op, window, limit, SRF/UNNEST, materialized CTE, partitioned
-- table) is a boundary -- queries whose only join key comes through one have
-- no region at all.
CREATE EXTENSION robust_predicate_transfer;

CREATE TABLE rpt_a (a int, b int) DISTRIBUTED BY (a);
CREATE TABLE rpt_b (a int) DISTRIBUTED BY (a);
CREATE TABLE rpt_c (a int) DISTRIBUTED BY (a);
INSERT INTO rpt_a SELECT i, i FROM generate_series(1, 100) i;
INSERT INTO rpt_b SELECT i FROM generate_series(1, 10) i;
INSERT INTO rpt_c SELECT i FROM generate_series(1, 50) i;
ANALYZE rpt_a;
ANALYZE rpt_b;
ANALYZE rpt_c;

-- A partitioned table joins through an Append, which resolution cannot cross.
CREATE TABLE rpt_p (a int) DISTRIBUTED BY (a)
    PARTITION BY RANGE (a) (START (1) END (101) EVERY (50));
INSERT INTO rpt_p SELECT i FROM generate_series(1, 100) i;
ANALYZE rpt_p;

-- No relations at all.
SELECT rpt_transfer_plan('SELECT 1');

-- A plain join: one region, both vertices unfiltered.
SELECT rpt_transfer_plan('SELECT * FROM rpt_b JOIN rpt_a ON rpt_b.a = rpt_a.a');

-- Join key produced by GROUP BY: SubqueryScan boundary, no region.
SELECT rpt_transfer_plan('SELECT * FROM rpt_a a JOIN (SELECT a, count(*) c FROM rpt_b GROUP BY a) s ON a.a = s.a');

-- Join key through UNION: Append boundary, no region.
SELECT rpt_transfer_plan('SELECT * FROM rpt_a a JOIN (SELECT a FROM rpt_b UNION SELECT a FROM rpt_c) u ON a.a = u.a');

-- Join key through a window function: boundary, no region.
SELECT rpt_transfer_plan('SELECT * FROM rpt_a a JOIN (SELECT a, row_number() OVER () r FROM rpt_b) w ON a.a = w.a');

-- Join key through LIMIT: boundary, no region.
SELECT rpt_transfer_plan('SELECT * FROM rpt_a a JOIN (SELECT a FROM rpt_b ORDER BY a LIMIT 5) l ON a.a = l.a');

-- Join key produced by a set-returning function (UNNEST): ProjectSet boundary.
SELECT rpt_transfer_plan('SELECT * FROM rpt_a a JOIN (SELECT u FROM rpt_b, unnest(ARRAY[a]) u) s ON a.a = s.u');

-- Join key through a materialized CTE: CteScan boundary.
SELECT rpt_transfer_plan('WITH w AS MATERIALIZED (SELECT a FROM rpt_b) SELECT * FROM rpt_a a JOIN w ON a.a = w.a');

-- Join against a partitioned table: Append boundary.
SELECT rpt_transfer_plan('SELECT * FROM rpt_a a JOIN rpt_p p ON a.a = p.a');

DROP TABLE rpt_p;
DROP TABLE rpt_a, rpt_b, rpt_c;
DROP EXTENSION robust_predicate_transfer;
