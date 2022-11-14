/* contrib/uuid-cb/uuid-cb--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use '''CREATE EXTENSION "uuid-cb"''' to load this file. \quit

CREATE FUNCTION uuid_cb_generate()
RETURNS char(38)
AS 'MODULE_PATHNAME', 'uuid_cb_generate'
VOLATILE STRICT LANGUAGE C;

CREATE FUNCTION uuid_cb_valid(char(38))
RETURNS boolean
AS 'MODULE_PATHNAME', 'uuid_cb_valid'
IMMUTABLE STRICT LANGUAGE C;
