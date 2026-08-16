CREATE EXTENSION "uuid-cb";

-- works with NULL
SELECT uuid_cb_valid(NULL);

-- valid UUIDs
SELECT uuid_cb_valid('10000000-0000-0000-0000-000000000000-1');
SELECT uuid_cb_valid('01000000-0000-0000-0000-000000000000-2');

-- invalid UUIDs
SELECT uuid_cb_valid('10000000-0000-0000-0000-000000000000-2');
SELECT uuid_cb_valid('10000000-0000-0000-0000-000000000000-1 ');
SELECT uuid_cb_valid('10000000-0000-0000-0000-000000000000');
SELECT uuid_cb_valid('foobar');

-- check uniqueness of generated UUIDs
SELECT COUNT(DISTINCT uid) = 100000 AS no_duplicates FROM
	(SELECT uuid_cb_generate() FROM generate_series(1, 100000)) as uid;

-- check correctness of generated UUIDs
SELECT COUNT(1) = 0 AS all_correct FROM
	(SELECT uuid_cb_valid(uuid_cb_generate()) as is_correct FROM generate_series(1, 100000)) as subq
	where subq.is_correct = False;
