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
 * anser.c
 *	  Shared-memory channel map for the Anser adaptive information
 *	  sharing subsystem.
 *
 * IDENTIFICATION
 *	  src/backend/cdb/anser/anser.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/anser.h"
#include "cdb/anserfilter.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/dsm_impl.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#define ANSER_CONTROL_NAME		"Anser Control"
#define ANSER_CHANNEL_HASH_NAME	"Anser Channel Hash"
#define ANSER_SUBMISSION_QUEUE_NAME	"Anser Submission Queue"
#define ANSER_WAIT_TABLE_NAME		"Anser Consumer Wait Table"
#define ANSER_TOKEN_HASH_NAME		"Anser Session Token Hash"

/*
 * Session token hash.
 *
 * Remote (segment) producers/consumers authenticate their libpq connection to
 * the QD with a per-session random token instead of relying on pg_hba entries
 * covering the segment hosts -- the parallel-retrieve-cursor model (see
 * retrieve_conn_authentication in libpq/auth.c).  The QD registers one token
 * per (gp_session_id, session user) when a plan gets its first injected
 * runtime filter and embeds the token in the dispatched plan; the segment
 * connects with gp_anser_conn=true and presents the token as the password,
 * and AnserSessionTokenIsValid() verifies it here.  Entries are removed when
 * the owning QD session exits.
 */
#define ANSER_TOKEN_BYTES		16	/* 128 bits, as ENDPOINT_TOKEN_ARR_LEN */
#define ANSER_TOKEN_HEX_LEN		(ANSER_TOKEN_BYTES * 2)

/* Token hash key: one token per (gp_session_id, session user). */
typedef struct AnserTokenTag
{
	int			session_id;
	Oid			user_id;
} AnserTokenTag;

/* Token hash entry: the hex-encoded random token registered by a session. */
typedef struct AnserTokenEntry
{
	AnserTokenTag tag;
	char		token_hex[ANSER_TOKEN_HEX_LEN + 1];
} AnserTokenEntry;

bool		gp_anser_enable = false;
bool		gp_anser_runtime_filter = false;
bool		gp_anser_conn = false;	/* marker GUC, set only via startup options */
int			gp_anser_max_channels = 0;	/* 0 = auto (see AnserMaxChannels) */
int			gp_anser_max_info_size = 64 * 1024 * 1024 + 1024 * 1024;
int			gp_anser_timeout_ms = 1000;
int			gp_anser_max_consumers_per_channel = 64;

/*
 * Fallback per-connection channel budget used to auto-size the channel map when
 * gp_max_slices is left unbounded (0).  Each concurrent query can open at most
 * one channel per runtime-filter slice, so the map is sized for
 * max_connections * max_slices; when max_slices is unbounded we assume this many
 * filter-carrying slices per query.  Only used to derive the default; an
 * explicit gp_anser_max_channels overrides it entirely.
 */
#define ANSER_AUTO_SLICES_PER_CONN	8

/*
 * Inbound submission queue.
 *
 * A remote producer backend (gp_anser_publish) hands one part to the gather
 * service through a free slot here, then blocks on its own proc latch until the
 * gather service flips the slot to a terminal state and wakes it.  The producer
 * keeps its payload DSM segment attached for the whole wait, so the gather
 * service can attach the same handle without a pin/unpin dance.
 */
typedef enum AnserSubmissionState
{
	ANSER_SUBMIT_FREE = 0,		/* slot available */
	ANSER_SUBMIT_PENDING,		/* filled by producer, awaiting gather */
	ANSER_SUBMIT_ACCEPTED,		/* gather appended the part */
	ANSER_SUBMIT_REJECTED		/* gather refused (cancel/overflow/lost DSM) */
} AnserSubmissionState;

typedef struct AnserSubmissionEntry
{
	AnserSubmissionState state;
	AnserChannelKey key;
	int32		expected_producers;
	dsm_handle	dsm_handle;		/* producer's part, DSM_HANDLE_INVALID if none */
	Size		len;
	bool		cancelled;
	int			producer_pid;
	Latch	   *producer_latch;
} AnserSubmissionEntry;

/*
 * Consumer wait table.
 *
 * A remote consumer backend (gp_anser_consume_wait) registers a slot and blocks
 * on its proc latch.  The send service, when a channel becomes READY, copies the
 * payload into a fresh pinned DSM segment per waiting consumer, stamps the
 * handle here, and wakes the consumer, which attaches, copies the bytes out, and
 * frees the segment.  On CANCELLED it just flips the slot and wakes.
 */
typedef enum AnserWaitSlotState
{
	ANSER_WAIT_FREE = 0,		/* slot available */
	ANSER_WAIT_WAITING,			/* consumer registered, blocked */
	ANSER_WAIT_DELIVERED,		/* send service stamped a payload segment */
	ANSER_WAIT_CANCELLED		/* send service cancelled this consumer */
} AnserWaitSlotState;

typedef struct AnserWaitSlot
{
	AnserWaitSlotState state;
	AnserChannelKey key;
	dsm_handle	dsm_handle;		/* per-consumer payload copy, pinned by sender */
	Size		len;
	int			consumer_pid;
	Latch	   *consumer_latch;
} AnserWaitSlot;

static AnserControl *AnserCtl = NULL;
static HTAB *AnserChannelHash = NULL;
static AnserSubmissionEntry *AnserSubmissionQueue = NULL;
static AnserWaitSlot *AnserWaitTable = NULL;
static HTAB *AnserTokenHash = NULL;

/* Set once this backend has registered its session-token cleanup hook. */
static bool anser_token_exit_registered = false;

/*
 * Data-path operations -- the internal machinery the public API and the gather/
 * send service cycles drive: producer submissions, gather apply, consumer wait
 * slots, payload storage/delivery, and the maintenance sweeps.
 */
static int	AnserEnqueueSubmission(const AnserChannelKey *channel_key,
								   int expected_producers, dsm_handle handle,
								   Size len, bool cancelled);
static bool AnserWaitSubmissionAck(int slot);
static void AnserAbandonSubmission(int slot);
static bool AnserGatherApply(const AnserChannelKey *channel_key,
							 int expected_producers, dsm_handle handle,
							 Size len, bool cancelled);
static int	AnserRegisterWaitSlot(const AnserChannelKey *channel_key);
static bool AnserWaitSlotResult(int slot, void **payload, Size *payload_len,
								bool *cancelled);
static void AnserAbandonWaitSlot(int slot);
static bool AnserWaitForState(const AnserChannelKey *channel_key,
							  long timeout_ms, bool registration_only,
							  bool *cancelled);
static bool AnserStorePayloadDSM(AnserChannelEntry *entry,
							   const void *payload, Size payload_len);
static void AnserReleasePayloadDSM(AnserChannelEntry *entry);
static bool AnserDeliverChannelData(const AnserChannelEntry *entry,
									void *buffer, Size buffer_size,
									Size *payload_len);
static void AnserCancelStaleChannels(void);
static void AnserSweepOrphanChannels(void);
static void AnserReapSubmissionSlots(void);
static void AnserReapWaitSlots(void);

/*
 * Internal helpers -- shared-memory sizing, small predicates, and key building
 * used by the operations above.
 */
static bool AnserInitialized(void);
static Size AnserChannelHashSize(void);
static int	AnserSubmissionQueueLen(void);
static int	AnserWaitTableLen(void);
static Size AnserSubmissionQueueSize(void);
static Size AnserWaitTableSize(void);
static bool AnserPidIsLive(int pid);
static bool AnserChannelHasWaiters(const AnserChannelKey *channel_key);
static bool AnserChannelOwnerIsAlive(const AnserChannelEntry *entry);
static bool AnserChannelAccessAllowed(const AnserChannelKey *channel_key,
									  Oid caller_role, bool caller_is_super,
									  bool *found);

/*
 * Shared-memory setup -- one-time structure initialization at postmaster start.
 */
static void AnserInitializeControl(bool found);
static void AnserInitializeChannelHash(void);
static void AnserInitializeSubmissionQueue(bool found);
static void AnserInitializeWaitTable(bool found);
static void AnserInitializeTokenHash(void);
static void AnserTokenSessionCleanup(int code, Datum arg);

