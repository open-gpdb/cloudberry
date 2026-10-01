# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# Test vacuum counter semantics, controls and lifecycle in one TAP suite.
# Adapt the repeatable-read and visibility-map scenarios from v44:
# https://www.postgresql.org/message-id/flat/cb305107-5935-4c34-9847-6ff0fef89f06%40yandex.ru#8b78d4ab399e37afc5eb2cee8a54d2f6
# This branch has a UDP collector and the older pg_stat_vacuum_* API:
# dead_tuples corresponds to recently_dead_tuples, and pages_frozen and
# rev_* expose the VM transitions.  Each query opens a new transaction,
# so polling does not reuse a cached statistics snapshot.
# PostgresNode runs in utility/maintenance mode; use the local views.

use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More;

my $node = get_new_node('vacuum_stats');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
track_counts = on
vacuum_cost_delay = 0
});
$node->start;
$node->safe_psql('postgres', q{
CREATE EXTENSION vacuum_stats;
CREATE TABLE vstat_barrier (id int) WITH (autovacuum_enabled = off);
});

my @counter_names = qw(tuples_deleted dead_tuples pages_deleted dead_pages
  pages_frozen pages_all_visible rev_all_frozen_pages rev_all_visible_pages
  wraparound_vacuum_count total_time delay_time);
my $all_counters = join(', ', @counter_names);
my $all_zero = join(' AND ', map { "$_ = 0" } @counter_names);

sub wait_for_stats
{
	my ($sql, $description, $expected) = @_;
	$expected = 't' unless defined $expected;
	$node->poll_query_until('postgres', $sql, $expected)
	  or BAIL_OUT("timed out waiting for $description: $sql");
}

# VACUUM sends vacuum_count before its extended counters.  Instead of
# using that earlier report, send ANALYZE from the same backend AFTER the
# command and wait for its report.  ANALYZE adds no vacuum counters, so it
# also works for database totals, disabled tracking, resets and VACUUM FULL.
# DML counters are buffered separately; wait for the affected table's DML
# report explicitly when checking rev_* counters.
sub run_and_wait
{
	my ($sql) = @_;
	my $count = $node->safe_psql('postgres',
		"SELECT analyze_count FROM pg_stat_all_tables WHERE relname = 'vstat_barrier'");
	my ($result, $stdout, $stderr) = $node->psql('postgres',
		"$sql;\nANALYZE vstat_barrier;", on_error_die => 1);
	wait_for_stats(
		"SELECT analyze_count = $count + 1 FROM pg_stat_all_tables WHERE relname = 'vstat_barrier'",
		"collector report after $sql");
	return $stderr;
}

sub wait_for_updates
{
	my ($table, $count) = @_;
	wait_for_stats(
		"SELECT n_tup_upd = $count FROM pg_stat_all_tables WHERE relname = '$table'",
		"UPDATE report for $table");
}

sub vacuum_table
{
	my ($table, $options, $settings) = @_;
	$options ||= '';
	$settings ||= '';
	return run_and_wait("$settings VACUUM $options $table");
}

sub counters
{
	my ($table, $columns) = @_;
	$columns ||= $all_counters;
	return $node->safe_psql('postgres',
		"SELECT $columns FROM pg_stat_vacuum_tables WHERE relname = '$table'");
}

sub index_counters
{
	my ($index, $columns) = @_;
	$columns ||= $all_counters;
	return $node->safe_psql('postgres',
		"SELECT $columns FROM pg_stat_vacuum_indexes WHERE indexrelname = '$index'");
}

sub database_counters
{
	my ($columns) = @_;
	$columns ||= $all_counters;
	return $node->safe_psql('postgres',
		"SELECT $columns FROM pg_stat_vacuum_database WHERE datname = 'postgres'");
}

