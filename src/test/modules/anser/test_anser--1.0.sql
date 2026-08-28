/* src/test/modules/anser/test_anser--1.0.sql */

CREATE FUNCTION anser_test_register_condition(
    gp_session_id int4,
    gp_command_count int4,
    condition_id int4,
    condition_key text,
    expected_producers int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_subscribe(
    gp_session_id int4,
    gp_command_count int4,
    condition_id int4,
    condition_key text)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_publish(
    gp_session_id int4,
    gp_command_count int4,
    condition_id int4,
    condition_key text,
    payload bytea,
    cancelled bool)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_publish_value(
    gp_session_id int4,
    gp_command_count int4,
    condition_id int4,
    condition_key text,
    value int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_consume(
    gp_session_id int4,
    gp_command_count int4,
    condition_id int4,
    condition_key text,
    timeout_ms int4)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_consume_has(
    gp_session_id int4,
    gp_command_count int4,
    condition_id int4,
    condition_key text,
    value int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_state(
    gp_session_id int4,
    gp_command_count int4,
    condition_id int4,
    condition_key text)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_cancel_query(
    gp_session_id int4,
    gp_command_count int4)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_bloom_roundtrip(
    condition_key text,
    value int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_bloom_fold_inplace()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION anser_test_bloom_rejects_mismatch()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION anser_test_node_roundtrip(value int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_client_roundtrip(value int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_token_roundtrip()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION anser_test_multi_consumer(value int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_abandoned_consumer_recycles(value int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_dsm_free_on_success()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION anser_test_dsm_free_on_timeout()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION anser_test_dsm_free_on_cancel()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION anser_test_set_sweep(enabled bool)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_sweep()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION anser_test_max_channels_stable_across_slices()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