Size
AnserShmemSize(void)
{
	Size		size = 0;

	if (!gp_anser_enable)
		return 0;

	size = add_size(size, MAXALIGN(sizeof(AnserControl)));
	size = add_size(size, AnserChannelHashSize());
	size = add_size(size, AnserSubmissionQueueSize());
	size = add_size(size, AnserWaitTableSize());
	size = add_size(size, hash_estimate_size(MaxConnections,
											 sizeof(AnserTokenEntry)));

	return size;
}

void
AnserShmemInit(void)
{
	bool		found;

	if (!gp_anser_enable)
		return;

	AnserCtl = (AnserControl *) ShmemInitStruct(ANSER_CONTROL_NAME,
												  sizeof(AnserControl),
												  &found);
	AnserInitializeControl(found);
	AnserInitializeChannelHash();

	AnserSubmissionQueue = (AnserSubmissionEntry *)
		ShmemInitStruct(ANSER_SUBMISSION_QUEUE_NAME,
						AnserSubmissionQueueSize(), &found);
	AnserInitializeSubmissionQueue(found);

	AnserWaitTable = (AnserWaitSlot *)
		ShmemInitStruct(ANSER_WAIT_TABLE_NAME,
						AnserWaitTableSize(), &found);
	AnserInitializeWaitTable(found);

	AnserInitializeTokenHash();
}

bool
AnserSubscribe(const AnserChannelKey *channel_key)
{
	AnserChannelEntry *entry;
	bool		found;

	if (!AnserInitialized() || channel_key == NULL)
		return false;

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	entry = (AnserChannelEntry *) hash_search(AnserChannelHash,
											 channel_key,
											 HASH_FIND,
											 &found);
	if (!found)
	{
		LWLockRelease(AnserChannelLock);
		return false;
	}

	entry->consumers++;
	entry->updated_at = GetCurrentTimestamp();
	LWLockRelease(AnserChannelLock);

	return true;
}

bool
AnserPublish(const AnserChannelKey *channel_key, const void *payload,
			 Size payload_len, bool cancelled)
{
	AnserChannelEntry *entry;
	bool		found;

	if (!AnserInitialized() || channel_key == NULL)
		return false;

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	entry = (AnserChannelEntry *) hash_search(AnserChannelHash,
											 channel_key,
											 HASH_FIND,
											 &found);
	if (!found)
	{
		LWLockRelease(AnserChannelLock);
		return false;
	}

	if (cancelled)
	{
		entry->state = ANSER_CHANNEL_CANCELLED;
		entry->updated_at = GetCurrentTimestamp();
		SetLatch(&AnserCtl->send_latch);
		LWLockRelease(AnserChannelLock);
		return true;
	}

	if (payload_len > (Size) gp_anser_max_info_size)
	{
		entry->state = ANSER_CHANNEL_CANCELLED;
		entry->updated_at = GetCurrentTimestamp();
		SetLatch(&AnserCtl->send_latch);
		LWLockRelease(AnserChannelLock);
		return false;
	}

	if (entry->state == ANSER_CHANNEL_PENDING)
		entry->state = ANSER_CHANNEL_COLLECTING;

	if (payload != NULL && payload_len > 0)
	{
		if (!AnserStorePayloadDSM(entry, payload, payload_len))
		{
			entry->state = ANSER_CHANNEL_CANCELLED;
			entry->updated_at = GetCurrentTimestamp();
			SetLatch(&AnserCtl->send_latch);
			LWLockRelease(AnserChannelLock);
			return false;
		}
	}

	entry->done_producers++;
	if (entry->done_producers >= entry->expected_producers)
		entry->state = ANSER_CHANNEL_READY;
	entry->updated_at = GetCurrentTimestamp();

	SetLatch(&AnserCtl->gather_latch);
	SetLatch(&AnserCtl->send_latch);
	LWLockRelease(AnserChannelLock);

	return true;
}

bool
AnserWaitProducersRegistered(const AnserChannelKey *channel_key, long timeout_ms)
{
	bool		cancelled = false;

	return AnserWaitForState(channel_key, timeout_ms, true, &cancelled) &&
		!cancelled;
}

bool
AnserWaitReady(const AnserChannelKey *channel_key, bool *cancelled)
{
	return AnserWaitForState(channel_key, -1, false, cancelled);
}

bool
AnserConsumeReady(const AnserChannelKey *channel_key, void *buffer,
				  Size buffer_size, Size *payload_len, bool *cancelled)
{
	AnserChannelEntry *entry;
	bool		found;
	bool		ready;
	bool		is_cancelled;
	bool		delivered = false;

	Assert(payload_len != NULL);
	Assert(cancelled != NULL);

	*payload_len = 0;
	*cancelled = false;

	if (!AnserInitialized() || channel_key == NULL)
		return false;

	LWLockAcquire(AnserChannelLock, LW_SHARED);
	entry = (AnserChannelEntry *) hash_search(AnserChannelHash,
										 channel_key,
										 HASH_FIND,
										 &found);
	if (!found)
	{
		LWLockRelease(AnserChannelLock);
		return false;
	}

	ready = (entry->state == ANSER_CHANNEL_READY);
	is_cancelled = (entry->state == ANSER_CHANNEL_CANCELLED);
	if (ready && !is_cancelled)
		delivered = AnserDeliverChannelData(entry, buffer, buffer_size,
										 payload_len);
	LWLockRelease(AnserChannelLock);

	if (!ready || is_cancelled || !delivered)
	{
		if (is_cancelled)
			*cancelled = true;
		return false;
	}

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	entry = (AnserChannelEntry *) hash_search(AnserChannelHash,
										 channel_key,
										 HASH_FIND,
										 &found);
	if (!found)
	{
		LWLockRelease(AnserChannelLock);
		return false;
	}

	is_cancelled = (entry->state == ANSER_CHANNEL_CANCELLED);
	if (is_cancelled)
	{
		*cancelled = true;
		LWLockRelease(AnserChannelLock);
		return false;
	}

	if (entry->state != ANSER_CHANNEL_READY)
	{
		LWLockRelease(AnserChannelLock);
		return false;
	}

	entry->done_consumers++;
	if (entry->consumers == 0 || entry->done_consumers >= entry->consumers)
	{
		entry->state = ANSER_CHANNEL_CONSUMED;
		AnserReleasePayloadDSM(entry);
	}
	entry->updated_at = GetCurrentTimestamp();
	LWLockRelease(AnserChannelLock);

	return true;
}

AnserChannelState
AnserChannelGetState(const AnserChannelKey *channel_key, bool *found)
{
	AnserChannelEntry *entry;
	bool		local_found;
	AnserChannelState state = ANSER_CHANNEL_CANCELLED;

	if (found != NULL)
		*found = false;

	if (!AnserInitialized() || channel_key == NULL)
		return state;

	LWLockAcquire(AnserChannelLock, LW_SHARED);
	entry = (AnserChannelEntry *) hash_search(AnserChannelHash,
										 channel_key,
										 HASH_FIND,
										 &local_found);
	if (local_found)
		state = entry->state;
	LWLockRelease(AnserChannelLock);

	if (found != NULL)
		*found = local_found;
	return state;
}

/*
 * Bytes of payload the channel currently holds, or -1 if it is not in the map.
 * Introspection for tests observing the payload-DSM lifetime: > 0 while a
 * payload is pinned, 0 once it has been freed but the entry still lingers, and
 * -1 once the entry has been reclaimed (payload freed and removed).
 */
int
AnserChannelPayloadBytes(const AnserChannelKey *channel_key)
{
	AnserChannelEntry *entry;
	bool		found;
	int			bytes = -1;

	if (!AnserInitialized() || channel_key == NULL)
		return -1;

	LWLockAcquire(AnserChannelLock, LW_SHARED);
	entry = (AnserChannelEntry *) hash_search(AnserChannelHash,
										 channel_key,
										 HASH_FIND,
										 &found);
	if (found)
		bytes = (int) entry->data_len;
	LWLockRelease(AnserChannelLock);

	return bytes;
}

/*
 * Number of consumers currently subscribed to a channel, or -1 if the channel
 * is unknown.  Read-only introspection used by tests to sequence a publish only
 * after all expected consumers have registered.
 */
int
AnserChannelConsumerCount(const AnserChannelKey *channel_key)
{
	AnserChannelEntry *entry;
	bool		found;
	int			count = -1;

	if (!AnserInitialized() || channel_key == NULL)
		return -1;

	LWLockAcquire(AnserChannelLock, LW_SHARED);
	entry = (AnserChannelEntry *) hash_search(AnserChannelHash, channel_key,
											  HASH_FIND, &found);
	if (found)
		count = entry->consumers;
	LWLockRelease(AnserChannelLock);

	return count;
}