subtest 'tracking disabled and enabled' => sub {
	$node->safe_psql('postgres', q{
CREATE TABLE vstat_off (id int) WITH (autovacuum_enabled = off);
INSERT INTO vstat_off SELECT generate_series(1, 1000);
DELETE FROM vstat_off;
});
	my $verbose = vacuum_table('vstat_off', 'VERBOSE');
	like($verbose, qr/table "vstat_off": vacuum statistics.*?cost-based delay: 0\.000 ms/s,
		'VERBOSE reports measurements even with statistics tracking disabled');
	is(counters('vstat_off', $all_zero), 't',
		'extended counters stay zero while tracking is disabled');
	is($node->safe_psql('postgres',
		"SELECT vacuum_count FROM pg_stat_all_tables WHERE relname = 'vstat_off'"),
		'1', 'ordinary vacuum statistics are still collected');
	# Reading zero counters must not create a vacuum statistics hash table.
	is($node->safe_psql('postgres', q{
BEGIN;
DO $$
BEGIN
    PERFORM count(*) FROM pg_stat_vacuum_tables WHERE total_time > 0;
END
$$;
SELECT count(*) FROM pg_backend_memory_contexts
WHERE name = 'Per-database vacuum statistics';
COMMIT;
}), '0', 'disabled tracking does not allocate a vacuum statistics hash table');
	$node->safe_psql('postgres', 'ALTER SYSTEM SET track_vacuum_statistics = on');
	$node->reload;
	wait_for_stats('SHOW track_vacuum_statistics',
		'track_vacuum_statistics to be enabled', 'on');
};

subtest 'removed tuples and truncated pages' => sub {
	$node->safe_psql('postgres', q{
CREATE TABLE vstat_heap (id int PRIMARY KEY) WITH (autovacuum_enabled = off);
INSERT INTO vstat_heap SELECT generate_series(1, 10000);
DELETE FROM vstat_heap WHERE id % 2 = 0;
});
	is(counters('vstat_heap', $all_zero), 't',
		'all counters are zero before the first vacuum');
	# Heap insertion can pre-extend the relation with empty tail pages.
	# Disable truncation in this run to make its zero counter deterministic.
	my $verbose = vacuum_table('vstat_heap', '(VERBOSE, TRUNCATE false)');
	is(counters('vstat_heap', 'tuples_deleted, dead_tuples, pages_deleted, dead_pages'),
		'5000|0|0|0', 'vacuum removes exactly half the rows without truncation');
	like($verbose, qr/pages with dead tuples not yet removable: 0\n/,
		'VERBOSE reports zero pages retaining dead tuples');
	like($verbose, qr/pages with tuples frozen: 0\n/,
		'VERBOSE reports zero pages frozen with the default freeze age');
	my $visible = counters('vstat_heap', 'pages_all_visible');
	like($verbose, qr/pages marked all-visible: \Q$visible\E\n/,
		'VERBOSE all-visible count matches the first vacuum report');
	like($verbose, qr/scanned index "vstat_heap_pkey".*?cost-based delay: 0\.000 ms/s,
		'VERBOSE reports the index cost delay');
	is(index_counters('vstat_heap_pkey', 'tuples_deleted, pages_deleted'),
		'5000|0', 'every other index key remains; no index pages are deleted');
	is(counters('vstat_heap', 'total_time > 0, delay_time = 0'),
		't|t', 'heap vacuum takes time but has no cost delay');
	is(index_counters('vstat_heap_pkey', 'total_time > 0, delay_time = 0'),
		't|t', 'index vacuum takes time but has no cost delay');

	$node->safe_psql('postgres', 'DELETE FROM vstat_heap');
	my $quiet = vacuum_table('vstat_heap');
	unlike($quiet, qr/vacuum statistics|pages marked all-visible|cost-based delay/,
		'ordinary VACUUM does not emit VERBOSE statistics at the default message level');
	is(counters('vstat_heap', 'tuples_deleted, dead_tuples, pages_deleted > 0'),
		'10000|0|t', 'the second vacuum removes the remaining rows and truncates the heap');
	is(index_counters('vstat_heap_pkey', 'tuples_deleted, pages_deleted > 0'),
		'10000|t', 'index tuple and page deletion counters accumulate');
};

