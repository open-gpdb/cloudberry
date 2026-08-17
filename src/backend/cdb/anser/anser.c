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
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/hsearch.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#define ANSER_CONTROL_NAME		"Anser Control"
#define ANSER_CHANNEL_HASH_NAME	"Anser Channel Hash"
#define ANSER_DATA_ARENA_NAME	"Anser Data Arena"
#define ANSER_ARENA_SLOTS_NAME	"Anser Arena Slots"

bool		gp_anser_enable = false;
int			gp_anser_max_channels = 128;
int			gp_anser_max_info_size = 1024 * 1024;
int			gp_anser_timeout_ms = 1000;

static AnserControl *AnserCtl = NULL;
static HTAB *AnserChannelHash = NULL;
static char *AnserDataArena = NULL;
static bool *AnserArenaSlotInUse = NULL;

static Size AnserDataArenaSize(void);
static Size AnserArenaSlotMapSize(void);
static Size AnserChannelHashSize(void);
static void AnserInitializeControl(bool found);
static void AnserInitializeChannelHash(void);
static void AnserInitializeDataArena(void);
static void AnserInitializeArenaSlotMap(void);
static void AnserSweepOrphanChannels(void);
static bool AnserChannelOwnerIsAlive(const AnserChannelEntry *entry);
static bool AnserBuildChannelKey(int gp_session_id, int gp_command_count,
								 uint32 condition_id, const char *condition_key,
								 AnserChannelKey *channel_key);
static bool AnserAllocateArenaSlot(AnserChannelEntry *entry);
static void AnserReleaseArenaSlot(AnserChannelEntry *entry);
static bool AnserDeliverChannelData(const AnserChannelEntry *entry,
									void *buffer, Size buffer_size,
									Size *payload_len);
static bool AnserInitialized(void);

Size
AnserShmemSize(void)
{
	Size		size = 0;

	if (!gp_anser_enable)
		return 0;

	size = add_size(size, MAXALIGN(sizeof(AnserControl)));
	size = add_size(size, AnserChannelHashSize());
	size = add_size(size, AnserDataArenaSize());
	size = add_size(size, AnserArenaSlotMapSize());

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
	AnserInitializeDataArena();
	AnserInitializeArenaSlotMap();

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	AnserSweepOrphanChannels();
	LWLockRelease(AnserChannelLock);
}