void
AnserAttachServiceLatch(bool gather_service)
{
	if (!AnserInitialized())
		return;

	OwnLatch(gather_service ? &AnserCtl->gather_latch : &AnserCtl->send_latch);
}

void
AnserDetachServiceLatch(bool gather_service)
{
	if (!AnserInitialized())
		return;

	DisownLatch(gather_service ? &AnserCtl->gather_latch : &AnserCtl->send_latch);
}

void
AnserWaitServiceLatch(bool gather_service, long timeout_ms)
{
	if (!AnserInitialized())
	{
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 timeout_ms,
						 PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		return;
	}

	if (gather_service)
	{
		(void) WaitLatch(&AnserCtl->gather_latch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 timeout_ms,
						 PG_WAIT_EXTENSION);
		ResetLatch(&AnserCtl->gather_latch);
	}
	else
	{
		(void) WaitLatch(&AnserCtl->send_latch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 timeout_ms,
						 PG_WAIT_EXTENSION);
		ResetLatch(&AnserCtl->send_latch);
	}
}

void
AnserWakeServiceLatch(bool gather_service)
{
	if (!AnserInitialized())
		return;

	if (gather_service)
		SetLatch(&AnserCtl->gather_latch);
	else
		SetLatch(&AnserCtl->send_latch);
}

void
AnserServiceMaintenance(void)
{
	if (!AnserInitialized())
		return;

	/*
	 * The periodic sweep is gated so tests can pause reclamation and observe
	 * terminal (CANCELLED/CONSUMED) channels deterministically.  Emergency
	 * reclamation on a full map is not gated -- it calls the sweep directly.
	 */
	if (!AnserCtl->sweep_enabled)
		return;

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	AnserSweepOrphanChannels();
	LWLockRelease(AnserChannelLock);
}

/*
 * Enable or disable the periodic maintenance sweep.  Test-only: production
 * always leaves it enabled.  Toggling it lets a test freeze terminal channels
 * in place (to assert their state) and then re-enable + force a sweep to prove
 * reclamation works.
 */
void
AnserSetSweepEnabled(bool enabled)
{
	if (!AnserInitialized())
		return;

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	AnserCtl->sweep_enabled = enabled;
	LWLockRelease(AnserChannelLock);
}

void
AnserCancelQuery(int gp_session_id, int gp_command_count)
{
	HASH_SEQ_STATUS status;
	AnserChannelEntry *entry;

	if (!AnserInitialized())
		return;

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	hash_seq_init(&status, AnserChannelHash);
	while ((entry = (AnserChannelEntry *) hash_seq_search(&status)) != NULL)
	{
		if (entry->key.gp_session_id == gp_session_id &&
			entry->key.gp_command_count == gp_command_count)
		{
			/*
			 * Do not free the payload DSM here: a READY channel may already have
			 * DELIVERED wait slots borrowing it (consumers mid-read).  Just mark
			 * it cancelled; the sweep releases the DSM once no slot still
			 * references it (unlike the gather/timeout cancels, this one can hit
			 * a channel past READY).
			 */
			entry->state = ANSER_CHANNEL_CANCELLED;
			entry->updated_at = GetCurrentTimestamp();
		}
	}
	SetLatch(&AnserCtl->send_latch);
	LWLockRelease(AnserChannelLock);
}

/*
 * Register/refresh a channel on behalf of a remote producer and arm the produce
 * deadline by moving it to COLLECTING.  Idempotent: repeated begins from the
 * several producers of one channel just refresh expected_producers and the
 * deadline.  This is the "a producer opened a connection" signal.
 */
bool
AnserProducerBegin(const AnserChannelKey *channel_key, int expected_producers,
				   Oid caller_role, bool caller_is_super)
{
	AnserChannelEntry *entry;
	bool		found;
	TimestampTz now;

	if (!AnserInitialized() || channel_key == NULL)
		return false;

	if (expected_producers <= 0)
	{
		ereport(WARNING,
				(errmsg("could not begin Anser channel: expected producers must be greater than zero")));
		return false;
	}

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	entry = (AnserChannelEntry *) hash_search(AnserChannelHash, channel_key,
											  HASH_ENTER_NULL, &found);
	if (entry == NULL)
	{
		AnserSweepOrphanChannels();
		entry = (AnserChannelEntry *) hash_search(AnserChannelHash, channel_key,
												  HASH_ENTER_NULL, &found);
	}

	if (entry == NULL)
	{
		LWLockRelease(AnserChannelLock);
		ereport(WARNING,
				(errmsg("could not begin Anser channel: channel map is full")));
		return false;
	}

	/*
	 * A live channel belongs to the role that created it: another role may not
	 * hijack it by guessing its (session, command, condition) key.
	 */
	if (found &&
		entry->state != ANSER_CHANNEL_CANCELLED &&
		entry->state != ANSER_CHANNEL_CONSUMED &&
		!caller_is_super &&
		OidIsValid(entry->creator_role) &&
		entry->creator_role != caller_role)
	{
		LWLockRelease(AnserChannelLock);
		return false;
	}

	now = GetCurrentTimestamp();
	if (!found)
	{
		MemSet(entry, 0, sizeof(AnserChannelEntry));
		entry->key = *channel_key;
		entry->state = ANSER_CHANNEL_PENDING;
		entry->creator_role = caller_role;
		entry->dsm_handle = DSM_HANDLE_INVALID;
	}
	else if (entry->state == ANSER_CHANNEL_CANCELLED ||
			 entry->state == ANSER_CHANNEL_CONSUMED)
	{
		/*
		 * Terminal channel already on this key -- an anomaly, since keys are
		 * unique per (session, command, condition).  Do not resurrect it:
		 * reviving a completed/aborted channel could strand a straggler wait slot
		 * or hand one query's data to another.  Fail so the caller falls open and
		 * the sweep reclaims the leftover.
		 */
		LWLockRelease(AnserChannelLock);
		return false;
	}

	entry->expected_producers = expected_producers;

	/*
	 * Fix the consumer count when the channel is created, rather than having
	 * every consumer re-assert it: it is a property of the query topology (one
	 * consumer per segment executing the consumer slice) known here.  The send
	 * service must deliver to all of them before recycling the payload.
	 *
	 * getgpsegmentCount() is the per-segment count, which matches the
	 * segment-executed filters Anser targets; a coordinator-only consumer
	 * slice would want 1, which this proxy does not represent.
	 */
	entry->expected_consumers = getgpsegmentCount();

	if (entry->state == ANSER_CHANNEL_PENDING)
		entry->state = ANSER_CHANNEL_COLLECTING;
	entry->updated_at = now;

	SetLatch(&AnserCtl->gather_latch);
	LWLockRelease(AnserChannelLock);

	return true;
}

/*
 * Remote-producer publish: hand one part to the gather service and block for
 * its ACK.  The payload is copied into a DSM segment kept attached for the whole
 * wait, so the gather service can read it by handle.  Fail-open: any local
 * failure downgrades the submission to a cancel so the dataset dies cleanly
 * rather than hanging consumers.
 */
