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

-- Query-level cancellation touches all channels for the command.
SELECT anser_test_register_condition(1, 2, 1, 'join_b', 1);
SELECT anser_test_register_condition(1, 2, 2, 'join_c', 1);
SELECT anser_test_cancel_query(1, 2);
SELECT anser_test_state(1, 2, 1, 'join_b');
SELECT anser_test_state(1, 2, 2, 'join_c');

DROP EXTENSION test_anser;
