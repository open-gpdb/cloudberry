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
 * anser.h
 *	  Shared-memory channel map for the Anser adaptive information
 *	  sharing subsystem.
 *
 * IDENTIFICATION
 *	  src/include/cdb/anser.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDB_ANSER_H
#define CDB_ANSER_H

#include "postgres.h"

#include "datatype/timestamp.h"
#include "storage/dsm.h"
#include "storage/latch.h"

#define ANSER_CONDITION_KEY_SIZE	64

/*
 * Registered adaptive-information condition for one running command.
 *
 * The planner integration will eventually populate condition_key with the
 * optimizer-generated equivalence-class symbols described in the Anser paper.
 * For PR 1 this key is intentionally opaque and provided by callers/tests.
 */
typedef struct AnserChannelKey
{
	int			gp_session_id;
	int			gp_command_count;
	uint32		condition_id;
	char		condition_key[ANSER_CONDITION_KEY_SIZE];
} AnserChannelKey;

typedef enum AnserChannelState
{
	ANSER_CHANNEL_PENDING = 0,
	ANSER_CHANNEL_COLLECTING,
	ANSER_CHANNEL_READY,
	ANSER_CHANNEL_CANCELLED,
	ANSER_CHANNEL_CONSUMED
} AnserChannelState;

typedef struct AnserChannelEntry
{
	AnserChannelKey key;
	AnserChannelState state;
	bool		cancelled;
	Oid			creator_role;	/* authenticated role that created the channel;
								 * only this role (or a superuser) may
								 * produce/consume on it -- see anserfuncs.c */
	uint32		expected_producers;
	uint32		done_producers;
	uint32		consumers;
	uint32		done_consumers;
	Size		data_len;
	dsm_handle	dsm_handle;
	TimestampTz created_at;
	TimestampTz updated_at;
} AnserChannelEntry;

typedef struct AnserControl
{
	uint32		max_channels;
	Size		max_info_size;
	Latch		gather_latch;
	Latch		send_latch;
} AnserControl;

/* GUCs */
extern bool gp_anser_enable;
extern bool gp_anser_runtime_filter;
extern int	gp_anser_max_channels;
extern int	gp_anser_max_info_size;
extern int	gp_anser_timeout_ms;
extern int	gp_anser_max_consumers_per_channel;

/* Shared-memory setup. */
extern Size AnserShmemSize(void);
extern void AnserShmemInit(void);

/* Public channel-manager API. */
extern bool AnserRegisterCondition(int gp_session_id, int gp_command_count,
								   uint32 condition_id, const char *condition_key,
								   uint32 expected_producers,
								   AnserChannelKey *channel_key);
extern bool AnserSubscribe(const AnserChannelKey *channel_key);
extern bool AnserPublish(const AnserChannelKey *channel_key,
						 const void *payload, Size payload_len,
						 bool cancelled);
extern bool AnserWaitProducersRegistered(const AnserChannelKey *channel_key,
										 long timeout_ms);
extern bool AnserConsume(const AnserChannelKey *channel_key,
						 void *buffer, Size buffer_size, Size *payload_len,
						 bool *cancelled, long timeout_ms);
extern bool AnserWaitReady(const AnserChannelKey *channel_key,
					   bool *cancelled);
extern bool AnserConsumeReady(const AnserChannelKey *channel_key,
						  void *buffer, Size buffer_size, Size *payload_len,
						  bool *cancelled);
extern AnserChannelState AnserChannelGetState(const AnserChannelKey *channel_key,
										  bool *found);
extern int	AnserChannelConsumerCount(const AnserChannelKey *channel_key);
extern bool AnserCancelChannel(const AnserChannelKey *channel_key);
extern void AnserCancelQuery(int gp_session_id, int gp_command_count);
extern void AnserAttachServiceLatch(bool gather_service);
extern void AnserDetachServiceLatch(bool gather_service);
extern void AnserWaitServiceLatch(bool gather_service, long timeout_ms);
extern void AnserWakeServiceLatch(bool gather_service);
extern void AnserServiceMaintenance(void);

/*
 * Network-path API.
 *
 * These entry points back the gp_anser_* built-in functions that remote
 * (segment) producers and consumers call over libpq.  Unlike the direct
 * AnserPublish/AnserConsume* API above, they do not touch the channel payload
 * from the calling backend: producers hand their part to the gather service
 * through the inbound submission queue and block for an ACK; consumers register
 * a wait slot and block until the send service delivers or cancels it.
 */
extern bool AnserProducerBegin(const AnserChannelKey *channel_key,
							   uint32 expected_producers,
							   Oid caller_role, bool caller_is_super);
extern bool AnserProducerSubmit(const AnserChannelKey *channel_key,
								uint32 expected_producers,
								const void *payload, Size payload_len,
								bool cancelled,
								Oid caller_role, bool caller_is_super);
extern bool AnserConsumerWait(const AnserChannelKey *channel_key,
							  void **payload, Size *payload_len,
							  bool *cancelled,
							  Oid caller_role, bool caller_is_super);

/*
 * Data-path cycles executed by the background services.  Each performs one
 * non-blocking pass over the shared state; the service loops call them
 * whenever their latch fires or the maintenance timer elapses.
 */
extern void AnserGatherServiceCycle(void);
extern void AnserSendServiceCycle(void);

/* Background-service entry points. */
extern void AnserGatherServiceMain(Datum main_arg);
extern void AnserSendServiceMain(Datum main_arg);
extern bool AnserStartRule(Datum main_arg);

#endif							/* CDB_ANSER_H */