subtest 'a repeatable-read snapshot prevents removal' => sub {
	$node->safe_psql('postgres', q{
CREATE TABLE vstat_snapshot (id int PRIMARY KEY, val int)
    WITH (autovacuum_enabled = off);
INSERT INTO vstat_snapshot SELECT i, i FROM generate_series(1, 1000) g(i);
});
	my ($in, $out) = ('', '');
	my $timer = IPC::Run::timeout($TestLib::timeout_default);
	my $reader = $node->background_psql('postgres', \$in, \$out, $timer);
	$in = "BEGIN ISOLATION LEVEL REPEATABLE READ;\n"
	  . "SELECT count(*) FROM vstat_snapshot;\n\\echo snapshot_ready\n";
	$reader->pump until $out =~ /snapshot_ready/;

	$node->safe_psql('postgres', 'UPDATE vstat_snapshot SET val = val + 1 WHERE id > 900');
	my $verbose = vacuum_table('vstat_snapshot', 'VERBOSE');
	is(counters('vstat_snapshot', 'tuples_deleted, dead_tuples, dead_pages > 0, pages_frozen'),
		'0|100|t|0', '100 old tuple versions remain while the snapshot is held');
	my $dead_pages = counters('vstat_snapshot', 'dead_pages');
	like($verbose, qr/pages with dead tuples not yet removable: \Q$dead_pages\E\n/,
		'VERBOSE reports the pages held back by the snapshot');
	is(index_counters('vstat_snapshot_pkey', 'tuples_deleted'),
		'0', 'index entries needed by the reader are retained');

	$in = "COMMIT;\n\\q\n";
	$reader->finish;
	vacuum_table('vstat_snapshot');
	is(counters('vstat_snapshot', 'tuples_deleted, dead_tuples, pages_frozen'),
		'100|100|0', 'after commit, 100 versions are removed; cumulative dead_tuples stays at 100');
};

subtest 'freezing, clearing and restoring visibility-map flags' => sub {
	$node->safe_psql('postgres', q{
CREATE TABLE vstat_vm (id int PRIMARY KEY, val int)
    WITH (autovacuum_enabled = off, fillfactor = 70);
INSERT INTO vstat_vm SELECT i, i FROM generate_series(1, 5000) g(i);
});
	my $verbose = vacuum_table('vstat_vm', '(FREEZE, VERBOSE)');
	is(counters('vstat_vm', 'pages_frozen > 0, pages_all_visible > 0, rev_all_frozen_pages, rev_all_visible_pages'),
		't|t|0|0', 'FREEZE sets both VM flags without clearing them');
	is(counters('vstat_vm', 'wraparound_vacuum_count'), '1',
		'FREEZE counts the vacuum made aggressive by the freeze age');
	my $marks = counters('vstat_vm', 'pages_frozen, pages_all_visible');
	my ($frozen, $visible) = split /\|/, $marks;
	like($verbose, qr/pages with tuples frozen: \Q$frozen\E\n/,
		'VERBOSE freezing count matches the first vacuum report');
	like($verbose, qr/pages marked all-visible: \Q$visible\E\n/,
		'VERBOSE all-visible count matches the first FREEZE report');
	like($verbose, qr/aggressive scan required by freeze age: yes/,
		'VERBOSE explains freeze-age-driven aggressive scanning');

	$verbose = vacuum_table('vstat_vm', '(FREEZE, VERBOSE)');
	is(counters('vstat_vm', 'pages_frozen, pages_all_visible'),
		$marks, 'another FREEZE does not count already-set flags again');
	like($verbose, qr/pages with tuples frozen: 0\n/,
		'VERBOSE reports freezing work for this run, not cumulative totals');
	like($verbose, qr/pages marked all-visible: 0\n/,
		'VERBOSE reports all-visible changes for this run, not cumulative totals');

	# Change an indexed column to avoid HOT updates.  Wait for the DML
	# report before checking VM counters, which travel with that report.
	$node->safe_psql('postgres', 'UPDATE vstat_vm SET id = id + 10000');
	wait_for_updates('vstat_vm', 5000);
	is(counters('vstat_vm', 'rev_all_frozen_pages > 0, rev_all_visible_pages > 0'),
		't|t', 'UPDATE clears both flags on previously frozen pages');
	my $cleared = counters('vstat_vm', 'rev_all_frozen_pages, rev_all_visible_pages');

	vacuum_table('vstat_vm', 'FREEZE');
	is(counters('vstat_vm', "pages_frozen > $frozen, pages_all_visible > $visible"),
		't|t', 'FREEZE restores flags and counts the new transitions');
	is(counters('vstat_vm', 'rev_all_frozen_pages, rev_all_visible_pages'),
		$cleared, 'restoring flags does not increase the cleared counters');
};

