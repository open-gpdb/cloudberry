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
 * anser_test.c
 *	  SQL-callable test helpers for the Anser subsystem.
 *
 * IDENTIFICATION
 *	  src/test/modules/anser/anser_test.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/anser.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "varatt.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(anser_test_register_condition);
PG_FUNCTION_INFO_V1(anser_test_subscribe);
PG_FUNCTION_INFO_V1(anser_test_publish);
PG_FUNCTION_INFO_V1(anser_test_consume);
PG_FUNCTION_INFO_V1(anser_test_state);
PG_FUNCTION_INFO_V1(anser_test_cancel_channel);
PG_FUNCTION_INFO_V1(anser_test_cancel_query);

static bool build_test_key(FunctionCallInfo fcinfo, AnserChannelKey *key);
static const char *state_to_string(AnserChannelState state);

Datum
anser_test_register_condition(PG_FUNCTION_ARGS)
{
	int32		gp_session_id = PG_GETARG_INT32(0);
	int32		gp_command_count = PG_GETARG_INT32(1);
	int32		condition_id_arg = PG_GETARG_INT32(2);
	char	   *condition_key = text_to_cstring(PG_GETARG_TEXT_PP(3));
	int32		expected_producers_arg = PG_GETARG_INT32(4);
	AnserChannelKey channel_key;

	if (condition_id_arg < 0 || expected_producers_arg <= 0)
		PG_RETURN_BOOL(false);

	if (strlen(condition_key) >= ANSER_CONDITION_KEY_SIZE)
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(AnserRegisterCondition(gp_session_id,
										gp_command_count,
										(uint32) condition_id_arg,
										condition_key,
										(uint32) expected_producers_arg,
										&channel_key));
}

Datum
anser_test_subscribe(PG_FUNCTION_ARGS)
{
	AnserChannelKey key;

	if (!build_test_key(fcinfo, &key))
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(AnserSubscribe(&key));
}

Datum
anser_test_publish(PG_FUNCTION_ARGS)
{
	AnserChannelKey key;
	bytea	   *payload = PG_GETARG_BYTEA_PP(4);
	bool		cancelled = PG_GETARG_BOOL(5);

	if (!build_test_key(fcinfo, &key))
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(AnserPublish(&key,
							  VARDATA_ANY(payload),
							  VARSIZE_ANY_EXHDR(payload),
							  cancelled));
}

Datum
anser_test_consume(PG_FUNCTION_ARGS)
{
	AnserChannelKey key;
	int32		timeout_arg = PG_GETARG_INT32(4);
	long		timeout_ms = timeout_arg;
	char	   *buffer;
	Size		payload_len = 0;
	bool		cancelled = false;
	bool		ready;
	bytea	   *result;

	if (timeout_arg < 0)
		PG_RETURN_NULL();

	if (!build_test_key(fcinfo, &key))
		PG_RETURN_NULL();

	buffer = (char *) palloc((Size) gp_anser_max_info_size);
	ready = AnserConsume(&key, buffer, (Size) gp_anser_max_info_size,
						 &payload_len, &cancelled, timeout_ms);
	if (!ready || cancelled)
		PG_RETURN_NULL();

	result = (bytea *) palloc(VARHDRSZ + payload_len);
	SET_VARSIZE(result, VARHDRSZ + payload_len);
	if (payload_len > 0)
		memcpy(VARDATA(result), buffer, payload_len);

	PG_RETURN_BYTEA_P(result);
}

Datum
anser_test_state(PG_FUNCTION_ARGS)
{
	AnserChannelKey key;
	bool		found = false;
	AnserChannelState state;

	if (!build_test_key(fcinfo, &key))
		PG_RETURN_TEXT_P(cstring_to_text("NOT_FOUND"));

	state = AnserChannelGetState(&key, &found);
	if (!found)
		PG_RETURN_TEXT_P(cstring_to_text("NOT_FOUND"));

	PG_RETURN_TEXT_P(cstring_to_text(state_to_string(state)));
}

Datum
anser_test_cancel_channel(PG_FUNCTION_ARGS)
{
	AnserChannelKey key;

	if (!build_test_key(fcinfo, &key))
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(AnserCancelChannel(&key));
}

Datum
anser_test_cancel_query(PG_FUNCTION_ARGS)
{
	int32		gp_session_id = PG_GETARG_INT32(0);
	int32		gp_command_count = PG_GETARG_INT32(1);

	AnserCancelQuery(gp_session_id, gp_command_count);
	PG_RETURN_VOID();
}

static bool
build_test_key(FunctionCallInfo fcinfo, AnserChannelKey *key)
{
	int32		gp_session_id = PG_GETARG_INT32(0);
	int32		gp_command_count = PG_GETARG_INT32(1);
	int32		condition_id_arg = PG_GETARG_INT32(2);
	char	   *condition_key = text_to_cstring(PG_GETARG_TEXT_PP(3));

	if (key == NULL || condition_id_arg < 0)
		return false;

	if (strlen(condition_key) >= ANSER_CONDITION_KEY_SIZE)
		return false;

	MemSet(key, 0, sizeof(AnserChannelKey));
	key->gp_session_id = gp_session_id;
	key->gp_command_count = gp_command_count;
	key->condition_id = (uint32) condition_id_arg;
	strlcpy(key->condition_key, condition_key, ANSER_CONDITION_KEY_SIZE);
	return true;
}

static const char *
state_to_string(AnserChannelState state)
{
	switch (state)
	{
		case ANSER_CHANNEL_PENDING:
			return "PENDING";
		case ANSER_CHANNEL_COLLECTING:
			return "COLLECTING";
		case ANSER_CHANNEL_READY:
			return "READY";
		case ANSER_CHANNEL_CANCELLED:
			return "CANCELLED";
		case ANSER_CHANNEL_CONSUMED:
			return "CONSUMED";
	}

	return "UNKNOWN";
}