bool
AnserRegisterCondition(int gp_session_id, int gp_command_count,
					   uint32 condition_id, const char *condition_key,
					   uint32 expected_producers,
					   AnserChannelKey *channel_key)
{
	AnserChannelEntry *entry;
	bool		found;
	TimestampTz now;

	if (!AnserInitialized())
		return false;

	if (expected_producers == 0)
	{
		ereport(WARNING,
				(errmsg("could not register Anser channel: expected producers must be greater than zero")));
		return false;
	}

	if (!AnserBuildChannelKey(gp_session_id, gp_command_count, condition_id,
						   condition_key, channel_key))
		return false;

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
				(errmsg("could not register Anser channel: channel map is full")));
		return false;
	}

	now = GetCurrentTimestamp();
	if (!found)
	{
		MemSet(entry, 0, sizeof(AnserChannelEntry));
		entry->key = *channel_key;
		entry->state = ANSER_CHANNEL_PENDING;
		entry->expected_producers = expected_producers;
		entry->arena_slot = ANSER_INVALID_ARENA_SLOT;
		entry->created_at = now;
	}
	else if (entry->state == ANSER_CHANNEL_CANCELLED ||
			 entry->state == ANSER_CHANNEL_CONSUMED)
	{
		AnserReleaseArenaSlot(entry);
		MemSet(entry, 0, sizeof(AnserChannelEntry));
		entry->key = *channel_key;
		entry->state = ANSER_CHANNEL_PENDING;
		entry->expected_producers = expected_producers;
		entry->arena_slot = ANSER_INVALID_ARENA_SLOT;
		entry->created_at = now;
	}
	else
		entry->expected_producers = expected_producers;

	entry->updated_at = now;
	LWLockRelease(AnserChannelLock);

	return true;
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
		entry->cancelled = true;
		entry->state = ANSER_CHANNEL_CANCELLED;
		entry->updated_at = GetCurrentTimestamp();
		SetLatch(&AnserCtl->send_latch);
		LWLockRelease(AnserChannelLock);
		return true;
	}

	if (payload_len > (Size) gp_anser_max_info_size)
	{
		entry->cancelled = true;
		entry->state = ANSER_CHANNEL_CANCELLED;
		entry->updated_at = GetCurrentTimestamp();
		SetLatch(&AnserCtl->send_latch);
		LWLockRelease(AnserChannelLock);
		return false;
	}

	if (entry->state == ANSER_CHANNEL_PENDING)
		entry->state = ANSER_CHANNEL_COLLECTING;

	if (entry->arena_slot == ANSER_INVALID_ARENA_SLOT &&
		!AnserAllocateArenaSlot(entry))
	{
		entry->cancelled = true;
		entry->state = ANSER_CHANNEL_CANCELLED;
		entry->updated_at = GetCurrentTimestamp();
		SetLatch(&AnserCtl->send_latch);
		LWLockRelease(AnserChannelLock);
		return false;
	}

	if (payload != NULL && payload_len > 0)
	{
		if (payload_len > (Size) gp_anser_max_info_size - entry->data_len)
		{
			entry->cancelled = true;
			entry->state = ANSER_CHANNEL_CANCELLED;
			entry->updated_at = GetCurrentTimestamp();
			SetLatch(&AnserCtl->send_latch);
			LWLockRelease(AnserChannelLock);
			return false;
		}

		memcpy(AnserDataArena + entry->data_offset + entry->data_len,
			   payload, payload_len);
		entry->data_len += payload_len;
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
AnserConsume(const AnserChannelKey *channel_key, void *buffer,
			 Size buffer_size, Size *payload_len, bool *cancelled,
			 long timeout_ms)
{
	TimestampTz start_time = GetCurrentTimestamp();

	if (payload_len != NULL)
		*payload_len = 0;
	if (cancelled != NULL)
		*cancelled = false;

	if (!AnserInitialized() || channel_key == NULL)
		return false;

	for (;;)
	{
		AnserChannelEntry *entry;
		bool		found;
		bool		ready;
		bool		is_cancelled;
		bool		delivered = false;

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
		is_cancelled = (entry->state == ANSER_CHANNEL_CANCELLED ||
						entry->cancelled);
		if (ready && !is_cancelled)
		{
			delivered = AnserDeliverChannelData(entry, buffer, buffer_size,
											   payload_len);
			if (!delivered)
			{
				LWLockRelease(AnserChannelLock);
				return false;
			}
		}
		LWLockRelease(AnserChannelLock);

		if (ready || is_cancelled)
		{
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

			is_cancelled = (entry->state == ANSER_CHANNEL_CANCELLED ||
							entry->cancelled);
			if (is_cancelled)
			{
				if (cancelled != NULL)
					*cancelled = true;
			}
			else if (entry->state != ANSER_CHANNEL_READY || !delivered)
			{
				LWLockRelease(AnserChannelLock);
				return false;
			}

			entry->done_consumers++;
			if (entry->consumers == 0 ||
				entry->done_consumers >= entry->consumers)
			{
				entry->state = is_cancelled ? ANSER_CHANNEL_CANCELLED :
					ANSER_CHANNEL_CONSUMED;
				AnserReleaseArenaSlot(entry);
			}
			entry->updated_at = GetCurrentTimestamp();
			LWLockRelease(AnserChannelLock);
			return ready && !is_cancelled;
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

		if (timeout_ms >= 0 &&
			TimestampDifferenceExceeds(start_time, GetCurrentTimestamp(),
									   timeout_ms))
		{
			entry->cancelled = true;
			entry->state = ANSER_CHANNEL_CANCELLED;
			entry->updated_at = GetCurrentTimestamp();
			if (cancelled != NULL)
				*cancelled = true;
			LWLockRelease(AnserChannelLock);
			return false;
		}

		LWLockRelease(AnserChannelLock);
		ResetLatch(MyLatch);
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10L,
						 PG_WAIT_EXTENSION);
	}
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

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	AnserSweepOrphanChannels();
	LWLockRelease(AnserChannelLock);
}

bool
AnserCancelChannel(const AnserChannelKey *channel_key)
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
	if (found)
	{
		entry->cancelled = true;
		entry->state = ANSER_CHANNEL_CANCELLED;
		entry->updated_at = GetCurrentTimestamp();
		AnserReleaseArenaSlot(entry);
		SetLatch(&AnserCtl->send_latch);
	}
	LWLockRelease(AnserChannelLock);

	return found;
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
			entry->cancelled = true;
			entry->state = ANSER_CHANNEL_CANCELLED;
			entry->updated_at = GetCurrentTimestamp();
			AnserReleaseArenaSlot(entry);
		}
	}
	SetLatch(&AnserCtl->send_latch);
	LWLockRelease(AnserChannelLock);
}

