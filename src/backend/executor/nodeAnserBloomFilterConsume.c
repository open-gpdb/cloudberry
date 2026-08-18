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
#include "cdb/anserfilter.h"
#include "executor/nodeAnserBloomFilter.h"

struct AnserBloomFilterConsumeState
{
	AnserChannelKey channel_key;
	bloom_filter *filter;
	uint32		expected_parts;
	uint32		received_parts;
	bool		consumed;
	bool		cancelled;
};

AnserBloomFilterConsumeState *
ExecInitAnserBloomFilterConsume(const AnserChannelKey *channel_key,
								uint32 expected_parts)
{
	AnserBloomFilterConsumeState *state;

	if (channel_key == NULL || expected_parts == 0)
		return NULL;

	state = palloc0(sizeof(AnserBloomFilterConsumeState));
	state->channel_key = *channel_key;
	state->expected_parts = expected_parts;
	return state;
}

bool
ExecAnserBloomFilterConsume(AnserBloomFilterConsumeState *state,
							long registration_timeout_ms)
{
	void	   *payload;
	Size		payload_len = 0;
	bool		cancelled = false;
	bool		ready;

	if (state == NULL)
		return false;

	if (state->consumed)
		return state->filter != NULL;

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

	state->filter = AnserBloomUnionParts(payload, payload_len,
									   state->expected_parts,
									   &state->received_parts);
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
