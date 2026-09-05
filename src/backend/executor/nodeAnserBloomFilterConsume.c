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
 * nodeAnserBloomFilterConsume.c
 *	  Standalone Anser Bloom filter consumer executor helper.
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeAnserBloomFilterConsume.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/anser.h"
#include "cdb/anserclient.h"
#include "cdb/anserfilter.h"
#include "cdb/cdbvars.h"
#include "executor/nodeAnserBloomFilter.h"

/*
 * State for one Bloom filter consumer.  Consumes the merged payload for a
 * channel exactly once (consumed), either from the coordinator's shared
 * memory channel map or over libpq.  token authenticates the libpq
 * transport on segments and is NULL on the coordinator.  cancelled records
 * that the producer side aborted instead of delivering the payload.
 */
struct AnserBloomFilterConsumeState
{
	AnserChannelKey channel_key;
	bloom_filter *filter;
	char	   *token;		/* QD session token for the libpq transport, or NULL */
	int64		total_elems;	/* filter sizing, shared with the producer */
	Size		max_payload_bytes;
	uint64		seed;
	uint32		expected_parts;
	uint32		received_parts;
	bool		consumed;
	bool		cancelled;
};

static bool ExecAnserBloomFilterConsumeDirect(AnserBloomFilterConsumeState *state,
											  long registration_timeout_ms);
static bool ExecAnserBloomFilterConsumeClient(AnserBloomFilterConsumeState *state);

AnserBloomFilterConsumeState *
ExecInitAnserBloomFilterConsume(const AnserChannelKey *channel_key,
								int64 total_elems, Size max_payload_bytes,
								uint32 expected_parts,
								const char *token)
{
	AnserBloomFilterConsumeState *state;

	if (channel_key == NULL || expected_parts == 0)
		return NULL;

	state = palloc0(sizeof(AnserBloomFilterConsumeState));
	state->channel_key = *channel_key;
	state->total_elems = total_elems;
	state->max_payload_bytes = max_payload_bytes;
	state->seed = AnserBloomSeed(channel_key->condition_key);
	state->expected_parts = expected_parts;
	state->token = (token != NULL && token[0] != '\0') ? pstrdup(token) : NULL;
	return state;
}

bool
ExecAnserBloomFilterConsume(AnserBloomFilterConsumeState *state,
							long registration_timeout_ms)
{
	if (state == NULL)
		return false;

	if (state->consumed)
		return state->filter != NULL;

	/*
	 * Coordinator-local consumers read the channel map directly; segment
	 * executors block on the send service over libpq to the QD.  The signatures
	 * are identical -- only the transport differs.
	 */
	if (Gp_role == GP_ROLE_EXECUTE)
		return ExecAnserBloomFilterConsumeClient(state);

	return ExecAnserBloomFilterConsumeDirect(state, registration_timeout_ms);
}

/*
 * Direct shared-memory consume path (coordinator): wait for producer
 * registration, wait for READY, then copy the merged payload out of the
 * channel map.
 */
static bool
ExecAnserBloomFilterConsumeDirect(AnserBloomFilterConsumeState *state,
								  long registration_timeout_ms)
{
	void	   *payload;
	Size		payload_len = 0;
	bool		cancelled = false;
	bool		ready;

	if (!AnserWaitProducersRegistered(&state->channel_key,
								   registration_timeout_ms))
	{
		state->consumed = true;
		return false;
	}

	if (!AnserWaitReady(&state->channel_key, &cancelled))
	{
		state->cancelled = cancelled;
		state->consumed = true;
		return false;
	}

	payload = palloc((Size) gp_anser_max_info_size);
	ready = AnserConsumeReady(&state->channel_key,
						   payload,
						   (Size) gp_anser_max_info_size,
						   &payload_len,
						   &cancelled);
	if (!ready || cancelled)
	{
		pfree(payload);
		state->cancelled = cancelled;
		state->consumed = true;
		return false;
	}

	/*
	 * The coordinator has already unioned every segment's part into one merged
	 * part (see AnserStorePayloadDSM), so we deserialize a single chunk rather
	 * than unioning N.  The merged header's total_parts records how many parts
	 * were folded, which we surface as the received count.
	 */
	{
		uint32		part_index = 0;
		uint32		folded = 0;

		state->filter = AnserBloomDeserializePart(payload, payload_len,
												  state->total_elems,
												  state->max_payload_bytes,
												  state->seed,
												  &part_index, &folded);
		state->received_parts = (state->filter != NULL) ? folded : 0;
	}
	pfree(payload);
	state->consumed = true;
	return state->filter != NULL;
}

/*
 * Network consume path (segment).  Blocks in the coordinator backend via libpq
 * until the send service delivers the whole payload (or cancels this consumer);
 * there is no registration/ready polling here -- the wait is unbounded and
 * cancellation is the only backstop.
 */
static bool
ExecAnserBloomFilterConsumeClient(AnserBloomFilterConsumeState *state)
{
	void	   *payload = NULL;
	Size		payload_len = 0;
	bool		cancelled = false;

	if (!AnserClientConsumeWait(&state->channel_key, &payload, &payload_len,
								&cancelled, state->token) || cancelled)
	{
		if (payload != NULL)
			pfree(payload);
		state->cancelled = cancelled;
		state->consumed = true;
		return false;
	}

	/*
	 * The coordinator has already unioned every segment's part into one merged
	 * part (see AnserStorePayloadDSM), so we deserialize a single chunk rather
	 * than unioning N.  The merged header's total_parts records how many parts
	 * were folded, which we surface as the received count.
	 */
	{
		uint32		part_index = 0;
		uint32		folded = 0;

		state->filter = AnserBloomDeserializePart(payload, payload_len,
												  state->total_elems,
												  state->max_payload_bytes,
												  state->seed,
												  &part_index, &folded);
		state->received_parts = (state->filter != NULL) ? folded : 0;
	}
	if (payload != NULL)
		pfree(payload);
	state->consumed = true;
	return state->filter != NULL;
}

bloom_filter *
ExecAnserBloomFilterConsumerGetFilter(AnserBloomFilterConsumeState *state)
{
	return state != NULL ? state->filter : NULL;
}

uint32
ExecAnserBloomFilterConsumerReceivedParts(AnserBloomFilterConsumeState *state)
{
	return state != NULL ? state->received_parts : 0;
}

bool
ExecAnserBloomFilterConsumerWasCancelled(AnserBloomFilterConsumeState *state)
{
	return state != NULL && state->cancelled;
}

void
ExecEndAnserBloomFilterConsume(AnserBloomFilterConsumeState *state)
{
	if (state == NULL)
		return;

	if (state->filter != NULL)
		bloom_free(state->filter);
	pfree(state);
}