static Size
AnserDataArenaSize(void)
{
	Assert(gp_anser_max_channels >= 0);
	Assert(gp_anser_max_info_size >= 0);

	return mul_size((Size) gp_anser_max_channels,
					(Size) gp_anser_max_info_size);
}

static Size
AnserArenaSlotMapSize(void)
{
	Assert(gp_anser_max_channels >= 0);
	return MAXALIGN(sizeof(bool) * (Size) gp_anser_max_channels);
}

static Size
AnserChannelHashSize(void)
{
	Assert(gp_anser_max_channels >= 0);

	return hash_estimate_size(gp_anser_max_channels,
							  sizeof(AnserChannelEntry));
}

static void
AnserInitializeControl(bool found)
{
	if (!found)
	{
		MemSet(AnserCtl, 0, sizeof(AnserControl));
		AnserCtl->max_channels = gp_anser_max_channels;
		AnserCtl->max_info_size = gp_anser_max_info_size;
		AnserCtl->arena_size = AnserDataArenaSize();
		AnserCtl->arena_next = 0;
		InitSharedLatch(&AnserCtl->gather_latch);
		InitSharedLatch(&AnserCtl->send_latch);
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
									  gp_anser_max_channels,
									  gp_anser_max_channels,
									  &hctl,
									  HASH_ELEM | HASH_BLOBS);
}

static void
AnserInitializeDataArena(void)
{
	bool		found;

	AnserDataArena = (char *) ShmemInitStruct(ANSER_DATA_ARENA_NAME,
												AnserDataArenaSize(),
												&found);
	if (!found)
		MemSet(AnserDataArena, 0, AnserDataArenaSize());
}

