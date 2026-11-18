#--------------------------------------------------------------------------
#
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
#
#--------------------------------------------------------------------------

# Opt-in schema-restore coverage. GP6_TEST_CONNSTR must name a disposable
# GP6 coordinator where this test can create and drop its own database.
# Target binaries must be on PATH; the target utility node is freshly initialized.
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More;
use IPC::Run;

plan skip_all => 'set GP6_TEST_CONNSTR to a disposable GP6 coordinator'
  unless defined $ENV{GP6_TEST_CONNSTR};

my $source = $ENV{GP6_TEST_CONNSTR};
my $dbname = "dump_gp6_checks_$$";
my $created_source = 0;
my $target;
my $target_running = 0;
my $work = TestLib::tempdir;

sub source_sql
{
	my ($database, $sql) = @_;
	my ($out, $err);
	local $ENV{PGOPTIONS} = '';
	IPC::Run::run ['psql', '-XAt', '-v', 'ON_ERROR_STOP=1',
		'-d', "$source dbname=$database", '-c', $sql],
		'>' => \$out, '2>' => \$err or die "GP6 SQL failed: $err";
	chomp $out;
	return $out;
}

END
{
	my $status = $?;
	if ($target_running)
	{
		system('pg_ctl', '-D', $target->data_dir, '-m', 'fast', '-w', 'stop');
	}
	if ($created_source)
	{
		eval { source_sql('postgres', "DROP DATABASE $dbname") };
		warn $@ if $@;
	}
	$? = $status;
}

like(source_sql('postgres', 'SELECT version()'), qr/Greenplum Database 6\./,
	'source is GP6');
source_sql('postgres', "CREATE DATABASE $dbname TEMPLATE template0");
$created_source = 1;

source_sql($dbname, q{
CREATE SCHEMA partition_checks;
SET search_path = partition_checks, public;
CREATE TABLE flat (k integer, dead text, v integer,
  CONSTRAINT shared_valid CHECK (v < 1000)) DISTRIBUTED BY (k)
  PARTITION BY RANGE (k) (PARTITION p START (0) END (10));
ALTER TABLE flat DROP COLUMN dead;
ALTER TABLE flat ADD CONSTRAINT shared_pending CHECK (v > 0) NOT VALID;
CREATE TABLE candidate (k integer, v integer,
  CONSTRAINT shared_valid CHECK (v < 1000)) DISTRIBUTED BY (k);
ALTER TABLE candidate ADD CONSTRAINT shared_pending CHECK (v > 0) NOT VALID;
ALTER TABLE flat EXCHANGE PARTITION p WITH TABLE candidate;
DROP TABLE candidate;

CREATE TABLE nested (k integer, dead text, r integer, v integer,
  CONSTRAINT shared_valid CHECK (v < 1000)) DISTRIBUTED BY (k)
  PARTITION BY RANGE (k) SUBPARTITION BY LIST (r)
  (PARTITION p START (0) END (10) (SUBPARTITION s VALUES (1)));
ALTER TABLE nested DROP COLUMN dead;
ALTER TABLE nested ADD CONSTRAINT shared_pending CHECK (v > 0) NOT VALID;
CREATE TABLE candidate (k integer, r integer, v integer,
  CONSTRAINT shared_valid CHECK (v < 1000)) DISTRIBUTED BY (k);
ALTER TABLE candidate ADD CONSTRAINT shared_pending CHECK (v > 0) NOT VALID;
ALTER TABLE nested ALTER PARTITION p EXCHANGE PARTITION s WITH TABLE candidate;
DROP TABLE candidate;

COMMENT ON CONSTRAINT shared_pending ON flat IS 'flat root check';
COMMENT ON CONSTRAINT shared_pending ON flat_1_prt_p IS 'flat leaf check';
COMMENT ON CONSTRAINT shared_pending ON nested IS 'nested root check';
COMMENT ON CONSTRAINT shared_pending ON nested_1_prt_p_2_prt_s IS 'nested leaf check';

-- GP6 rejects extra checks on EXCHANGE candidates and ADD CONSTRAINT on
-- individual leaves. An ordinary table is the independent-check control.
CREATE TABLE standalone (v integer) DISTRIBUTED BY (v);
ALTER TABLE standalone ADD CONSTRAINT independent_pending CHECK (v <> 7) NOT VALID;
});

# EXCHANGE leaves these constraints both local and inherited on GP6. Their
# NOT VALID state makes pg_dump emit separate post-data constraint entries.
is(source_sql($dbname, q{
SELECT c.relname, con.conislocal, con.coninhcount, con.convalidated
FROM pg_constraint con JOIN pg_class c ON c.oid = con.conrelid
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = 'partition_checks' AND con.conname = 'shared_pending'
  AND c.relname IN ('flat_1_prt_p', 'nested_1_prt_p_2_prt_s')
ORDER BY c.relname;
}), "flat_1_prt_p|t|1|f\nnested_1_prt_p_2_prt_s|t|1|f",
	'GP6 exchanged leaves have local and inherited NOT VALID checks');
is(source_sql($dbname, q{
SELECT count(*) FROM pg_inherits i
JOIN pg_class child ON child.oid = i.inhrelid
JOIN pg_class parent ON parent.oid = i.inhparent
JOIN pg_namespace n ON n.oid = child.relnamespace
WHERE n.nspname = 'partition_checks' AND child.relnatts <> parent.relnatts;
}), '2', 'both exchanged leaves require physical layout reconstruction');

