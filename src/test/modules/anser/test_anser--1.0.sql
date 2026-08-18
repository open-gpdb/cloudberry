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

CREATE FUNCTION anser_test_consume(
    gp_session_id int4,
    gp_command_count int4,
    condition_id int4,
    condition_key text,
    timeout_ms int4)
RETURNS bytea
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

CREATE FUNCTION anser_test_cancel_channel(
    gp_session_id int4,
    gp_command_count int4,
    condition_id int4,
    condition_key text)
RETURNS bool
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

CREATE FUNCTION anser_test_bloom_union(
    condition_key text,
    left_value int4,
    right_value int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION anser_test_node_roundtrip(value int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