bool
AnserProducerSubmit(const AnserChannelKey *channel_key,
					int expected_producers, const void *payload,
					Size payload_len, bool cancelled,
					Oid caller_role, bool caller_is_super)
{
	dsm_segment *seg = NULL;
	dsm_handle	handle = DSM_HANDLE_INVALID;
	int			slot;
	bool		accepted;

	if (!AnserInitialized() || channel_key == NULL)
		return false;

	/* Refuse to feed a channel owned by a different role. */
	if (!AnserChannelAccessAllowed(channel_key, caller_role, caller_is_super,
								   NULL))
		return false;

	if (!cancelled && payload_len > (Size) gp_anser_max_info_size)
	{
		cancelled = true;
		payload = NULL;
		payload_len = 0;
	}

	if (!cancelled && payload != NULL && payload_len > 0)
	{
		seg = dsm_create(payload_len, DSM_CREATE_NULL_IF_MAXSEGMENTS);
		if (seg == NULL)
		{
			cancelled = true;
			payload_len = 0;
		}
		else
		{
			memcpy(dsm_segment_address(seg), payload, payload_len);
			handle = dsm_segment_handle(seg);
		}
	}

	slot = AnserEnqueueSubmission(channel_key, expected_producers, handle,
								  cancelled ? 0 : payload_len, cancelled);
	if (slot < 0)
	{
		if (seg != NULL)
			dsm_detach(seg);
		return false;
	}

	/*
	 * If we are interrupted while waiting for the ACK, reclaim our submission
	 * slot so it does not linger until this backend exits.  (Our payload DSM is
	 * released by the aborting transaction's resource owner.)
	 */
	PG_TRY();
	{
		accepted = AnserWaitSubmissionAck(slot);
	}
	PG_CATCH();
	{
		AnserAbandonSubmission(slot);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (seg != NULL)
		dsm_detach(seg);

	return accepted;
}

/*
 * Give up a submission slot after the producer is interrupted mid-wait.  If the
 * gather service already finished with it, reclaim it now; if it is still
 * pending, detach ourselves (clear pid/latch) so the gather neither wakes a gone
 * backend nor leaves the slot for us to reclaim -- the reaper frees it once the
 * gather marks it terminal.
 */
static void
AnserAbandonSubmission(int slot)
{
	AnserSubmissionEntry *e = &AnserSubmissionQueue[slot];

	LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
	if (e->producer_pid == MyProcPid)
	{
		if (e->state == ANSER_SUBMIT_ACCEPTED ||
			e->state == ANSER_SUBMIT_REJECTED)
			e->state = ANSER_SUBMIT_FREE;
		else if (e->state == ANSER_SUBMIT_PENDING)
		{
			e->producer_pid = 0;
			e->producer_latch = NULL;
		}
	}
	LWLockRelease(AnserRingLock);
}

/*
 * Claim a free submission slot (waiting for one if the queue is momentarily
 * full) and mark it PENDING for the gather service.  Returns the slot index.
 */
static int
AnserEnqueueSubmission(const AnserChannelKey *channel_key,
					   int expected_producers, dsm_handle handle,
					   Size len, bool cancelled)
{
	int			len_slots = AnserSubmissionQueueLen();

	for (;;)
	{
		int			i;

		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
		for (i = 0; i < len_slots; i++)
		{
			AnserSubmissionEntry *e = &AnserSubmissionQueue[i];

			if (e->state == ANSER_SUBMIT_FREE)
			{
				e->key = *channel_key;
				e->expected_producers = expected_producers;
				e->dsm_handle = handle;
				e->len = len;
				e->cancelled = cancelled;
				e->producer_pid = MyProcPid;
				e->producer_latch = &MyProc->procLatch;
				e->state = ANSER_SUBMIT_PENDING;
				LWLockRelease(AnserRingLock);
				SetLatch(&AnserCtl->gather_latch);
				return i;
			}
		}
		LWLockRelease(AnserRingLock);

		ResetLatch(MyLatch);
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 ANSER_WAIT_POLL_INTERVAL_MS, PG_WAIT_EXTENSION);
	}
}

/*
 * Block on the proc latch until the gather service reaches a terminal state for
 * our slot, then release the slot and report whether the part was accepted.
 */
static bool
AnserWaitSubmissionAck(int slot)
{
	AnserSubmissionEntry *e = &AnserSubmissionQueue[slot];

	for (;;)
	{
		AnserSubmissionState st;

		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
		st = e->state;
		if (st == ANSER_SUBMIT_ACCEPTED || st == ANSER_SUBMIT_REJECTED)
		{
			e->state = ANSER_SUBMIT_FREE;
			LWLockRelease(AnserRingLock);
			return st == ANSER_SUBMIT_ACCEPTED;
		}
		LWLockRelease(AnserRingLock);

		ResetLatch(MyLatch);
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 ANSER_WAIT_LATCH_TIMEOUT_MS, PG_WAIT_EXTENSION);
	}
}

/*
 * Remote-consumer wait: subscribe, register a wait slot, and block on the proc
 * latch until the send service delivers a payload or cancels this consumer.  On
 * success *payload points at a freshly palloc'd copy of the bytes.  The calling
 * backend does no channel-map polling; the wait happens entirely here.
 */
bool
AnserConsumerWait(const AnserChannelKey *channel_key, void **payload,
				  Size *payload_len, bool *cancelled,
				  Oid caller_role, bool caller_is_super)
{
	int			slot;
	bool		result;

	if (payload != NULL)
		*payload = NULL;
	if (payload_len != NULL)
		*payload_len = 0;
	if (cancelled != NULL)
		*cancelled = false;

	if (!AnserInitialized() || channel_key == NULL)
		return false;

	/*
	 * Wait for a producer to announce the channel before subscribing.  In the
	 * plan tree producers sit below their consumers, so the channel is usually
	 * registered first; but execution order across the cluster is not
	 * guaranteed, so a consumer that arrives early waits (up to
	 * gp_anser_timeout_ms) for registration instead of failing open at once.
	 * A false return means the producer never registered in time, or the
	 * dataset was already cancelled -- either way this consumer fails open.
	 */
	if (!AnserWaitProducersRegistered(channel_key, (long) gp_anser_timeout_ms))
	{
		if (cancelled != NULL)
			*cancelled = true;
		return false;
	}

	/*
	 * Only the owning role (or a superuser) may read a channel.  Checked after
	 * registration so there is a recorded creator_role to compare against.
	 */
	if (!AnserChannelAccessAllowed(channel_key, caller_role, caller_is_super,
								   NULL))
		return false;

	/*
	 * Subscribe before registering the wait slot so the channel's consumer
	 * count is never lower than the number of live wait slots; the send service
	 * relies on that ordering for its recycle accounting.
	 */
	if (!AnserSubscribe(channel_key))
		return false;

	slot = AnserRegisterWaitSlot(channel_key);
	if (slot < 0)
	{
		if (cancelled != NULL)
			*cancelled = true;
		return false;
	}

	SetLatch(&AnserCtl->send_latch);

	/*
	 * Reclaim our wait slot (and unpin any payload the send service already
	 * stamped) if we are interrupted before collecting the result, so it does
	 * not linger until this backend exits.
	 */
	PG_TRY();
	{
		result = AnserWaitSlotResult(slot, payload, payload_len, cancelled);
	}
	PG_CATCH();
	{
		AnserAbandonWaitSlot(slot);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return result;
}

/*
 * Give up a wait slot after the consumer is interrupted mid-wait, unpinning any
 * per-consumer payload copy the send service stamped but we never collected.
 * Guarded by pid so a slot already reclaimed and reused is left untouched.
 */
static void
AnserAbandonWaitSlot(int slot)
{
	AnserWaitSlot *s = &AnserWaitTable[slot];
	AnserChannelKey key;
	bool		was_waiting = false;

	LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
	if (s->consumer_pid == MyProcPid && s->state != ANSER_WAIT_FREE)
	{
		/*
		 * The slot's dsm_handle is borrowed from the channel (which owns and
		 * frees the payload DSM), so abandoning just stops this slot from
		 * borrowing -- do not unpin it here.
		 */
		was_waiting = (s->state == ANSER_WAIT_WAITING);
		key = s->key;
		s->dsm_handle = DSM_HANDLE_INVALID;
		s->len = 0;
		s->state = ANSER_WAIT_FREE;
	}
	LWLockRelease(AnserRingLock);

	/*
	 * A consumer that abandons before any data was delivered to it must no
	 * longer count toward the channel's expected consumer total; otherwise the
	 * send service's "delivered to every consumer" recycle test can never be
	 * satisfied and the channel lingers in READY forever.  (A slot that was
	 * already DELIVERED is left counted: the send service incremented
	 * done_consumers for it, so the accounting still balances.)
	 */
	if (was_waiting)
	{
		AnserChannelEntry *entry;
		bool		found;

		LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
		entry = (AnserChannelEntry *) hash_search(AnserChannelHash, &key,
												  HASH_FIND, &found);
		if (found && entry->consumers > 0)
		{
			entry->consumers--;
			/* Let the send service re-evaluate recycling. */
			SetLatch(&AnserCtl->send_latch);
		}
		LWLockRelease(AnserChannelLock);
	}
}

/*
 * Claim a free wait-table slot for this consumer.  Returns the slot index, or
 * -1 if the table is full (the consumer then fails open).
 */
static int
AnserRegisterWaitSlot(const AnserChannelKey *channel_key)
{
	int			len_slots = AnserWaitTableLen();
	int			i;

	LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
	for (i = 0; i < len_slots; i++)
	{
		AnserWaitSlot *s = &AnserWaitTable[i];

		if (s->state == ANSER_WAIT_FREE)
		{
			s->key = *channel_key;
			s->dsm_handle = DSM_HANDLE_INVALID;
			s->len = 0;
			s->consumer_pid = MyProcPid;
			s->consumer_latch = &MyProc->procLatch;
			s->state = ANSER_WAIT_WAITING;
			LWLockRelease(AnserRingLock);
			return i;
		}
	}
	LWLockRelease(AnserRingLock);

	return -1;
}

/*
 * Block until the send service resolves our wait slot.  On DELIVERED, attach the
 * per-consumer payload segment, copy it into palloc'd memory, and free the
 * segment (the send service pinned it and handed us ownership).
 */
static bool
AnserWaitSlotResult(int slot, void **payload, Size *payload_len,
					bool *cancelled)
{
	AnserWaitSlot *s = &AnserWaitTable[slot];
	AnserChannelKey slot_key = s->key;

	for (;;)
	{
		AnserWaitSlotState st;
		dsm_handle	handle = DSM_HANDLE_INVALID;
		Size		len = 0;

		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
		st = s->state;
		if (st == ANSER_WAIT_DELIVERED)
		{
			/*
			 * Read the borrowed channel payload handle but leave the slot
			 * DELIVERED: that keeps the channel's payload DSM alive (the sweep
			 * will not reclaim a channel with a DELIVERED slot) until we have
			 * copied it out below.  We flip the slot to FREE only afterward.
			 */
			handle = s->dsm_handle;
			len = s->len;
		}
		else if (st == ANSER_WAIT_CANCELLED)
		{
			s->state = ANSER_WAIT_FREE;
		}
		LWLockRelease(AnserRingLock);

		if (st == ANSER_WAIT_CANCELLED)
		{
			if (cancelled != NULL)
				*cancelled = true;
			return false;
		}

		if (st == ANSER_WAIT_DELIVERED)
		{
			void	   *buf = NULL;
			bool		vanished = false;

			if (handle != DSM_HANDLE_INVALID)
			{
				dsm_segment *seg = dsm_attach(handle);

				if (seg == NULL)
					vanished = true;	/* should not happen: we hold DELIVERED */
				else
				{
					if (len > 0)
					{
						buf = palloc(len);
						memcpy(buf, dsm_segment_address(seg), len);
					}
					/* Borrowed handle -- detach, but the channel owns/frees it. */
					dsm_detach(seg);
				}
			}

			/* Done reading: release the slot so the channel can be reclaimed. */
			LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
			if (s->state == ANSER_WAIT_DELIVERED)
				s->state = ANSER_WAIT_FREE;
			LWLockRelease(AnserRingLock);

			if (vanished)
			{
				if (cancelled != NULL)
					*cancelled = true;
				return false;
			}

			if (payload != NULL)
				*payload = buf;
			if (payload_len != NULL)
				*payload_len = len;
			return true;
		}

		/*
		 * Still WAITING.  Guard against the registration/recycle race: if our
		 * channel has already been recycled (CONSUMED/CANCELLED) or swept out of
		 * the map between AnserWaitProducersRegistered and our slot
		 * registration, the send service will never resolve this slot -- there
		 * is no live payload to deliver.  Reclaim the slot ourselves and fail
		 * open rather than block forever.
		 */
		if (st == ANSER_WAIT_WAITING)
		{
			bool		found = false;
			AnserChannelState cstate = AnserChannelGetState(&slot_key, &found);

			if (!found ||
				cstate == ANSER_CHANNEL_CANCELLED ||
				cstate == ANSER_CHANNEL_CONSUMED)
			{
				LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
				if (s->consumer_pid == MyProcPid &&
					s->state == ANSER_WAIT_WAITING)
					s->state = ANSER_WAIT_FREE;
				LWLockRelease(AnserRingLock);

				if (cancelled != NULL)
					*cancelled = true;
				return false;
			}
		}

		ResetLatch(MyLatch);
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 ANSER_WAIT_LATCH_TIMEOUT_MS, PG_WAIT_EXTENSION);
	}
}