static void
AnserInitializeArenaSlotMap(void)
{
	bool		found;

	AnserArenaSlotInUse = (bool *) ShmemInitStruct(ANSER_ARENA_SLOTS_NAME,
													AnserArenaSlotMapSize(),
													&found);
	if (!found)
		MemSet(AnserArenaSlotInUse, 0, AnserArenaSlotMapSize());
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
											  (Size) gp_anser_max_channels);

	hash_seq_init(&status, AnserChannelHash);
	while ((entry = (AnserChannelEntry *) hash_seq_search(&status)) != NULL)
	{
		bool		recycle = false;

		if (entry->state == ANSER_CHANNEL_CONSUMED ||
			entry->state == ANSER_CHANNEL_CANCELLED)
			recycle = true;
		else if (!AnserChannelOwnerIsAlive(entry))
			recycle = true;

		if (recycle)
		{
			AnserReleaseArenaSlot(entry);
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
 * Conservative placeholder for liveness checking.
 *
 * Correct ssid/ccnt owner validation is intentionally left for a follow-up;
 * PR 1 has explicit AnserCancelQuery() cleanup at query end/failure.
 */
static bool
AnserChannelOwnerIsAlive(const AnserChannelEntry *entry)
{
	Assert(entry != NULL);
	return true;
}

static bool
AnserBuildChannelKey(int gp_session_id, int gp_command_count,
					 uint32 condition_id, const char *condition_key,
					 AnserChannelKey *channel_key)
{
	if (channel_key == NULL || condition_key == NULL)
		return false;

	if (strlen(condition_key) >= ANSER_CONDITION_KEY_SIZE)
	{
		ereport(WARNING,
				(errmsg("could not build Anser channel key: condition key is too long")));
		return false;
	}

	MemSet(channel_key, 0, sizeof(AnserChannelKey));
	channel_key->gp_session_id = gp_session_id;
	channel_key->gp_command_count = gp_command_count;
	channel_key->condition_id = condition_id;
	strlcpy(channel_key->condition_key, condition_key,
			ANSER_CONDITION_KEY_SIZE);

	return true;
}

static bool
AnserAllocateArenaSlot(AnserChannelEntry *entry)
{
	uint32		slot;

	Assert(LWLockHeldByMeInMode(AnserChannelLock, LW_EXCLUSIVE));
	Assert(entry != NULL);

	if (entry->arena_slot != ANSER_INVALID_ARENA_SLOT)
		return true;

	for (slot = 0; slot < AnserCtl->max_channels; slot++)
	{
		if (!AnserArenaSlotInUse[slot])
		{
			AnserArenaSlotInUse[slot] = true;
			entry->arena_slot = slot;
			entry->data_offset = (Size) slot * AnserCtl->max_info_size;
			entry->data_len = 0;
			return true;
		}
	}

	AnserSweepOrphanChannels();
	for (slot = 0; slot < AnserCtl->max_channels; slot++)
	{
		if (!AnserArenaSlotInUse[slot])
		{
			AnserArenaSlotInUse[slot] = true;
			entry->arena_slot = slot;
			entry->data_offset = (Size) slot * AnserCtl->max_info_size;
			entry->data_len = 0;
			return true;
		}
	}

	return false;
}

static void
AnserReleaseArenaSlot(AnserChannelEntry *entry)
{
	Assert(LWLockHeldByMeInMode(AnserChannelLock, LW_EXCLUSIVE));

	if (entry == NULL || entry->arena_slot == ANSER_INVALID_ARENA_SLOT)
		return;

	Assert(entry->arena_slot < AnserCtl->max_channels);
	AnserArenaSlotInUse[entry->arena_slot] = false;
	entry->arena_slot = ANSER_INVALID_ARENA_SLOT;
	entry->data_offset = 0;
	entry->data_len = 0;
}

/*
 * Consume a ready channel payload.
 *
 * PR 1 stores opaque bytes and copies them out for the test module. Keep this
 * logic behind a dedicated helper because later PRs will need different
 * consumers: bloom-filter union/materialization, direct filter installation,
 * metadata-only consumption, and possibly payload formats that are not copied
 * into a caller-owned buffer at all.
 */
static bool
AnserDeliverChannelData(const AnserChannelEntry *entry, void *buffer,
						Size buffer_size, Size *payload_len)
{
	Size		copy_len;

	Assert(LWLockHeldByMe(AnserChannelLock));
	Assert(entry != NULL);

	if (entry->arena_slot == ANSER_INVALID_ARENA_SLOT)
	{
		if (payload_len != NULL)
			*payload_len = 0;
		return (entry->data_len == 0);
	}

	if (buffer == NULL && entry->data_len > 0)
		return false;

	if (buffer_size < entry->data_len)
		return false;

	copy_len = entry->data_len;
	if (copy_len > 0)
		memcpy(buffer, AnserDataArena + entry->data_offset, copy_len);

	if (payload_len != NULL)
		*payload_len = copy_len;

	return true;
}

static bool
AnserInitialized(void)
{
	return gp_anser_enable && AnserCtl != NULL && AnserChannelHash != NULL &&
		AnserDataArena != NULL && AnserArenaSlotInUse != NULL;
}
