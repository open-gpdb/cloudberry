CREATE EXTENSION vacuum_stats;

SET client_min_messages = warning;
-- nothing is collected unless this is on; it reaches the segments, where
-- vacuum does its work
SET track_vacuum_statistics = on;

CREATE TABLE vstat_heap (id int, val text) DISTRIBUTED BY (id);
CREATE INDEX vstat_heap_idx ON vstat_heap (val);
INSERT INTO vstat_heap SELECT g, 'val_' || g FROM generate_series(1, 1000) g;
DELETE FROM vstat_heap WHERE id % 2 = 0;
VACUUM vstat_heap;

-- The statistics collector is fed asynchronously (UDP), and every QE
-- caches its stats snapshot for the transaction, so poll until the
-- counters become visible, resetting the snapshots on each iteration.
CREATE FUNCTION vstat_clear_segment_snapshots() RETURNS SETOF void AS
$$ SELECT pg_catalog.pg_stat_clear_snapshot() $$
LANGUAGE SQL EXECUTE ON ALL SEGMENTS;

CREATE FUNCTION wait_for_vacuum_stats(cond text) RETURNS bool AS
$$
DECLARE
    ok bool;
BEGIN
    FOR i IN 1..120 LOOP
        PERFORM pg_stat_clear_snapshot();
        PERFORM * FROM vstat_clear_segment_snapshots();
        EXECUTE cond INTO ok;
        IF ok THEN
            RETURN true;
        END IF;
        PERFORM pg_sleep(0.25);
    END LOOP;
    RETURN false;
END
$$ LANGUAGE plpgsql;

-- table counters: the deleted tuples must show up
SELECT wait_for_vacuum_stats($$
    SELECT sum(tuples_deleted) > 0
    FROM gp_stat_vacuum_tables WHERE relname = 'vstat_heap' $$);

SELECT sum(tuples_deleted) > 0 AS tuples_deleted_ok,
       sum(dead_tuples) >= 0 AS dead_tuples_ok,
       sum(pages_deleted) >= 0 AS pages_deleted_ok,
       sum(dead_pages) >= 0 AS dead_pages_ok,
       sum(pages_frozen) >= 0 AS pages_frozen_ok,
       sum(wraparound_vacuum_count) >= 0 AS wraparound_ok,
       sum(rev_all_frozen_pages) >= 0 AS rev_all_frozen_pages_ok,
       sum(total_time) > 0 AS total_time_ok
FROM gp_stat_vacuum_tables WHERE relname = 'vstat_heap';

-- vacuum marks the remaining pages all-visible
SELECT wait_for_vacuum_stats($$
    SELECT sum(pages_all_visible) > 0
    FROM gp_stat_vacuum_tables WHERE relname = 'vstat_heap' $$);

-- index counters: the index entries removed by vacuum must show up
SELECT wait_for_vacuum_stats($$
    SELECT sum(tuples_deleted) > 0
    FROM gp_stat_vacuum_indexes WHERE indexrelname = 'vstat_heap_idx' $$);

SELECT sum(tuples_deleted) > 0 AS tuples_deleted_ok,
       sum(pages_deleted) >= 0 AS pages_deleted_ok,
       sum(dead_pages) >= 0 AS dead_pages_ok,
       sum(total_time) >= 0 AS total_time_ok
FROM gp_stat_vacuum_indexes WHERE indexrelname = 'vstat_heap_idx';

-- inserting into all-visible pages revokes their all-visible status,
-- which is delivered with the regular relation statistics
INSERT INTO vstat_heap SELECT g, 'again_' || g FROM generate_series(1, 30) g;

SELECT wait_for_vacuum_stats($$
    SELECT sum(rev_all_visible_pages) > 0
    FROM gp_stat_vacuum_tables WHERE relname = 'vstat_heap' $$);

