CREATE EXTENSION test_anser;

-- Pause the background maintenance sweep so terminal (CANCELLED/CONSUMED)
-- channels stay observable and the state assertions below are deterministic
-- rather than racing the live gather/send services.  Re-enabled at the end,
-- where we prove the sweep actually reclaims them.
SELECT anser_test_set_sweep(false);

-- Happy path: one condition, two producers, two consumers.  Each producer
-- publishes a real bloom part (multi-payload combine is bloom-only now); the two
-- parts union on the coordinator, so each consumer's received filter contains
-- both producers' values.
SELECT anser_test_register_condition(1, 1, 1, 'join_a', 2);
SELECT anser_test_subscribe(1, 1, 1, 'join_a');
SELECT anser_test_subscribe(1, 1, 1, 'join_a');
SELECT anser_test_publish_value(1, 1, 1, 'join_a', 10);
SELECT anser_test_state(1, 1, 1, 'join_a');
SELECT anser_test_publish_value(1, 1, 1, 'join_a', 20);
SELECT anser_test_state(1, 1, 1, 'join_a');
SELECT anser_test_consume_has(1, 1, 1, 'join_a', 10);
SELECT anser_test_consume_has(1, 1, 1, 'join_a', 20);
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

-- GUC sizing guard: AnserMaxChannels() is memoized at postmaster start, so a
-- per-session SET gp_max_slices must not change the reported channel-map size
-- (otherwise the shared arrays and the Len functions would disagree).
SELECT anser_test_max_channels_stable_across_slices() AS max_channels_stable;

-- Bloom payload protocol and standalone producer/consumer helpers.
SELECT anser_test_bloom_roundtrip('bf_roundtrip', 42);
-- In-place fold: same-size union mutates the buffer; a mismatched size is
-- rejected.  This is the coordinator's only combine path (first part is stored
-- verbatim, every later part folds in here).
SELECT anser_test_bloom_fold_inplace() AS fold_inplace_ok;
SELECT anser_test_node_roundtrip(168);

-- Safety regression: the consumer rebuilds the filter from its own parameters and
-- requires the received bitset to be exactly the expected size (and the header
-- magic to match); truncated/oversized/corrupt parts are rejected (fail open).
SELECT anser_test_bloom_rejects_mismatch() AS reject_mismatch;

-- Built-in round trip through the live services: producer_begin -> publish
-- (gather service appends, channel goes READY) -> consume_wait (send service
-- delivers) returns the payload.  Proves the full producer -> gather -> send ->
-- consumer chain, not just the map.
SELECT gp_anser_producer_begin(10, 1, 1, 'svc_roundtrip', 1) AS begin_ok;
SELECT gp_anser_publish(10, 1, 1, 'svc_roundtrip', '\x6162'::bytea, false) AS publish_ok;
SELECT encode(gp_anser_consume_wait(10, 1, 1, 'svc_roundtrip'), 'escape') AS payload;

-- Producer-begin timeout: begin arms the produce deadline (COLLECTING) but no
-- producer publishes; the gather maintenance pass cancels the whole dataset
-- after gp_anser_timeout_ms, so the waiting consumer is delivered a cancel and
-- consume_wait returns NULL.
SELECT gp_anser_producer_begin(11, 1, 1, 'svc_timeout', 1) AS begin_ok;
SELECT anser_test_state(11, 1, 1, 'svc_timeout') AS state_after_begin;
SELECT gp_anser_consume_wait(11, 1, 1, 'svc_timeout') IS NULL AS consume_cancelled;

-- Client helper loopback: drive AnserClientPublish / AnserClientConsumeWait
-- against the local coordinator over libpq, proving the client transport
-- end-to-end without a multi-node cluster.
SELECT anser_test_client_roundtrip(4242) AS client_ok;

-- Session token: registration, validation, and rejection of bogus token/user
-- (the token authenticates the segment -> QD backward connection).
SELECT anser_test_token_roundtrip() AS token_ok;

-- Multi-consumer partial delivery: two consumers block concurrently on one
-- channel (loopback libpq); one is cancelled mid-wait while the other still
-- receives the intact payload.  Proves delivery is per-consumer.
SELECT anser_test_multi_consumer(24680) AS partial_delivery_ok;

-- Regression guard (abandoned-consumer recycle): a consumer cancelled mid-wait
-- must stop counting toward the channel's expected consumers, so the channel
-- still recycles to CONSUMED after the surviving consumer is delivered instead
-- of lingering forever in READY with stale data.
SELECT anser_test_abandoned_consumer_recycles(13579) AS recycled_no_stale_data;

-- Clearing works: with the sweep paused, the cancelled channel above is still
-- present as CANCELLED.  Re-enable the sweep and run one synchronously; the
-- terminal channel is then reclaimed (NOT_FOUND), proving maintenance clears it.
SELECT anser_test_state(1, 1, 2, 'join_timeout') AS before_clear;
SELECT anser_test_set_sweep(true);
SELECT anser_test_sweep();
SELECT anser_test_state(1, 1, 2, 'join_timeout') AS after_clear;

-- Payload-DSM lifetime (run last: these toggle the sweep and reclaim terminal
-- channels).  Each proves the shared channel payload DSM is freed at the right
-- moment: (1) success -> freed by the sweep after the last consume; (2) only
-- 3/5 producers -> freed when the produce timeout cancels the channel; (3)
-- cancelled with consumers attached -> freed by the sweep only after every
-- consumer slot has drained (not eagerly at cancel).
SELECT anser_test_dsm_free_on_success() AS dsm_free_success;
SELECT anser_test_dsm_free_on_timeout() AS dsm_free_timeout;
SELECT anser_test_dsm_free_on_cancel() AS dsm_free_cancel;

DROP EXTENSION test_anser;
