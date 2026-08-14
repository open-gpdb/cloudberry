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
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/hsearch.h"
#include "utils/timestamp.h"

#define ANSER_CONTROL_NAME		"Anser Control"
#define ANSER_CHANNEL_HASH_NAME	"Anser Channel Hash"
#define ANSER_DATA_ARENA_NAME	"Anser Data Arena"

bool		gp_anser_enable = false;
int			gp_anser_max_channels = 128;
int			gp_anser_max_info_size = 1024 * 1024;
int			gp_anser_timeout_ms = 1000;

static AnserControl *AnserCtl = NULL;
static HTAB *AnserChannelHash = NULL;
static char *AnserDataArena = NULL;

static Size AnserDataArenaSize(void);
static Size AnserChannelHashSize(void);
static void AnserInitializeControl(bool found);
static void AnserInitializeChannelHash(void);
static void AnserInitializeDataArena(void);
static void AnserSweepOrphanChannels(void);
static bool AnserChannelOwnerIsAlive(const AnserChannelEntry *entry);

Size
AnserShmemSize(void)
{
	Size		size = 0;

	if (!gp_anser_enable)
		return 0;

	size = add_size(size, MAXALIGN(sizeof(AnserControl)));
	size = add_size(size, AnserChannelHashSize());
	size = add_size(size, AnserDataArenaSize());

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

	LWLockAcquire(AnserChannelLock, LW_EXCLUSIVE);
	AnserSweepOrphanChannels();
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

/*
 * Recycle terminal or orphaned channels before declaring registration failure.
 *
 * Chunk 1 only provides the shared-memory sweep skeleton. The public API chunk
 * will call this before returning a fail-open WARNING on channel allocation
 * exhaustion.
 */
static void
AnserSweepOrphanChannels(void)
{
	HASH_SEQ_STATUS status;
	AnserChannelEntry *entry;

	Assert(LWLockHeldByMeInMode(AnserChannelLock, LW_EXCLUSIVE));

	if (AnserChannelHash == NULL)
		return;

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
			(void) hash_search(AnserChannelHash,
							  &entry->key,
							  HASH_REMOVE,
							  NULL);
		}
	}
}

/*
 * Conservative placeholder for liveness checking.
 *
 * Correct ssid/ccnt owner validation is intentionally left for the public API
 * chunk, where query registration and cancellation semantics are introduced.
 */
static bool
AnserChannelOwnerIsAlive(const AnserChannelEntry *entry)
{
	Assert(entry != NULL);
	return true;
}
