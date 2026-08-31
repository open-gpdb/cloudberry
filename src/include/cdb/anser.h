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
 * Poll interval (milliseconds) a backend sleeps on its latch between rechecks
 * when the awaited change does NOT set its latch, so it must recheck shared
 * state itself (waiting for a channel state in AnserWaitForState, or for a free
 * submission slot in AnserEnqueueSubmission).  Kept small so waits stay
 * responsive without busy-looping.
 */
#define ANSER_WAIT_POLL_INTERVAL_MS	10L

/*
 * Safety wakeup (milliseconds) for latch-driven waits where the event always
 * sets the waiter's latch (a producer's submission ACK in AnserWaitSubmissionAck,
 * a consumer's delivery in AnserWaitSlotResult).  The wait normally ends on the
 * latch; this timeout only bounds how long a lost wakeup could stall it.
 */
#define ANSER_WAIT_LATCH_TIMEOUT_MS	1000L

/*
 * Wakeup interval (milliseconds) for a background service's main loop.  Each
 * service runs its data-path pass whenever its latch fires; this timed wakeup
 * additionally bounds how long a stale COLLECTING channel or a dead-backend slot
 * can linger between latches before periodic maintenance reclaims it.
 */
#define ANSER_SERVICE_WAKEUP_INTERVAL_MS	1000L

/*
 * Registered adaptive-information condition for one running command.
 *
 * condition_key is an opaque symbol that identifies the condition (the
 * optimizer-generated equivalence-class symbols described in the Anser
 * paper); the channel map only compares keys for equality.
 */
typedef struct AnserChannelKey
{
	int			gp_session_id;
	int			gp_command_count;
	uint32		condition_id;
	char		condition_key[ANSER_CONDITION_KEY_SIZE];
} AnserChannelKey;

/*
 * Channel lifecycle: PENDING (created, awaiting producers) -> COLLECTING
 * (first part received) -> READY (all expected parts unioned) ->
 * CONSUMED (all expected consumers delivered).  CANCELLED replaces any
 * state on produce timeout, producer cancel, or owning query end.
 */
typedef enum AnserChannelState
{
	ANSER_CHANNEL_PENDING = 0,
	ANSER_CHANNEL_COLLECTING,
	ANSER_CHANNEL_READY,
	ANSER_CHANNEL_CANCELLED,
	ANSER_CHANNEL_CONSUMED
} AnserChannelState;

/*
 * One channel in the shared-memory map: the condition key, lifecycle state,
 * creator ownership, producer/consumer accounting, and the DSM handle of
 * the gathered payload.
 */
typedef struct AnserChannelEntry
{
	AnserChannelKey key;
	AnserChannelState state;
	Oid			creator_role;	/* authenticated role that created the channel;
								 * only this role (or a superuser) may
								 * produce/consume on it -- see anserfuncs.c */
	int32		expected_producers;
	int32		done_producers;
	int32		consumers;
	int32		expected_consumers;	/* consumers to deliver before recycling the
									 * payload (one per segment); 0 = unknown */
	int32		done_consumers;
	Size		data_len;
	dsm_handle	dsm_handle;
	TimestampTz updated_at;		/* last activity; drives the produce timeout */
} AnserChannelEntry;

/*
 * Shared control block: effective sizing limits, the background services'
 * latches, and the maintenance-sweep switch.
 */
typedef struct AnserControl
{
	uint32		max_channels;
	Size		max_info_size;
	Latch		gather_latch;
	Latch		send_latch;
	bool		sweep_enabled;	/* when false, the periodic maintenance sweep
								 * leaves terminal channels in place; a test-only
								 * knob so terminal state can be observed
								 * deterministically.  Emergency (map-full)
								 * reclamation is unaffected. */
} AnserControl;

/* GUCs */
extern bool gp_anser_enable;
extern bool gp_anser_runtime_filter;
extern bool gp_anser_conn;		/* startup-option marker for token-auth conns */
extern int	gp_anser_max_channels;
extern int	gp_anser_max_info_size;
extern int	gp_anser_timeout_ms;
extern int	gp_anser_max_consumers_per_channel;

/* Shared-memory setup. */
extern Size AnserShmemSize(void);
extern void AnserShmemInit(void);

/*
 * Effective channel-map size (gp_anser_max_channels, or its auto-sizing from
 * max_connections * gp_max_slices).  Computed once and cached for the life of
 * the process; see the definition in anser.c.
 */
extern int	AnserMaxChannels(void);

/* Public channel-manager API. */
extern bool AnserSubscribe(const AnserChannelKey *channel_key);
extern bool AnserPublish(const AnserChannelKey *channel_key,
						 const void *payload, Size payload_len,
						 bool cancelled);
extern bool AnserWaitProducersRegistered(const AnserChannelKey *channel_key,
										 long timeout_ms);
extern bool AnserWaitReady(const AnserChannelKey *channel_key,
					   bool *cancelled);
extern bool AnserConsumeReady(const AnserChannelKey *channel_key,
						  void *buffer, Size buffer_size, Size *payload_len,
						  bool *cancelled);
extern AnserChannelState AnserChannelGetState(const AnserChannelKey *channel_key,
										  bool *found);
extern int	AnserChannelConsumerCount(const AnserChannelKey *channel_key);
extern int	AnserChannelPayloadBytes(const AnserChannelKey *channel_key);
extern void AnserCancelQuery(int gp_session_id, int gp_command_count);
extern void AnserAttachServiceLatch(bool gather_service);
extern void AnserDetachServiceLatch(bool gather_service);
extern void AnserWaitServiceLatch(bool gather_service, long timeout_ms);
extern void AnserWakeServiceLatch(bool gather_service);
extern void AnserServiceMaintenance(void);
extern void AnserSetSweepEnabled(bool enabled);

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
							   int expected_producers,
							   Oid caller_role, bool caller_is_super);
extern bool AnserProducerSubmit(const AnserChannelKey *channel_key,
								int expected_producers,
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

/*
 * Session-token authentication for the segment -> coordinator libpq
 * transport (parallel-retrieve-cursor model; see anser.c).  The QD calls
 * AnserGetOrCreateSessionToken at plan time; libpq/auth.c calls
 * AnserSessionTokenIsValid from the gp_anser_conn authentication branch.
 */
extern char *AnserGetOrCreateSessionToken(Oid user_id);
extern bool AnserSessionTokenIsValid(Oid user_id, const char *token_hex);

#endif							/* CDB_ANSER_H */