my $checks = q{
SELECT c.relname, con.conname, con.convalidated, con.conislocal, con.coninhcount
FROM pg_constraint con JOIN pg_class c ON c.oid = con.conrelid
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = 'partition_checks'
  AND con.conname IN ('shared_valid', 'shared_pending', 'independent_pending')
ORDER BY c.relname, con.conname;
};
my $expected_checks = <<'CHECKS';
flat|shared_pending|f|t|0
flat|shared_valid|t|t|0
flat_1_prt_p|shared_pending|f|f|1
flat_1_prt_p|shared_valid|t|f|1
nested|shared_pending|f|t|0
nested|shared_valid|t|t|0
nested_1_prt_p|shared_pending|f|f|1
nested_1_prt_p|shared_valid|t|f|1
nested_1_prt_p_2_prt_s|shared_pending|f|f|1
nested_1_prt_p_2_prt_s|shared_valid|t|f|1
standalone|independent_pending|f|t|0
CHECKS
chomp $expected_checks;

my $comments = q{
SELECT c.relname, coalesce(obj_description(con.oid, 'pg_constraint'), '<none>')
FROM pg_constraint con JOIN pg_class c ON c.oid = con.conrelid
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = 'partition_checks' AND con.conname = 'shared_pending'
ORDER BY c.relname;
};
my $expected_comments = source_sql($dbname, $comments);

$target = get_new_node('gp6_checks_target');
$target->init;
$target_running = 1;
command_ok(['pg_ctl', '-D', $target->data_dir, '-l', $target->logfile,
	'-o', '-b -c gp_role=utility --gp_dbid=1 --gp_contentid=-1 -c maintenance_mode=on',
	'-w', 'start'], 'start binary-upgrade target');
# COPY seeds the planner's segment inventory without requiring a running segment.
{
	local $ENV{PGOPTIONS} = '-c gp_role=utility';
	$target->safe_psql('postgres', q{
SET allow_system_table_mods = true;
COPY gp_segment_configuration
  (dbid, content, role, preferred_role, mode, status, port, hostname, address, datadir, warehouseid)
FROM STDIN;
1	-1	p	p	n	u	1	localhost	127.0.0.1	/unused	0
2	0	p	p	n	u	1	localhost	127.0.0.1	/unused	0
\.
});
}

for my $format ('plain', 'parallel')
{
	my $dump = "$work/$format.dump";
	{
		local $ENV{PGOPTIONS} = '';
		command_ok(['pg_dump', '--binary-upgrade', '--schema-only',
			'-n', 'partition_checks', ($format eq 'plain' ? '-Fp' : '-Fc'),
			'-f', $dump, '-d', "$source dbname=$dbname"], "$format: dump GP6 schema");
	}
	local $ENV{PGOPTIONS} = '-c gp_role=utility';
	$target->safe_psql('postgres', "CREATE DATABASE $format TEMPLATE template0");
	if ($format eq 'plain')
	{
		command_ok(['psql', '-X', '-v', 'ON_ERROR_STOP=1',
			'-d', $target->connstr($format), '-f', $dump],
			'plain: restore without duplicate constraints');
	}
	else
	{
		command_ok(['pg_restore', '--exit-on-error', '-j4',
			'-d', $target->connstr($format), $dump],
			'parallel: restore without duplicate constraints');
	}
	is($target->safe_psql($format, $checks), $expected_checks,
		"$format: validation, inheritance and independent checks preserved");
	is($target->safe_psql($format, $comments), $expected_comments,
		"$format: distinct root and leaf constraint comments preserved");
	is($target->safe_psql($format, q{
SELECT child.relname, parent.relname FROM pg_inherits i
JOIN pg_class child ON child.oid = i.inhrelid
JOIN pg_class parent ON parent.oid = i.inhparent
JOIN pg_namespace n ON n.oid = child.relnamespace
WHERE n.nspname = 'partition_checks' ORDER BY child.relname;
}), "flat_1_prt_p|flat\nnested_1_prt_p|nested\nnested_1_prt_p_2_prt_s|nested_1_prt_p",
		"$format: direct and nested partitions remain attached");
	for my $leaf ('flat_1_prt_p', 'nested_1_prt_p_2_prt_s')
	{
		my $values = $leaf eq 'flat_1_prt_p' ? '1, -1' : '1, 1, -1';
		command_fails_like(['psql', '-X', '-v', 'ON_ERROR_STOP=1',
			'-d', $target->connstr($format), '-c',
			"INSERT INTO partition_checks.$leaf VALUES ($values)"],
			qr/violates check constraint "shared_pending"/,
			"$format: inherited NOT VALID check enforces new writes to $leaf");
	}
	command_fails_like(['psql', '-X', '-v', 'ON_ERROR_STOP=1',
		'-d', $target->connstr($format), '-c',
		'INSERT INTO partition_checks.standalone VALUES (7)'],
		qr/violates check constraint "independent_pending"/,
		"$format: independent NOT VALID check still enforces new writes");
}

command_ok(['pg_ctl', '-D', $target->data_dir, '-m', 'fast', '-w', 'stop'],
	'stop binary-upgrade target');
$target_running = 0;
source_sql('postgres', "DROP DATABASE $dbname");
$created_source = 0;
done_testing();
