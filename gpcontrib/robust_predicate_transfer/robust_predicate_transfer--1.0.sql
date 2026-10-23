/* gpcontrib/robust_predicate_transfer/robust_predicate_transfer--1.0.sql */

-- Plan a query and dump its RPT+ transfer plan (regions, join graph,
-- equivalence classes, forward chain, backward broadcast, consumer
-- eligibility).  Analysis only: nothing is injected or executed.
CREATE FUNCTION rpt_transfer_plan(query text)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
