CREATE EXTENSION test_anser;

-- Happy path: one condition, two producers, two consumers.
SELECT anser_test_register_condition(1, 1, 1, 'join_a', 2);
SELECT anser_test_subscribe(1, 1, 1, 'join_a');
SELECT anser_test_subscribe(1, 1, 1, 'join_a');
SELECT anser_test_publish(1, 1, 1, 'join_a', '\x6161'::bytea, false);
SELECT anser_test_state(1, 1, 1, 'join_a');
SELECT anser_test_publish(1, 1, 1, 'join_a', '\x6262'::bytea, false);
SELECT anser_test_state(1, 1, 1, 'join_a');
SELECT encode(anser_test_consume(1, 1, 1, 'join_a', 0), 'escape');
SELECT encode(anser_test_consume(1, 1, 1, 'join_a', 0), 'escape');
SELECT anser_test_state(1, 1, 1, 'join_a');

-- Timeout/cancel path: no producer publishes.
SELECT anser_test_register_condition(1, 1, 2, 'join_timeout', 1);
SELECT anser_test_subscribe(1, 1, 2, 'join_timeout');
SELECT anser_test_consume(1, 1, 2, 'join_timeout', 1) IS NULL;
SELECT anser_test_state(1, 1, 2, 'join_timeout');

-- Input validation: negative IDs/counts and overlong condition keys fail.
SELECT anser_test_register_condition(1, 1, -1, 'bad_id', 1);
SELECT anser_test_register_condition(1, 1, 3, 'bad_count', 0);
SELECT anser_test_register_condition(1, 1, 3, repeat('x', 64), 1);
SELECT anser_test_subscribe(1, 1, -1, 'bad_id');
SELECT anser_test_subscribe(1, 1, 3, repeat('x', 64));

-- Query-level cancellation touches all channels for the command.
SELECT anser_test_register_condition(1, 2, 1, 'join_b', 1);
SELECT anser_test_register_condition(1, 2, 2, 'join_c', 1);
SELECT anser_test_cancel_query(1, 2);
SELECT anser_test_state(1, 2, 1, 'join_b');
SELECT anser_test_state(1, 2, 2, 'join_c');

-- Bloom payload protocol and standalone producer/consumer helpers.
SELECT anser_test_bloom_roundtrip('bf_roundtrip', 42);
SELECT anser_test_bloom_union('bf_union', 42, 84);
SELECT anser_test_node_roundtrip(168);

DROP EXTENSION test_anser;
