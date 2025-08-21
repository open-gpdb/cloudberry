
# Copyright (c) 2021-2023, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_isready');
program_version_ok('pg_isready');
program_options_handling_ok('pg_isready');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

$node->command_fails(['pg_isready'], 'fails with no server running');

$node->start;

$node->command_ok(
<<<<<<< HEAD
	[ 'pg_isready', "--timeout=$TestLib::timeout_default" ],
=======
	[ 'pg_isready', "--timeout=$PostgreSQL::Test::Utils::timeout_default" ],
>>>>>>> REL_16_9
	'succeeds with server running');

done_testing();
