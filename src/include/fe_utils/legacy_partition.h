/*-------------------------------------------------------------------------
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * legacy_partition.h
 *    Shared GP6 partition queries for pg_dump and pg_upgrade.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LEGACY_PARTITION_H
#define LEGACY_PARTITION_H

/*
 * A mismatched heap descriptor can be reconstructed only for a same-schema,
 * single-parent leaf that the target can attach.  Typed tables cannot be
 * attached, and every live NOT NULL column inherited from the root must be
 * NOT NULL in the leaf too.  Match these columns by name, since their physical
 * positions can differ.  Both dump selection and upgrade preflight must use
 * exactly this predicate.  The query must provide pg_partition AS p and
 * pg_class AS parent (the root) and child.
 */
#define LEGACY_HEAP_PARTITION_REBUILDABLE \
	"(child.relstorage = 'h' " \
	" AND child.reloftype = 0 " \
	" AND child.relnamespace = parent.relnamespace " \
	" AND (SELECT count(*) FROM pg_catalog.pg_inherits i " \
	"      WHERE i.inhrelid = child.oid) = 1 " \
	" AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_inherits i " \
	"                 WHERE i.inhparent = child.oid) " \
	" AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_attribute pa " \
	"                 WHERE pa.attrelid = parent.oid AND pa.attnum > 0 " \
	"                   AND NOT pa.attisdropped AND pa.attnotnull " \
	"                   AND NOT EXISTS ( " \
	"                     SELECT 1 FROM pg_catalog.pg_attribute ca " \
	"                     WHERE ca.attrelid = child.oid " \
	"                       AND ca.attname = pa.attname AND ca.attnum > 0 " \
	"                       AND NOT ca.attisdropped AND ca.attnotnull)) " \
	" AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_partition deeper " \
	"                 WHERE deeper.parrelid = p.parrelid " \
	"                   AND deeper.parlevel = p.parlevel + 1 " \
	"                   AND NOT deeper.paristemplate)) "

#endif                          /* LEGACY_PARTITION_H */