subtest 'append-optimized compaction' => sub {
	for my $am ('ao_row', 'ao_column')
	{
		my $table = "vstat_$am";
		$node->safe_psql('postgres', qq{
CREATE TABLE $table (id int, val int) USING $am;
CREATE INDEX ${table}_idx ON $table (id);
INSERT INTO $table SELECT i, i FROM generate_series(1, 10000) g(i);
DELETE FROM $table WHERE id % 2 = 0;
});
		# Preserve the zero-freeze-age case: AO still must not count a
		# wraparound vacuum, unlike its auxiliary heap relations.
		my $verbose = vacuum_table($table, 'VERBOSE', 'SET vacuum_freeze_table_age = 0;');
		is(counters($table, 'tuples_deleted, dead_tuples, pages_frozen, pages_all_visible, wraparound_vacuum_count'),
			'5000|0|0|0|0', "$am compaction removes 5000 rows and has no heap VM or wraparound work");
		# Compaction relocates surviving tuples, so all original index
		# entries, including those of surviving rows, become obsolete.
		is(index_counters("${table}_idx", 'tuples_deleted'),
			'10000', "$am index cleanup removes the old TIDs");
		is(counters($table, 'total_time > 0, delay_time = 0'),
			't|t', "$am compaction takes time without cost delay");
		my $time = sprintf('%.3f', counters($table, 'total_time'));
		like($verbose,
			qr/append-optimized table "\Q$table\E": vacuum statistics\nDETAIL:  0 dead tuples remain\.\nelapsed: \Q$time\E ms, cost-based delay: 0\.000 ms/,
			"$am VERBOSE reports remaining dead tuples and accumulated phase time");
		is(counters($table, 'pages_deleted > 0, dead_pages, rev_all_frozen_pages, rev_all_visible_pages'),
			't|0|0|0', "$am reports freed space without heap page or VM counters");

		# Without obsolete segment files, AO still runs index cleanup.
		# Its time must be reported even if the AM returns no page statistics.
		my $index_time = index_counters("${table}_idx", 'total_time');
		vacuum_table($table);
		is(index_counters("${table}_idx", "tuples_deleted, total_time > $index_time, delay_time = 0"),
			'10000|t|t', "$am reports index cleanup without deleting more TIDs");

		$node->safe_psql('postgres', "DELETE FROM $table WHERE id <= 200");
		vacuum_table($table, '', 'SET gp_appendonly_compaction = off;');
		is(counters($table, 'tuples_deleted, dead_tuples'),
			'5000|100', "$am counts hidden rows left when compaction is disabled");
	}
};