/*
 * One gather-service pass: drain the submission queue, cancel channels that have
 * sat in COLLECTING past the produce deadline, and reclaim slots left behind by
 * producers that died mid-wait.
 */
void
AnserGatherServiceCycle(void)
{
	int			len_slots;
	int			i;

	if (!AnserInitialized() || AnserSubmissionQueue == NULL)
		return;

	len_slots = AnserSubmissionQueueLen();
	for (i = 0; i < len_slots; i++)
	{
		AnserSubmissionEntry *e = &AnserSubmissionQueue[i];
		AnserChannelKey key;
		int32		expected_producers;
		dsm_handle	handle;
		Size		len;
		bool		cancelled;
		bool		accepted;

		LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
		if (e->state != ANSER_SUBMIT_PENDING)
		{
			LWLockRelease(AnserRingLock);
			continue;
		}
		key = e->key;
		expected_producers = e->expected_producers;
		handle = e->dsm_handle;
		len = e->len;
		cancelled = e->cancelled;
		LWLockRelease(AnserRingLock);

		accepted = AnserGatherApply(&key, expected_producers, handle, len,
									cancelled);

		LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
		/* The slot is still ours: only the gather service leaves PENDING. */
		e->state = accepted ? ANSER_SUBMIT_ACCEPTED : ANSER_SUBMIT_REJECTED;
		if (e->producer_latch != NULL)
			SetLatch(e->producer_latch);
		LWLockRelease(AnserRingLock);
	}

	AnserCancelStaleChannels();
	AnserReapSubmissionSlots();
}

/*
 * Apply one submitted part to its channel: attach the producer's payload, append
 * it (or cancel the dataset), and advance the channel toward READY.  Mirrors the
 * direct AnserPublish path but sourced from a DSM handle.
 */
static bool
AnserGatherApply(const AnserChannelKey *channel_key, int expected_producers,
				 dsm_handle handle, Size len, bool cancelled)
{
	AnserChannelEntry *entry;
	bool		found;
	dsm_segment *seg = NULL;
	void	   *addr = NULL;

	if (!cancelled && handle != DSM_HANDLE_INVALID && len > 0)
	{
		seg = dsm_attach(handle);
		if (seg == NULL)
			cancelled = true;	/* producer gone / segment lost */
		else
			addr = dsm_segment_address(seg);
	}

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	entry = (AnserChannelEntry *) hash_search(AnserChannelHash, channel_key,
											  HASH_FIND, &found);

	/*
	 * No producer_begin registered this channel (or it was already recycled):
	 * refuse the part rather than creating an unowned channel, which would
	 * bypass the creator_role access check.  The client always begins before
	 * publishing, so a legitimate part always finds its channel here.  A
	 * dataset already in a terminal state likewise refuses late parts.
	 */
	if (!found ||
		entry->state == ANSER_CHANNEL_CANCELLED ||
		entry->state == ANSER_CHANNEL_CONSUMED)
	{
		LWLockRelease(AnserChannelLock);
		if (seg != NULL)
			dsm_detach(seg);
		return false;
	}

	if (expected_producers > 0)
		entry->expected_producers = expected_producers;

	if (cancelled)
	{
		entry->state = ANSER_CHANNEL_CANCELLED;
		entry->updated_at = GetCurrentTimestamp();
		AnserReleasePayloadDSM(entry);
		LWLockRelease(AnserChannelLock);
		if (seg != NULL)
			dsm_detach(seg);
		SetLatch(&AnserCtl->send_latch);
		return true;
	}

	if (entry->state == ANSER_CHANNEL_PENDING)
		entry->state = ANSER_CHANNEL_COLLECTING;

	if (addr != NULL && len > 0)
	{
		if (!AnserStorePayloadDSM(entry, addr, len))
		{
			entry->state = ANSER_CHANNEL_CANCELLED;
			entry->updated_at = GetCurrentTimestamp();
			AnserReleasePayloadDSM(entry);
			LWLockRelease(AnserChannelLock);
			if (seg != NULL)
				dsm_detach(seg);
			SetLatch(&AnserCtl->send_latch);
			return false;
		}
	}

	entry->done_producers++;
	if (entry->done_producers >= entry->expected_producers)
		entry->state = ANSER_CHANNEL_READY;
	entry->updated_at = GetCurrentTimestamp();
	LWLockRelease(AnserChannelLock);

	if (seg != NULL)
		dsm_detach(seg);
	SetLatch(&AnserCtl->send_latch);

	return true;
}

