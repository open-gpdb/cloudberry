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
 * anserfuncs.c
 *	  Built-in SQL functions that expose the Anser network transport.
 *
 * These are the thin coordinator-side edges of the Anser data path.  Remote
 * (segment) producers and consumers reach the coordinator-resident channel map
 * by opening an ordinary libpq connection to the QD and calling these builtins;
 * all real work happens in the gather and send background services.  A producer
 * announces itself with gp_anser_producer_begin(), streams parts with
 * gp_anser_publish(), and a consumer blocks in gp_anser_consume_wait() until the
 * send service delivers its payload (or cancels it).
 *
 * Access control: these functions are intentionally granted to PUBLIC (their
 * pg_proc.dat entries carry no proacl, so they inherit the default EXECUTE grant
 * to PUBLIC).  The network transport connects to the QD as the *query's own*
 * role, so restricting the functions to superusers -- or revoking them from
 * PUBLIC -- would silently disable runtime filtering for every non-superuser
 * query (it would fail open to unfiltered execution).
 *
 * The (session id, command count, condition) key cannot be derived server-side:
 * each builtin runs in a fresh coordinator backend the segment opened over
 * libpq, with its own session -- not the originating query's -- so the key must
 * travel in the call.  Because it is caller-supplied, we bind every channel to
 * the authenticated role that created it (see AnserChannelEntry.creator_role):
 * a caller may only produce/consume on a channel its own role created, unless it
 * is a superuser.  That blocks the dangerous vector -- one role poisoning
 * another role's bloom filter, which could drop matching rows -- and leaves only
 * same-role/cross-command self-interference, which fails open, never to wrong
 * results.
 *
 * IDENTIFICATION
 *	  src/backend/cdb/anser/anserfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/anser.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "varatt.h"

static bool anser_builtin_build_key(int32 gp_session_id, int32 gp_command_count,
									int32 condition_id, text *condition_key_text,
									AnserChannelKey *key);

/*
 * gp_anser_producer_begin(ssid, ccnt, cond_id, cond_key, expected_producers)
 *
 * Register (idempotently) the channel and arm its produce deadline.  This is the
 * "a producer opened a connection" signal; if the channel does not become READY
 * within gp_anser_timeout_ms the gather service cancels the whole dataset.
 */
Datum
gp_anser_producer_begin(PG_FUNCTION_ARGS)
{
	int32		gp_session_id = PG_GETARG_INT32(0);
	int32		gp_command_count = PG_GETARG_INT32(1);
	int32		condition_id = PG_GETARG_INT32(2);
	text	   *condition_key = PG_GETARG_TEXT_PP(3);
	int32		expected_producers = PG_GETARG_INT32(4);
	AnserChannelKey key;

	if (expected_producers <= 0)
		PG_RETURN_BOOL(false);

	if (!anser_builtin_build_key(gp_session_id, gp_command_count, condition_id,
								 condition_key, &key))
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(AnserProducerBegin(&key, expected_producers,
									  GetUserId(), superuser()));
}

/*
 * gp_anser_publish(ssid, ccnt, cond_id, cond_key, payload, cancelled)
 *
 * Hand one part to the gather service and block for its ACK.  expected_producers
 * is not repeated here: gp_anser_producer_begin already stamped it on the
 * channel, so we pass 0 to leave it unchanged.
 */
Datum
gp_anser_publish(PG_FUNCTION_ARGS)
{
	int32		gp_session_id = PG_GETARG_INT32(0);
	int32		gp_command_count = PG_GETARG_INT32(1);
	int32		condition_id = PG_GETARG_INT32(2);
	text	   *condition_key = PG_GETARG_TEXT_PP(3);
	bytea	   *payload = PG_GETARG_BYTEA_PP(4);
	bool		cancelled = PG_GETARG_BOOL(5);
	AnserChannelKey key;

	if (!anser_builtin_build_key(gp_session_id, gp_command_count, condition_id,
								 condition_key, &key))
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(AnserProducerSubmit(&key, 0,
									   VARDATA_ANY(payload),
									   VARSIZE_ANY_EXHDR(payload),
									   cancelled,
									   GetUserId(), superuser()));
}

/*
 * gp_anser_consume_wait(ssid, ccnt, cond_id, cond_key) -> bytea
 *
 * Subscribe, register a wait slot, and block on the proc latch until the send
 * service delivers the payload or cancels this consumer.  Returns the payload
 * bytes on delivery, or NULL when the channel is cancelled/unreachable.  Blocks
 * the calling coordinator backend for the query's lifetime, per the "consumer
 * waits, does not process further" semantics.
 */
Datum
gp_anser_consume_wait(PG_FUNCTION_ARGS)
{
	int32		gp_session_id = PG_GETARG_INT32(0);
	int32		gp_command_count = PG_GETARG_INT32(1);
	int32		condition_id = PG_GETARG_INT32(2);
	text	   *condition_key = PG_GETARG_TEXT_PP(3);
	AnserChannelKey key;
	void	   *payload = NULL;
	Size		payload_len = 0;
	bool		cancelled = false;
	bytea	   *result;

	if (!anser_builtin_build_key(gp_session_id, gp_command_count, condition_id,
								 condition_key, &key))
		PG_RETURN_NULL();

	if (!AnserConsumerWait(&key, &payload, &payload_len, &cancelled,
						   GetUserId(), superuser()) ||
		cancelled)
	{
		if (payload != NULL)
			pfree(payload);
		PG_RETURN_NULL();
	}

	result = (bytea *) palloc(VARHDRSZ + payload_len);
	SET_VARSIZE(result, VARHDRSZ + payload_len);
	if (payload_len > 0)
		memcpy(VARDATA(result), payload, payload_len);
	if (payload != NULL)
		pfree(payload);

	PG_RETURN_BYTEA_P(result);
}

/*
 * anser_builtin_build_key(ssid, ccnt, cond_id, cond_key, key)
 *
 * Validate the caller-supplied channel key components and copy them into
 * *key.  Returns false (fail open) when a component is out of range.
 */
static bool
anser_builtin_build_key(int32 gp_session_id, int32 gp_command_count,
						int32 condition_id, text *condition_key_text,
						AnserChannelKey *key)
{
	char	   *condition_key = text_to_cstring(condition_key_text);
	bool		ok = true;

	if (condition_id < 0 || strlen(condition_key) >= ANSER_CONDITION_KEY_SIZE)
		ok = false;
	else
	{
		MemSet(key, 0, sizeof(AnserChannelKey));
		key->gp_session_id = gp_session_id;
		key->gp_command_count = gp_command_count;
		key->condition_id = (uint32) condition_id;
		strlcpy(key->condition_key, condition_key, ANSER_CONDITION_KEY_SIZE);
	}

	pfree(condition_key);
	return ok;
}