subtest 'VACUUM FULL leaves the extended counters unchanged' => sub {
	$node->safe_psql('postgres', q{
CREATE TABLE vstat_full (id int PRIMARY KEY) WITH (autovacuum_enabled = off);
INSERT INTO vstat_full SELECT generate_series(1, 10000);
DELETE FROM vstat_full WHERE id % 2 = 0;
});
	vacuum_table('vstat_full');
	is(counters('vstat_full', 'tuples_deleted'), '5000',
		'plain vacuum establishes nonzero counters');
	$node->safe_psql('postgres', 'DELETE FROM vstat_full');
	wait_for_stats(
		"SELECT n_tup_del = 10000 FROM pg_stat_all_tables WHERE relname = 'vstat_full'",
		'DELETE report before VACUUM FULL');
	my $table_before = counters('vstat_full');
	my $index_before = index_counters('vstat_full_pkey');
	run_and_wait('VACUUM FULL vstat_full');
	is(counters('vstat_full'), $table_before, 'VACUUM FULL leaves table counters unchanged');
	is(index_counters('vstat_full_pkey'), $index_before, 'VACUUM FULL leaves index counters unchanged');
};

subtest 'cost-based delay is part of total time' => sub {
	$node->safe_psql('postgres', q{
CREATE TABLE vstat_delay (id int) WITH (autovacuum_enabled = off);
INSERT INTO vstat_delay SELECT generate_series(1, 10000);
DELETE FROM vstat_delay;
});
	# Session-local settings do not leak into later scenarios.
	my $verbose = vacuum_table('vstat_delay', 'VERBOSE',
		'SET vacuum_cost_delay = 1; SET vacuum_cost_limit = 1;');
	is(counters('vstat_delay', 'delay_time > 0 AND delay_time <= total_time'),
		't', 'the measured cost delay is positive and included in total time');
	my $time = sprintf('%.3f', counters('vstat_delay', 'total_time'));
	my $delay = sprintf('%.3f', counters('vstat_delay', 'delay_time'));
	like($verbose, qr/elapsed: \Q$time\E ms, cost-based delay: \Q$delay\E ms/,
		'VERBOSE timing matches the vacuum report, including nonzero cost delay');
};

subtest 'index pages are not counted again by later vacuums' => sub {
	$node->safe_psql('postgres', q{
CREATE TABLE vstat_idx (id int PRIMARY KEY) WITH (autovacuum_enabled = off);
INSERT INTO vstat_idx SELECT generate_series(1, 100000);
DELETE FROM vstat_idx WHERE id <= 90000;
});
	vacuum_table('vstat_idx');
	my $pages = index_counters('vstat_idx_pkey', 'pages_deleted');
	cmp_ok($pages, '>', 0, 'the first vacuum deleted index pages');
	$node->safe_psql('postgres', 'DELETE FROM vstat_idx WHERE id > 99000');
	vacuum_table('vstat_idx');
	is(index_counters('vstat_idx_pkey', 'tuples_deleted'), '91000',
		'the next vacuum removed exactly 1000 more index entries');
	is(index_counters('vstat_idx_pkey', "pages_deleted - $pages < $pages / 2"),
		't', 'pages deleted by the first run are not counted again');
};