/*
 * Cancel any channel that announced producers (COLLECTING) but did not reach
 * READY within gp_anser_timeout_ms.  Cancellation is whole-dataset:
 * all-parts-or-nothing.
 */
static void
AnserCancelStaleChannels(void)
{
	HASH_SEQ_STATUS status;
	AnserChannelEntry *entry;
	TimestampTz now = GetCurrentTimestamp();
	bool		any = false;

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	hash_seq_init(&status, AnserChannelHash);
	while ((entry = (AnserChannelEntry *) hash_seq_search(&status)) != NULL)
	{
		if (entry->state == ANSER_CHANNEL_COLLECTING &&
			TimestampDifferenceExceeds(entry->updated_at, now,
									   gp_anser_timeout_ms))
		{
			entry->state = ANSER_CHANNEL_CANCELLED;
			entry->updated_at = now;
			AnserReleasePayloadDSM(entry);
			any = true;
		}
	}
	LWLockRelease(AnserChannelLock);

	if (any)
		SetLatch(&AnserCtl->send_latch);
}

/*
 * Reclaim terminal submission slots whose producer backend has exited without
 * consuming the ACK (e.g. cancelled mid-wait).
 */
static void
AnserReapSubmissionSlots(void)
{
	int			len_slots = AnserSubmissionQueueLen();
	int			i;

	LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
	for (i = 0; i < len_slots; i++)
	{
		AnserSubmissionEntry *e = &AnserSubmissionQueue[i];

		if ((e->state == ANSER_SUBMIT_ACCEPTED ||
			 e->state == ANSER_SUBMIT_REJECTED) &&
			!AnserPidIsLive(e->producer_pid))
			e->state = ANSER_SUBMIT_FREE;
	}
	LWLockRelease(AnserRingLock);
}

/*
 * One send-service pass: deliver every READY/CANCELLED channel to its waiting
 * consumers and reclaim slots left behind by consumers that have exited.
 */
void
AnserSendServiceCycle(void)
{
	HASH_SEQ_STATUS status;
	AnserChannelEntry *entry;
	int			len_slots;

	if (!AnserInitialized() || AnserWaitTable == NULL)
		return;

	len_slots = AnserWaitTableLen();

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	hash_seq_init(&status, AnserChannelHash);
	while ((entry = (AnserChannelEntry *) hash_seq_search(&status)) != NULL)
	{
		bool		ready = (entry->state == ANSER_CHANNEL_READY);

		/*
		 * Stragglers that registered after a channel finished (cancelled, or
		 * already consumed) can no longer be handed data; they are delivered a
		 * cancel so they fail open instead of blocking forever on a channel the
		 * sweep would otherwise never reclaim.
		 */
		bool		cancel_waiters = (entry->state == ANSER_CHANNEL_CANCELLED ||
									  entry->state == ANSER_CHANNEL_CONSUMED);
		int			i;

		if (!ready && !cancel_waiters)
			continue;

		LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
		for (i = 0; i < len_slots; i++)
		{
			AnserWaitSlot *s = &AnserWaitTable[i];

			if (s->state != ANSER_WAIT_WAITING)
				continue;
			if (memcmp(&s->key, &entry->key, sizeof(AnserChannelKey)) != 0)
				continue;

			if (cancel_waiters)
			{
				s->dsm_handle = DSM_HANDLE_INVALID;
				s->len = 0;
				s->state = ANSER_WAIT_CANCELLED;
				if (s->consumer_latch != NULL)
					SetLatch(s->consumer_latch);
				continue;
			}

			/*
			 * READY: lend this consumer the channel's single payload segment --
			 * the slot borrows entry->dsm_handle rather than getting its own
			 * copy.  The consumer copies it out and only then frees the slot; the
			 * payload DSM is released once the channel is reclaimed with no slot
			 * still borrowing it (AnserChannelHasWaiters / the sweep).  Holding
			 * the slot DELIVERED keeps that handle alive across the read.
			 */
			s->dsm_handle = entry->dsm_handle;
			s->len = entry->data_len;
			s->state = ANSER_WAIT_DELIVERED;
			if (s->consumer_latch != NULL)
				SetLatch(s->consumer_latch);

			entry->done_consumers++;
		}
		LWLockRelease(AnserRingLock);

		/*
		 * Recycle once every expected consumer has been handed the payload.  Do
		 * NOT free the payload DSM here: consumers still hold DELIVERED slots
		 * that borrow it.  It is released when the sweep reclaims this now
		 * terminal channel, after every borrowing slot has drained.
		 */
		if (ready && entry->expected_consumers > 0 &&
			entry->done_consumers >= entry->expected_consumers)
		{
			entry->state = ANSER_CHANNEL_CONSUMED;
			entry->updated_at = GetCurrentTimestamp();
		}
	}
	LWLockRelease(AnserChannelLock);

	AnserReapWaitSlots();
}

/*
 * Reclaim wait slots whose consumer backend has exited, unpinning any payload
 * copy the send service already stamped but the consumer never collected.
 */
static void
AnserReapWaitSlots(void)
{
	int			len_slots = AnserWaitTableLen();
	int			i;

	LWLockAcquire(AnserRingLock, LW_EXCLUSIVE);
	for (i = 0; i < len_slots; i++)
	{
		AnserWaitSlot *s = &AnserWaitTable[i];

		if (s->state == ANSER_WAIT_FREE)
			continue;
		if (AnserPidIsLive(s->consumer_pid))
			continue;

		/*
		 * A dead consumer's slot is freed without touching its dsm_handle: that
		 * handle is borrowed from the channel (the channel owns and frees the
		 * payload DSM), so freeing the slot just stops it from borrowing.
		 */
		s->dsm_handle = DSM_HANDLE_INVALID;
		s->len = 0;
		s->state = ANSER_WAIT_FREE;
	}
	LWLockRelease(AnserRingLock);
}

/*
 * Effective size of the channel map.
 *
 * When gp_anser_max_channels is set explicitly (> 0) it wins.  Otherwise the map
 * is auto-sized to max_connections * max_slices: at most MaxConnections
 * concurrent queries, each opening up to gp_max_slices runtime-filter channels.
 * gp_max_slices == 0 means "unbounded", for which we substitute a fixed
 * per-connection budget (ANSER_AUTO_SLICES_PER_CONN) so the map stays finite.
 *
 * This value sizes fixed shared memory at postmaster start, so it must be stable
 * for the life of the postmaster and identical in every backend.  MaxConnections
 * is PGC_POSTMASTER (stable), but gp_max_slices is PGC_USERSET, so we cache the
 * computed value on first use.  That first use is the postmaster's shmem-sizing
 * pass (before any backend forks or any session runs SET), so the cache captures
 * the postmaster-level gp_max_slices and is inherited unchanged by every
 * backend -- a later per-session SET gp_max_slices cannot resize the map.
 *
 * Exposed (non-static) so the regression suite can prove the cache holds: see
 * anser_test_max_channels_stable_across_slices().
 */
int
AnserMaxChannels(void)
{
	static int	cached = 0;
	int			slices;
	int64		v;

	if (gp_anser_max_channels > 0)
		return gp_anser_max_channels;

	if (cached > 0)
		return cached;

	slices = (gp_max_slices > 0) ? gp_max_slices : ANSER_AUTO_SLICES_PER_CONN;
	v = (int64) MaxConnections * (int64) slices;

	if (v < 1)
		v = 1;
	if (v > INT_MAX)
		v = INT_MAX;

	cached = (int) v;
	return cached;
}

static Size
AnserChannelHashSize(void)
{
	return hash_estimate_size(AnserMaxChannels(),
							  sizeof(AnserChannelEntry));
}

/*
 * The submission queue holds parts in flight between blocked producers and the
 * gather service.  Every segment producing for a channel submits its own part,
 * and they hand off concurrently, so -- like the consumer wait table -- we size
 * for one in-flight slot per producer per channel (channels * per-channel
 * producers, which mirrors the per-channel consumer count = segment count).
 * Producers that still find it full wait for a free slot rather than failing.
 */
static int
AnserSubmissionQueueLen(void)
{
	int64		len = (int64) AnserMaxChannels() *
		(int64) gp_anser_max_consumers_per_channel;

	/* Guard against int overflow from extreme GUC settings. */
	if (len > INT_MAX)
		len = INT_MAX;

	return (int) len;
}

