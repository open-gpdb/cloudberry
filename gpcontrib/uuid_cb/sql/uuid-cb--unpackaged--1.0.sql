/* contrib/uuid-cb/uuid-cb--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use '''CREATE EXTENSION "uuid-cb" FROM unpackaged''' to load this file. \quit

ALTER EXTENSION "uuid-cb" ADD function uuid_cb_generate();
ALTER EXTENSION "uuid-cb" ADD function uuid_cb_valid(char(38));