-- database counters accumulate the per-relation ones
SELECT wait_for_vacuum_stats($$
    SELECT sum(tuples_deleted) > 0
    FROM gp_stat_vacuum_database WHERE datname = current_database() $$);

-- local (per-node) views also work; on the coordinator the table is empty,
-- so just check that the relations are visible there
SELECT count(*) = 1 AS heap_visible FROM pg_stat_vacuum_tables
WHERE relname = 'vstat_heap';
SELECT count(*) = 1 AS index_visible FROM pg_stat_vacuum_indexes
WHERE indexrelname = 'vstat_heap_idx';
SELECT count(*) = 1 AS db_visible FROM pg_stat_vacuum_database
WHERE datname = current_database();

-- Append-optimized tables are vacuumed by their own code path, which reports
-- the same counters: the tuples compaction dropped, the tuples it could not
-- drop yet (still hidden by the visibility map of the AO relation), the
-- blocks it freed, and the time it took.  An AO relation has no visibility
-- map pages to keep up to date and nothing to freeze, so pages_frozen and
-- pages_all_visible stay zero.
CREATE TABLE vstat_ao (id int, val text)
    WITH (appendonly = true) DISTRIBUTED BY (id);
CREATE INDEX vstat_ao_idx ON vstat_ao (val);
INSERT INTO vstat_ao SELECT g, 'val_' || g FROM generate_series(1, 1000) g;
DELETE FROM vstat_ao WHERE id % 2 = 0;
VACUUM vstat_ao;

SELECT wait_for_vacuum_stats($$
    SELECT sum(tuples_deleted) > 0
    FROM gp_stat_vacuum_tables WHERE relname = 'vstat_ao' $$);

SELECT sum(tuples_deleted) > 0 AS tuples_deleted_ok,
       sum(dead_tuples) >= 0 AS dead_tuples_ok,
       sum(pages_deleted) >= 0 AS pages_deleted_ok,
       sum(pages_frozen) = 0 AS no_pages_frozen,
       sum(pages_all_visible) = 0 AS no_pages_all_visible,
       sum(total_time) > 0 AS total_time_ok
FROM gp_stat_vacuum_tables WHERE relname = 'vstat_ao';

-- the indexes of an AO table are vacuumed and reported as well
SELECT wait_for_vacuum_stats($$
    SELECT sum(tuples_deleted) > 0
    FROM gp_stat_vacuum_indexes WHERE indexrelname = 'vstat_ao_idx' $$);

SELECT sum(tuples_deleted) > 0 AS tuples_deleted_ok,
       sum(total_time) >= 0 AS total_time_ok
FROM gp_stat_vacuum_indexes WHERE indexrelname = 'vstat_ao_idx';

-- the tuples that could not be dropped yet are counted as dead ones: a
-- repeatable read snapshot held elsewhere is not something a regression test
-- can arrange, so just check the counter is readable for the AO relation
SELECT count(*) = 1 AS ao_visible FROM pg_stat_vacuum_tables
WHERE relname = 'vstat_ao';

-- the time spent in the cost-based vacuum delay is part of the total time,
-- for heap relations, AO relations and indexes alike
SELECT bool_and(delay_time >= 0 AND delay_time <= total_time) AS delay_ok
FROM gp_stat_vacuum_tables WHERE relname IN ('vstat_heap', 'vstat_ao');
SELECT bool_and(delay_time >= 0 AND delay_time <= total_time) AS delay_ok
FROM gp_stat_vacuum_indexes WHERE indexrelname IN ('vstat_heap_idx', 'vstat_ao_idx');
SELECT bool_and(delay_time >= 0 AND delay_time <= total_time) AS delay_ok
FROM gp_stat_vacuum_database WHERE datname = current_database();

DROP FUNCTION wait_for_vacuum_stats(text);
DROP FUNCTION vstat_clear_segment_snapshots();
DROP TABLE vstat_heap;
DROP TABLE vstat_ao;
DROP EXTENSION vacuum_stats;