static int
AnserWaitTableLen(void)
{
	int64		len = (int64) AnserMaxChannels() *
		(int64) gp_anser_max_consumers_per_channel;

	/* Guard against int overflow from extreme GUC settings. */
	if (len > INT_MAX)
		len = INT_MAX;

	return (int) len;
}

static Size
AnserSubmissionQueueSize(void)
{
	return mul_size(sizeof(AnserSubmissionEntry),
					(Size) AnserSubmissionQueueLen());
}

static Size
AnserWaitTableSize(void)
{
	return mul_size(sizeof(AnserWaitSlot), (Size) AnserWaitTableLen());
}

static void
AnserInitializeSubmissionQueue(bool found)
{
	if (!found)
		MemSet(AnserSubmissionQueue, 0, AnserSubmissionQueueSize());
}

static void
AnserInitializeWaitTable(bool found)
{
	if (!found)
		MemSet(AnserWaitTable, 0, AnserWaitTableSize());
}

static bool
AnserPidIsLive(int pid)
{
	if (pid == 0)
		return false;

	return BackendPidGetProc(pid) != NULL;
}

/*
 * Does any consumer still have a WAITING wait slot for this channel?  Callers
 * hold AnserChannelLock; we take AnserRingLock (channel-lock-then-ring-lock
 * order, matching the send cycle) to read the wait table.
 */
static bool
AnserChannelHasWaiters(const AnserChannelKey *channel_key)
{
	int			len_slots;
	int			i;
	bool		found = false;

	if (AnserWaitTable == NULL)
		return false;

	len_slots = AnserWaitTableLen();
	LWLockAcquire(AnserRingLock, LW_SHARED);
	for (i = 0; i < len_slots; i++)
	{
		/*
		 * A slot still references the channel while it is WAITING (not yet
		 * delivered) or DELIVERED (delivered but the consumer has not finished
		 * copying the borrowed payload out).  Either blocks reclaim: the sweep
		 * must not free the payload DSM while a DELIVERED slot could still attach
		 * it.
		 */
		if ((AnserWaitTable[i].state == ANSER_WAIT_WAITING ||
			 AnserWaitTable[i].state == ANSER_WAIT_DELIVERED) &&
			memcmp(&AnserWaitTable[i].key, channel_key,
				   sizeof(AnserChannelKey)) == 0)
		{
			found = true;
			break;
		}
	}
	LWLockRelease(AnserRingLock);

	return found;
}

/*
 * May this caller produce/consume on the channel?  A superuser always may; any
 * other role may only touch a channel it created.  An unknown channel, or one
 * with no recorded creator, is permitted here -- callers handle "not found"
 * through their normal paths.  If found is non-NULL it receives whether the
 * channel currently exists.
 */
static bool
AnserChannelAccessAllowed(const AnserChannelKey *channel_key, Oid caller_role,
						  bool caller_is_super, bool *found)
{
	AnserChannelEntry *entry;
	bool		local_found;
	bool		allowed = true;

	LWLockAcquire(AnserChannelLock, LW_SHARED);
	entry = (AnserChannelEntry *) hash_search(AnserChannelHash, channel_key,
											  HASH_FIND, &local_found);
	if (local_found && !caller_is_super &&
		OidIsValid(entry->creator_role) &&
		entry->creator_role != caller_role)
		allowed = false;
	LWLockRelease(AnserChannelLock);

	if (found != NULL)
		*found = local_found;

	return allowed;
}

static void
AnserInitializeControl(bool found)
{
	if (!found)
	{
		MemSet(AnserCtl, 0, sizeof(AnserControl));
		AnserCtl->max_channels = AnserMaxChannels();
		AnserCtl->max_info_size = gp_anser_max_info_size;
		InitSharedLatch(&AnserCtl->gather_latch);
		InitSharedLatch(&AnserCtl->send_latch);
		AnserCtl->sweep_enabled = true;
	}
}

static void
AnserInitializeChannelHash(void)
{
	HASHCTL		hctl;

	MemSet(&hctl, 0, sizeof(hctl));
	hctl.keysize = sizeof(AnserChannelKey);
	hctl.entrysize = sizeof(AnserChannelEntry);

	AnserChannelHash = ShmemInitHash(ANSER_CHANNEL_HASH_NAME,
									  AnserMaxChannels(),
									  AnserMaxChannels(),
									  &hctl,
									  HASH_ELEM | HASH_BLOBS);
}

static void
AnserInitializeTokenHash(void)
{
	HASHCTL		hctl;

	MemSet(&hctl, 0, sizeof(hctl));
	hctl.keysize = sizeof(AnserTokenTag);
	hctl.entrysize = sizeof(AnserTokenEntry);
	hctl.hash = tag_hash;

	/* One entry per concurrent session; removed when the session exits. */
	AnserTokenHash = ShmemInitHash(ANSER_TOKEN_HASH_NAME,
								   MaxConnections,
								   MaxConnections,
								   &hctl,
								   HASH_ELEM | HASH_FUNCTION);
}

/*
 * Drop this session's token entry at backend exit.  Registered once by the
 * first AnserGetOrCreateSessionToken() call in the backend.
 */
static void
AnserTokenSessionCleanup(int code, Datum arg)
{
	AnserTokenTag tag;

	if (AnserTokenHash == NULL)
		return;

	tag.session_id = gp_session_id;
	tag.user_id = DatumGetObjectId(arg);

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	(void) hash_search(AnserTokenHash, &tag, HASH_REMOVE, NULL);
	LWLockRelease(AnserChannelLock);
}

/*
 * AnserGetOrCreateSessionToken
 *
 * Return this session's token (palloc'd hex string), generating and
 * registering it on first use.  NULL when the subsystem is off, the user id
 * is invalid, or the token hash is full -- callers fail open (connect without
 * the token, i.e. fall back to pg_hba-driven authentication).
 *
 * user_id must be the *session* user: segment executors connect back to the
 * QD as the session user (cdbconn passes MyProcPort->user_name), regardless
 * of any SET ROLE in effect on the QD.
 */
char *
AnserGetOrCreateSessionToken(Oid user_id)
{
	AnserTokenTag tag;
	AnserTokenEntry *entry;
	bool		found;
	char		token_hex[ANSER_TOKEN_HEX_LEN + 1];
	bool		have_token = false;

	if (!AnserInitialized() || !OidIsValid(user_id))
		return NULL;

	tag.session_id = gp_session_id;
	tag.user_id = user_id;

	/* Copy the token into a stack buffer: no palloc while holding the lock. */
	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	entry = (AnserTokenEntry *) hash_search(AnserTokenHash, &tag,
											HASH_ENTER, &found);
	if (entry != NULL)
	{
		if (!found)
		{
			uint8		token[ANSER_TOKEN_BYTES];

			if (!pg_strong_random(token, ANSER_TOKEN_BYTES))
			{
				(void) hash_search(AnserTokenHash, &tag, HASH_REMOVE, NULL);
				entry = NULL;
			}
			else
			{
				hex_encode((const char *) token, ANSER_TOKEN_BYTES,
						   entry->token_hex);
				entry->token_hex[ANSER_TOKEN_HEX_LEN] = '\0';
			}
		}
		if (entry != NULL)
		{
			strlcpy(token_hex, entry->token_hex, sizeof(token_hex));
			have_token = true;
		}
	}
	LWLockRelease(AnserChannelLock);

	if (!have_token)
		return NULL;

	if (!anser_token_exit_registered)
	{
		anser_token_exit_registered = true;
		before_shmem_exit(AnserTokenSessionCleanup, ObjectIdGetDatum(user_id));
	}

	return pstrdup(token_hex);
}

/*
 * AnserSessionTokenIsValid
 *
 * Token check for the gp_anser_conn authentication branch in
 * libpq/auth.c: true iff some live session of this exact user registered this
 * token.  Runs before InitPostgres in the accepting backend; shared-memory
 * pointers are inherited from the postmaster, so no attach is needed.
 */
bool
AnserSessionTokenIsValid(Oid user_id, const char *token_hex)
{
	HASH_SEQ_STATUS status;
	AnserTokenEntry *entry;
	bool		valid = false;

	if (!AnserInitialized() || !OidIsValid(user_id) || token_hex == NULL ||
		strlen(token_hex) != ANSER_TOKEN_HEX_LEN)
		return false;

	LWLockAcquire(AnserChannelLock, LW_SHARED);
	hash_seq_init(&status, AnserTokenHash);
	while ((entry = (AnserTokenEntry *) hash_seq_search(&status)) != NULL)
	{
		if (entry->tag.user_id == user_id &&
			strcmp(entry->token_hex, token_hex) == 0)
		{
			valid = true;
			hash_seq_term(&status);
			break;
		}
	}
	LWLockRelease(AnserChannelLock);

	return valid;
}