subtest 'reset preserves ordinary statistics and other relations' => sub {
	$node->safe_psql('postgres', q{
CREATE TABLE vstat_reset (id int PRIMARY KEY) WITH (autovacuum_enabled = off);
CREATE TABLE vstat_keep (id int) WITH (autovacuum_enabled = off);
INSERT INTO vstat_reset SELECT generate_series(1, 1000);
INSERT INTO vstat_keep SELECT generate_series(1, 1000);
DELETE FROM vstat_keep;
});
	vacuum_table('vstat_keep');
	vacuum_table('vstat_reset', 'FREEZE');
	$node->safe_psql('postgres', 'UPDATE vstat_reset SET id = id + 1000');
	wait_for_updates('vstat_reset', 1000);
	vacuum_table('vstat_reset', 'FREEZE');
	is(counters('vstat_reset',
		'tuples_deleted, rev_all_frozen_pages > 0, rev_all_visible_pages > 0'),
		'1000|t|t', 'populate removal and VM revision counters before reset');
	my $keep_before = counters('vstat_keep');
	my $index_before = index_counters('vstat_reset_pkey');
	my $db_before = database_counters();
	my $ordinary_query = "SELECT vacuum_count, n_tup_upd FROM pg_stat_all_tables WHERE relname = 'vstat_reset'";
	my $ordinary_before = $node->safe_psql('postgres', $ordinary_query);
	my $reset_before = counters('vstat_reset');

	my ($result, $stdout, $stderr) = $node->psql('postgres',
		'SELECT vacuum_stats_reset(0::oid)');
	is($result, 3, 'an invalid relation OID is rejected');
	like($stderr, qr/invalid relation OID: 0/, 'invalid OID has a specific error');
	run_and_wait('SELECT vacuum_stats_reset(NULL::oid)');
	is(counters('vstat_reset'), $reset_before,
		'invalid and NULL OIDs do not reset a relation');
	is(database_counters(), $db_before,
		'invalid and NULL OIDs do not reset database totals');

	run_and_wait("SELECT vacuum_stats_reset('vstat_reset'::regclass::oid)");
	is(counters('vstat_reset', $all_zero), 't', 'relation reset clears every vacuum counter including rev_*');
	is(counters('vstat_keep'), $keep_before, 'relation reset preserves another table');
	is(index_counters('vstat_reset_pkey'), $index_before, 'relation reset preserves the index counters');
	is(database_counters(), $db_before, 'relation reset preserves database totals');
	is($node->safe_psql('postgres', $ordinary_query), $ordinary_before,
		'relation reset preserves ordinary statistics');

	# Refill the reset relation, including both VM revision counters,
	# before checking the database-wide reset.
	$node->safe_psql('postgres', 'UPDATE vstat_reset SET id = id + 1000');
	wait_for_updates('vstat_reset', 2000);
	vacuum_table('vstat_reset', 'FREEZE');
	is(counters('vstat_reset',
		'tuples_deleted, rev_all_frozen_pages > 0, rev_all_visible_pages > 0'),
		'1000|t|t', 'the counters are nonzero again before database reset');
	$ordinary_before = $node->safe_psql('postgres', $ordinary_query);
	run_and_wait('SELECT vacuum_stats_reset()');
	is($node->safe_psql('postgres', "SELECT bool_and($all_zero) FROM pg_stat_vacuum_tables"),
		't', 'database reset clears all table counters');
	is($node->safe_psql('postgres', "SELECT bool_and($all_zero) FROM pg_stat_vacuum_indexes"),
		't', 'database reset clears all index counters');
	is(database_counters($all_zero), 't', 'database reset clears database totals');
	is($node->safe_psql('postgres', $ordinary_query), $ordinary_before,
		'database reset preserves ordinary statistics');


};

subtest 'database totals do not count index work twice' => sub {
	$node->safe_psql('postgres', q{
CREATE TABLE vstat_db (id int PRIMARY KEY, val int) WITH (autovacuum_enabled = off);
CREATE INDEX ON vstat_db (val);
INSERT INTO vstat_db SELECT i, i FROM generate_series(1, 10000) g(i);
DELETE FROM vstat_db WHERE id % 2 = 0;
});
	run_and_wait('SELECT vacuum_stats_reset()');
	vacuum_table('vstat_db');
	is($node->safe_psql('postgres',
		"SELECT sum(tuples_deleted) FROM pg_stat_vacuum_indexes WHERE relname = 'vstat_db'"),
		'10000', 'both indexes report 5000 removed entries');
	is(database_counters('tuples_deleted'), '5000', 'database totals count only heap tuples');
	is(database_counters(), counters('vstat_db'),
		'all database vacuum counters match the only table vacuumed since reset');
};

