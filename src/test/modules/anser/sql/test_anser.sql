CREATE EXTENSION test_anser;

-- Pause the background maintenance sweep so terminal (CANCELLED/CONSUMED)
-- channels stay observable and the state assertions below are deterministic
-- rather than racing the live gather/send services.  Re-enabled at the end,
-- where we prove the sweep actually reclaims them.
SELECT anser_test_set_sweep(false);

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

-- GUC sizing guard: AnserMaxChannels() is memoized at postmaster start, so a
-- per-session SET gp_max_slices must not change the reported channel-map size
-- (otherwise the shared arrays and the Len functions would disagree).
SELECT anser_test_max_channels_stable_across_slices() AS max_channels_stable;

-- Bloom payload protocol and standalone producer/consumer helpers.
SELECT anser_test_bloom_roundtrip('bf_roundtrip', 42);
SELECT anser_test_bloom_union('bf_union', 42, 84);
-- Coordinator-side fold: parts combined into one merged chunk (union on master).
SELECT anser_test_bloom_fold('bf_fold', 42, 84);
-- Payload-combine policy: opaque payloads append, bloom parts union (one chunk).
SELECT anser_test_payload_combine() AS combine_ok;
SELECT anser_test_node_roundtrip(168);

-- Security regression: a crafted part header with a tiny bitset (bitset_bytes
-- == 0) must be rejected, not turned into a filter with no bitset storage.
SELECT anser_test_bloom_rejects_tiny(0) AS reject_0;
SELECT anser_test_bloom_rejects_tiny(2) AS reject_2;
SELECT anser_test_bloom_rejects_tiny(4) AS reject_4;

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

DROP EXTENSION test_anser;