/*
 * Recycle terminal or orphaned channels before declaring registration failure.
 */
static void
AnserSweepOrphanChannels(void)
{
	HASH_SEQ_STATUS status;
	AnserChannelEntry *entry;
	AnserChannelKey *remove_keys;
	int			remove_count = 0;
	int			i;

	Assert(LWLockHeldByMeInMode(AnserChannelLock, LW_EXCLUSIVE));

	if (AnserChannelHash == NULL)
		return;

	remove_keys = (AnserChannelKey *) palloc(sizeof(AnserChannelKey) *
											  (Size) AnserMaxChannels());

	hash_seq_init(&status, AnserChannelHash);
	while ((entry = (AnserChannelEntry *) hash_seq_search(&status)) != NULL)
	{
		bool		recycle = false;

		if (entry->state == ANSER_CHANNEL_CONSUMED ||
			entry->state == ANSER_CHANNEL_CANCELLED)
			recycle = true;
		else if (!AnserChannelOwnerIsAlive(entry))
			recycle = true;

		/*
		 * Never recycle a channel that still has consumers blocked on it: the
		 * send service must first deliver the payload or a cancel to those wait
		 * slots.  Removing the channel out from under them would strand the
		 * consumers, which only wake on their slot.
		 */
		if (recycle && AnserChannelHasWaiters(&entry->key))
			recycle = false;

		if (recycle)
		{
			AnserReleasePayloadDSM(entry);
			remove_keys[remove_count++] = entry->key;
		}
	}

	for (i = 0; i < remove_count; i++)
		(void) hash_search(AnserChannelHash,
						  &remove_keys[i],
						  HASH_REMOVE,
						  NULL);

	pfree(remove_keys);
}

/*
 * Is the query that owns this channel still alive?
 *
 * Validation is deliberately conservative, at session granularity rather than
 * per query/command: a channel lives as long as its owning coordinator session
 * does, and AnserCancelQuery() provides explicit cleanup at query end/failure.
 */
static bool
AnserChannelOwnerIsAlive(const AnserChannelEntry *entry)
{
	Assert(entry != NULL);

	/*
	 * A channel belongs to one query, identified by gp_session_id.  It is alive
	 * as long as that coordinator (QD) session still has a backend in the proc
	 * array; once the session is gone -- query finished/aborted, or a fixed test
	 * session id that never maps to a live backend -- the channel is orphaned and
	 * may be reclaimed.
	 *
	 * Liveness is deliberately tied to the session, NOT to the backend that
	 * created the channel: network-path producers create it from a short-lived
	 * libpq request backend (AnserClientPublish PQfinish's the connection right
	 * after publishing), so that backend is normally already gone while the
	 * channel is still needed by consumers.
	 */
	if (entry->key.gp_session_id <= 0)
		return true;			/* no session to check against; keep it */

	return FindProcByGpSessionId((long) entry->key.gp_session_id) != NULL;
}

static bool
AnserStorePayloadDSM(AnserChannelEntry *entry, const void *payload,
						   Size payload_len)
{
	dsm_segment *acc_seg = NULL;
	dsm_segment *new_seg;
	void	   *acc_addr = NULL;

	Assert(LWLockHeldByMeInMode(AnserChannelLock, LW_EXCLUSIVE));
	Assert(entry != NULL);

	if (payload == NULL || payload_len == 0)
		return true;

	if (payload_len > (Size) gp_anser_max_info_size)
		return false;

	/* Attach the channel's running merged payload, if it already has one. */
	if (entry->dsm_handle != DSM_HANDLE_INVALID && entry->data_len > 0)
	{
		acc_seg = dsm_attach(entry->dsm_handle);
		if (acc_seg == NULL)
			return false;
		acc_addr = dsm_segment_address(acc_seg);
	}

	/*
	 * Subsequent part: fold it into the existing payload in place.  Every part on
	 * a channel shares the same (condition-key-derived) bloom parameters, so it is
	 * the same serialized size and the union is a bitwise OR of the bitsets -- no
	 * fresh segment, no full-payload copy.  Safe because we hold AnserChannelLock
	 * and consumers only ever read their own copies.  A part that cannot fold in
	 * place (wrong size, malformed) is rejected: the caller then cancels the
	 * channel and its consumers fail open.
	 */
	if (acc_addr != NULL)
	{
		bool		folded = AnserBloomFoldPartInPlace(acc_addr, entry->data_len,
													   payload, payload_len);

		dsm_detach(acc_seg);
		return folded;
	}

	/*
	 * First part: store it verbatim in a fresh, pinned segment.  The coordinator
	 * never reconstructs a filter from the payload -- it only copies the first
	 * part and OR-folds the rest -- so no bitset parameters are needed here.
	 */
	new_seg = dsm_create(payload_len, DSM_CREATE_NULL_IF_MAXSEGMENTS);
	if (new_seg == NULL)
		return false;
	memcpy(dsm_segment_address(new_seg), payload, payload_len);

	AnserReleasePayloadDSM(entry);
	dsm_pin_segment(new_seg);
	entry->dsm_handle = dsm_segment_handle(new_seg);
	entry->data_len = payload_len;
	dsm_detach(new_seg);

	return true;
}

static void
AnserReleasePayloadDSM(AnserChannelEntry *entry)
{
	Assert(LWLockHeldByMeInMode(AnserChannelLock, LW_EXCLUSIVE));

	if (entry == NULL || entry->dsm_handle == DSM_HANDLE_INVALID)
		return;

	dsm_unpin_segment(entry->dsm_handle);
	entry->dsm_handle = DSM_HANDLE_INVALID;
	entry->data_len = 0;
}

/*
 * Consume a ready channel payload.
 */
static bool
AnserDeliverChannelData(const AnserChannelEntry *entry, void *buffer,
						Size buffer_size, Size *payload_len)
{
	dsm_segment *seg;
	void	   *addr;

	Assert(LWLockHeldByMe(AnserChannelLock));
	Assert(entry != NULL);
	Assert(payload_len != NULL);

	if (entry->dsm_handle == DSM_HANDLE_INVALID)
	{
		*payload_len = 0;
		return (entry->data_len == 0);
	}

	if (buffer == NULL && entry->data_len > 0)
		return false;

	if (buffer_size < entry->data_len)
		return false;

	seg = dsm_attach(entry->dsm_handle);
	if (seg == NULL)
		return false;

	addr = dsm_segment_address(seg);
	if (entry->data_len > 0)
		memcpy(buffer, addr, entry->data_len);
	dsm_detach(seg);

	*payload_len = entry->data_len;

	return true;
}

static bool
AnserWaitForState(const AnserChannelKey *channel_key, long timeout_ms,
				  bool registration_only, bool *cancelled)
{
	TimestampTz start_time = GetCurrentTimestamp();

	if (cancelled != NULL)
		*cancelled = false;

	if (!AnserInitialized() || channel_key == NULL)
		return false;

	for (;;)
	{
		AnserChannelEntry *entry;
		bool		found;
		bool		registered;
		bool		ready;
		bool		is_cancelled;

		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(AnserChannelLock, LW_SHARED);
		entry = (AnserChannelEntry *) hash_search(AnserChannelHash,
										 channel_key,
										 HASH_FIND,
										 &found);
		registered = found && entry->expected_producers > 0;
		ready = found && entry->state == ANSER_CHANNEL_READY;
		is_cancelled = found && entry->state == ANSER_CHANNEL_CANCELLED;
		LWLockRelease(AnserChannelLock);

		if (is_cancelled)
		{
			if (cancelled != NULL)
				*cancelled = true;
			return false;
		}

		if (registration_only)
		{
			if (registered)
				return true;
		}
		else if (ready)
			return true;

		if (timeout_ms >= 0 &&
			TimestampDifferenceExceeds(start_time, GetCurrentTimestamp(),
									   timeout_ms))
			return false;

		ResetLatch(MyLatch);
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 ANSER_WAIT_POLL_INTERVAL_MS,
						 PG_WAIT_EXTENSION);
	}
}

static bool
AnserInitialized(void)
{
	return gp_anser_enable && AnserCtl != NULL && AnserChannelHash != NULL &&
		AnserTokenHash != NULL;
}