subtest 'statistics snapshots release their vacuum counters' => sub {
	# Keep the last snapshot alive until the context count is read.
	# Outside this transaction its context would already have been freed.
	is($node->safe_psql('postgres', q{
BEGIN;
DO $$
BEGIN
    FOR i IN 1..50 LOOP
        PERFORM pg_stat_clear_snapshot();
        PERFORM count(*) FROM pg_stat_vacuum_tables WHERE tuples_deleted > 0;
    END LOOP;
END
$$;
SELECT count(*) FROM pg_backend_memory_contexts
WHERE name = 'Per-database vacuum statistics';
COMMIT;
}), '1', 'only the current snapshot owns a vacuum statistics hash table');
};

subtest 'reset targets shared catalogs independently' => sub {
	# Shared catalogs have a separate collector entry, not the current DB's.
	# Create actual dead index entries instead of timing an empty cleanup.
	$node->safe_psql('postgres', 'CREATE DATABASE vstat_shared_reset');
	$node->safe_psql('postgres', 'DROP DATABASE vstat_shared_reset');
	vacuum_table('pg_database', '(FREEZE, INDEX_CLEANUP ON)');
	is(counters('pg_database', 'total_time > 0'), 't',
		'populate vacuum counters for a shared catalog');
	my $shared_before = counters('pg_database');
	run_and_wait('SELECT vacuum_stats_reset()');
	is(counters('pg_database'), $shared_before,
		'database reset leaves shared catalog counters alone');

	vacuum_table('vstat_reset');
	my $reset_before = counters('vstat_reset');
	my $db_before = database_counters();
	run_and_wait("SELECT vacuum_stats_reset('pg_database'::regclass::oid)");
	is(counters('pg_database', $all_zero), 't',
		'relation reset reaches the shared catalog entry');
	is(counters('vstat_reset'), $reset_before,
		'shared relation reset leaves current-database relations alone');
	is(database_counters(), $db_before,
		'shared relation reset leaves current-database totals alone');

	# Indexes are separate reset targets, including shared catalog indexes.
	is(index_counters('pg_database_oid_index', 'tuples_deleted > 0'), 't',
		'resetting the shared table preserves its index counters');
	run_and_wait(q{
SELECT vacuum_stats_reset(indexrelid) FROM pg_index
WHERE indrelid = 'pg_database'::regclass
});
	is(index_counters('pg_database_oid_index', $all_zero), 't',
		'relation reset also reaches shared index counters');
};

subtest 'clean restart preserves statistics; crash recovery resets them' => sub {
	$node->safe_psql('postgres', q{
CREATE TABLE vstat_restart (id int PRIMARY KEY) WITH (autovacuum_enabled = off);
INSERT INTO vstat_restart SELECT generate_series(1, 1000);
DELETE FROM vstat_restart WHERE id % 2 = 0;
});
	vacuum_table('vstat_restart', 'FREEZE');
	is(counters('vstat_restart', 'tuples_deleted'), '500', 'table counters are populated before restart');
	is(index_counters('vstat_restart_pkey', 'tuples_deleted'), '500', 'index counters are populated before restart');
	my $table_before = counters('vstat_restart');
	my $index_before = index_counters('vstat_restart_pkey');
	my $db_before = database_counters();
	$node->restart;
	is(counters('vstat_restart'), $table_before, 'table counters survive a clean restart');
	is(index_counters('vstat_restart_pkey'), $index_before, 'index counters survive a clean restart');
	is(database_counters(), $db_before, 'database counters survive a clean restart');
	$node->stop('immediate');
	$node->start;
	is(counters('vstat_restart', $all_zero), 't', 'crash recovery resets table counters');
	is(index_counters('vstat_restart_pkey', $all_zero), 't', 'crash recovery resets index counters');
	is(database_counters($all_zero), 't', 'crash recovery resets database counters');
};

$node->stop;
done_testing();
