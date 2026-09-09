/* contrib/vacuum_stats/vacuum_stats--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION vacuum_stats" to load this file. \quit

--
-- Per-relation accessor functions (tables and indexes).
--
CREATE FUNCTION pg_stat_get_vacuum_tuples_deleted(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_vacuum_tuples_deleted'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_vacuum_dead_tuples(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_vacuum_dead_tuples'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_vacuum_pages_deleted(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_vacuum_pages_deleted'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_vacuum_dead_pages(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_vacuum_dead_pages'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_vacuum_pages_frozen(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_vacuum_pages_frozen'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_vacuum_pages_all_visible(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_vacuum_pages_all_visible'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_vacuum_wraparound_count(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_vacuum_wraparound_count'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_vacuum_rev_all_frozen_pages(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_vacuum_rev_all_frozen_pages'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_vacuum_rev_all_visible_pages(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_vacuum_rev_all_visible_pages'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_vacuum_delay_time(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_vacuum_delay_time'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_vacuum_total_time(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_vacuum_total_time'
LANGUAGE C STABLE STRICT;

--
-- Per-database accessor functions.
--
CREATE FUNCTION pg_stat_get_db_vacuum_tuples_deleted(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_db_vacuum_tuples_deleted'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_db_vacuum_dead_tuples(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_db_vacuum_dead_tuples'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_db_vacuum_pages_deleted(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_db_vacuum_pages_deleted'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_db_vacuum_dead_pages(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_db_vacuum_dead_pages'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_db_vacuum_pages_frozen(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_db_vacuum_pages_frozen'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_db_vacuum_pages_all_visible(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_db_vacuum_pages_all_visible'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_db_vacuum_wraparound_count(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_db_vacuum_wraparound_count'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_db_vacuum_rev_all_frozen_pages(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_db_vacuum_rev_all_frozen_pages'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_db_vacuum_rev_all_visible_pages(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_db_vacuum_rev_all_visible_pages'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_db_vacuum_delay_time(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_db_vacuum_delay_time'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION pg_stat_get_db_vacuum_total_time(oid) RETURNS int8
AS 'MODULE_PATHNAME', 'pg_stat_get_db_vacuum_total_time'
LANGUAGE C STABLE STRICT;

--
-- Local (per-node) views.  total_time is exposed in milliseconds, like
-- the other timing columns of the cumulative statistics views.
--
CREATE VIEW pg_stat_vacuum_tables AS
    SELECT
        c.oid AS relid,
        n.nspname AS schemaname,
        c.relname AS relname,
        pg_stat_get_vacuum_tuples_deleted(c.oid) AS tuples_deleted,
        pg_stat_get_vacuum_dead_tuples(c.oid) AS dead_tuples,
        pg_stat_get_vacuum_pages_deleted(c.oid) AS pages_deleted,
        pg_stat_get_vacuum_dead_pages(c.oid) AS dead_pages,
        pg_stat_get_vacuum_pages_frozen(c.oid) AS pages_frozen,
        pg_stat_get_vacuum_pages_all_visible(c.oid) AS pages_all_visible,
        pg_stat_get_vacuum_rev_all_frozen_pages(c.oid) AS rev_all_frozen_pages,
        pg_stat_get_vacuum_rev_all_visible_pages(c.oid) AS rev_all_visible_pages,
        pg_stat_get_vacuum_wraparound_count(c.oid) AS wraparound_vacuum_count,
        pg_stat_get_vacuum_total_time(c.oid) / 1000.0 AS total_time,
        pg_stat_get_vacuum_delay_time(c.oid) / 1000.0 AS delay_time
    FROM pg_catalog.pg_class c
        LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
    WHERE c.relkind IN ('r', 't', 'm');

CREATE VIEW pg_stat_vacuum_indexes AS
    SELECT
        c.oid AS relid,
        i.oid AS indexrelid,
        n.nspname AS schemaname,
        c.relname AS relname,
        i.relname AS indexrelname,
        pg_stat_get_vacuum_tuples_deleted(i.oid) AS tuples_deleted,
        pg_stat_get_vacuum_dead_tuples(i.oid) AS dead_tuples,
        pg_stat_get_vacuum_pages_deleted(i.oid) AS pages_deleted,
        pg_stat_get_vacuum_dead_pages(i.oid) AS dead_pages,
        pg_stat_get_vacuum_pages_frozen(i.oid) AS pages_frozen,
        pg_stat_get_vacuum_pages_all_visible(i.oid) AS pages_all_visible,
        pg_stat_get_vacuum_rev_all_frozen_pages(i.oid) AS rev_all_frozen_pages,
        pg_stat_get_vacuum_rev_all_visible_pages(i.oid) AS rev_all_visible_pages,
        pg_stat_get_vacuum_wraparound_count(i.oid) AS wraparound_vacuum_count,
        pg_stat_get_vacuum_total_time(i.oid) / 1000.0 AS total_time,
        pg_stat_get_vacuum_delay_time(i.oid) / 1000.0 AS delay_time
    FROM pg_catalog.pg_class c
        JOIN pg_catalog.pg_index x ON c.oid = x.indrelid
        JOIN pg_catalog.pg_class i ON i.oid = x.indexrelid
        LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
    WHERE c.relkind IN ('r', 't', 'm') AND i.relkind = 'i';

CREATE VIEW pg_stat_vacuum_database AS
    SELECT
        d.oid AS datid,
        d.datname AS datname,
        pg_stat_get_db_vacuum_tuples_deleted(d.oid) AS tuples_deleted,
        pg_stat_get_db_vacuum_dead_tuples(d.oid) AS dead_tuples,
        pg_stat_get_db_vacuum_pages_deleted(d.oid) AS pages_deleted,
        pg_stat_get_db_vacuum_dead_pages(d.oid) AS dead_pages,
        pg_stat_get_db_vacuum_pages_frozen(d.oid) AS pages_frozen,
        pg_stat_get_db_vacuum_pages_all_visible(d.oid) AS pages_all_visible,
        pg_stat_get_db_vacuum_rev_all_frozen_pages(d.oid) AS rev_all_frozen_pages,
        pg_stat_get_db_vacuum_rev_all_visible_pages(d.oid) AS rev_all_visible_pages,
        pg_stat_get_db_vacuum_wraparound_count(d.oid) AS wraparound_vacuum_count,
        pg_stat_get_db_vacuum_total_time(d.oid) / 1000.0 AS total_time,
        pg_stat_get_db_vacuum_delay_time(d.oid) / 1000.0 AS delay_time
    FROM pg_catalog.pg_database d;

GRANT SELECT ON pg_stat_vacuum_tables, pg_stat_vacuum_indexes,
    pg_stat_vacuum_database TO PUBLIC;
