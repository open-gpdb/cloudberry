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
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * anserclient.h
 *	  libpq client helpers that let a remote (segment) backend reach the
 *	  coordinator-resident Anser services over an ordinary connection to the QD.
 *
 * IDENTIFICATION
 *	  src/include/cdb/anserclient.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDB_ANSERCLIENT_H
#define CDB_ANSERCLIENT_H

#include "cdb/anser.h"

/*
 * Publish one producer part to the coordinator.  Opens a short-lived libpq
 * connection to the QD, runs gp_anser_producer_begin + gp_anser_publish, and
 * closes.  Fail-open: any connection/protocol error best-effort publishes a
 * cancel for the dataset and returns false, never raising.
 *
 * `token` is the QD session token used to authenticate the connection (the
 * parallel-retrieve-cursor model: gp_anser_conn=true + token as password,
 * bypassing pg_hba); NULL or "" connects without it and relies on pg_hba.
 */
extern bool AnserClientPublish(const AnserChannelKey *channel_key,
							   uint32 expected_producers,
							   const void *payload, Size payload_len,
							   bool cancelled, const char *token);

/*
 * Wait for delivery of a channel payload from the coordinator.  Opens a
 * query-lifetime libpq connection to the QD, runs gp_anser_consume_wait, and
 * blocks (interruptibly) until the row arrives.  On success *payload points at a
 * palloc'd copy of the bytes.  Connection loss is treated as a cancel for this
 * consumer only.  `token` is as in AnserClientPublish.
 */
extern bool AnserClientConsumeWait(const AnserChannelKey *channel_key,
								   void **payload, Size *payload_len,
								   bool *cancelled, const char *token);

#endif							/* CDB_ANSERCLIENT_H */
