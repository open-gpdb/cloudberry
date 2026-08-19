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

#include "libpq-fe.h"

#include "cdb/anser.h"
#include "cdb/anserclient.h"
#include "cdb/anserfilter.h"
#include "cdb/cdbvars.h"
#include "commands/dbcommands.h"
#include "executor/nodeAnserBloomFilter.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/latch.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/wait_event.h"
#include "varatt.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(anser_test_register_condition);
PG_FUNCTION_INFO_V1(anser_test_subscribe);
PG_FUNCTION_INFO_V1(anser_test_publish);
PG_FUNCTION_INFO_V1(anser_test_consume);
PG_FUNCTION_INFO_V1(anser_test_state);
PG_FUNCTION_INFO_V1(anser_test_cancel_channel);
PG_FUNCTION_INFO_V1(anser_test_cancel_query);
PG_FUNCTION_INFO_V1(anser_test_bloom_roundtrip);
PG_FUNCTION_INFO_V1(anser_test_bloom_union);
PG_FUNCTION_INFO_V1(anser_test_node_roundtrip);
PG_FUNCTION_INFO_V1(anser_test_client_roundtrip);
PG_FUNCTION_INFO_V1(anser_test_multi_consumer);

static bool build_test_key(FunctionCallInfo fcinfo, AnserChannelKey *key);
static const char *state_to_string(AnserChannelState state);
static char *anser_loopback_host(void);
static PGconn *anser_open_consumer(const AnserChannelKey *key);
static bool anser_wait_consumer_count(const AnserChannelKey *key, int target);
static void anser_cancel_conn(PGconn *conn);
static bool anser_drain_until_idle(PGconn *conn);
static bool anser_consumer_got_payload(PGconn *conn, const unsigned char *expected,
									   Size expected_len);
static bool anser_consumer_returned_row(PGconn *conn);

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

Datum
anser_test_bloom_roundtrip(PG_FUNCTION_ARGS)
{
	char	   *key = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int32		value_arg = PG_GETARG_INT32(1);
	Datum		value = Int32GetDatum(value_arg);
	uint64		seed = AnserBloomSeed(key);
	bloom_filter *filter;
	bloom_filter *roundtrip;
	char	   *payload;
	Size		payload_size;
	Size		payload_len = 0;
	uint32		part_index = 0;
	uint32		total_parts = 0;
	bool		lacks;

	filter = AnserBloomCreate(32, 1024 * 1024, seed);
	if (filter == NULL)
		PG_RETURN_BOOL(false);

	bloom_add_element(filter, (unsigned char *) &value, sizeof(Datum));
	payload_size = AnserBloomSerializedSize(filter);
	payload = palloc(payload_size);
	if (!AnserBloomSerializePart(filter, 0, 1, payload, payload_size,
								  &payload_len))
		PG_RETURN_BOOL(false);

	roundtrip = AnserBloomDeserializePart(payload, payload_len,
									   &part_index, &total_parts);
	if (roundtrip == NULL)
		PG_RETURN_BOOL(false);

	lacks = bloom_lacks_element(roundtrip, (unsigned char *) &value,
							 sizeof(Datum));
	bloom_free(filter);
	bloom_free(roundtrip);
	PG_RETURN_BOOL(!lacks && part_index == 0 && total_parts == 1);
}

Datum
anser_test_bloom_union(PG_FUNCTION_ARGS)
{
	char	   *key = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int32		left_arg = PG_GETARG_INT32(1);
	int32		right_arg = PG_GETARG_INT32(2);
	Datum		left_value = Int32GetDatum(left_arg);
	Datum		right_value = Int32GetDatum(right_arg);
	uint64		seed = AnserBloomSeed(key);
	bloom_filter *left;
	bloom_filter *right;
	bloom_filter *united;
	char	   *payload;
	Size		left_size;
	Size		right_size;
	Size		left_len = 0;
	Size		right_len = 0;
	uint32		received = 0;
	bool		ok;

	left = AnserBloomCreate(32, 1024 * 1024, seed);
	right = AnserBloomCreate(32, 1024 * 1024, seed);
	if (left == NULL || right == NULL)
		PG_RETURN_BOOL(false);

	bloom_add_element(left, (unsigned char *) &left_value, sizeof(Datum));
	bloom_add_element(right, (unsigned char *) &right_value, sizeof(Datum));
	left_size = AnserBloomSerializedSize(left);
	right_size = AnserBloomSerializedSize(right);
	payload = palloc(left_size + right_size);
	ok = AnserBloomSerializePart(left, 0, 2, payload, left_size, &left_len) &&
		AnserBloomSerializePart(right, 1, 2, payload + left_len, right_size,
								&right_len);
	if (!ok)
		PG_RETURN_BOOL(false);

	united = AnserBloomUnionParts(payload, left_len + right_len, 2, &received);
	if (united == NULL)
		PG_RETURN_BOOL(false);

	ok = received == 2 &&
		!bloom_lacks_element(united, (unsigned char *) &left_value,
							 sizeof(Datum)) &&
		!bloom_lacks_element(united, (unsigned char *) &right_value,
							 sizeof(Datum));
	bloom_free(left);
	bloom_free(right);
	bloom_free(united);
	PG_RETURN_BOOL(ok);
}

Datum
anser_test_node_roundtrip(PG_FUNCTION_ARGS)
{
	AnserChannelKey key;
	AnserBloomFilterProduceState *producer;
	AnserBloomFilterConsumeState *consumer;
	int32		value_arg = PG_GETARG_INT32(0);
	Datum		value = Int32GetDatum(value_arg);
	bool		ok;

	if (!AnserRegisterCondition(99, 1, 1, "node_roundtrip", 1, &key))
		PG_RETURN_BOOL(false);
	if (!AnserSubscribe(&key))
		PG_RETURN_BOOL(false);

	producer = ExecInitAnserBloomFilterProduce(&key, 32, 1024 * 1024, 0, 1);
	if (producer == NULL)
		PG_RETURN_BOOL(false);
	ExecAnserBloomFilterProduceAddDatum(producer, value, false);
	ok = ExecAnserBloomFilterProducePublish(producer);
	ExecEndAnserBloomFilterProduce(producer);
	if (!ok)
		PG_RETURN_BOOL(false);

	consumer = ExecInitAnserBloomFilterConsume(&key, 1);
	if (consumer == NULL)
		PG_RETURN_BOOL(false);
	ok = ExecAnserBloomFilterConsume(consumer, 1000) &&
		ExecAnserBloomFilterConsumerGetFilter(consumer) != NULL &&
		ExecAnserBloomFilterConsumerReceivedParts(consumer) == 1 &&
		!ExecAnserBloomFilterConsumerWasCancelled(consumer) &&
		!bloom_lacks_element(ExecAnserBloomFilterConsumerGetFilter(consumer),
							 (unsigned char *) &value, sizeof(Datum));
	ExecEndAnserBloomFilterConsume(consumer);
	PG_RETURN_BOOL(ok);
}

/*
 * Drive the libpq client helpers against our own coordinator (loopback), proving
 * the AnserClient* path end-to-end without a multi-node cluster.  The helpers
 * read the QD address from qdHostname/qdPostmasterPort, which are blank on the
 * coordinator itself, so we point them at the local postmaster for the duration
 * of the call and restore them afterward.
 */
Datum
anser_test_client_roundtrip(PG_FUNCTION_ARGS)
{
	int32		value_arg = PG_GETARG_INT32(0);
	char	   *saved_host = qdHostname;
	int			saved_port = qdPostmasterPort;
	AnserChannelKey key;
	unsigned char payload[sizeof(int32)];
	void	   *out = NULL;
	Size		out_len = 0;
	bool		cancelled = false;
	bool		ok = false;

	qdHostname = anser_loopback_host();
	qdPostmasterPort = PostPortNumber;

	MemSet(&key, 0, sizeof(key));
	key.gp_session_id = 20;
	key.gp_command_count = 1;
	key.condition_id = 1;
	strlcpy(key.condition_key, "client_loopback", ANSER_CONDITION_KEY_SIZE);

	memcpy(payload, &value_arg, sizeof(payload));

	PG_TRY();
	{
		if (AnserClientPublish(&key, 1, payload, sizeof(payload), false) &&
			AnserClientConsumeWait(&key, &out, &out_len, &cancelled) &&
			!cancelled &&
			out_len == sizeof(payload) &&
			memcmp(out, payload, out_len) == 0)
			ok = true;
	}
	PG_FINALLY();
	{
		qdHostname = saved_host;
		qdPostmasterPort = saved_port;
	}
	PG_END_TRY();

	if (out != NULL)
		pfree(out);

	PG_RETURN_BOOL(ok);
}

/*
 * Best loopback target for a libpq connection to our own postmaster: the first
 * configured Unix-socket directory when available (avoids TCP/hba surprises),
 * otherwise "localhost".
 */
static char *
anser_loopback_host(void)
{
	const char *sockdirs = GetConfigOption("unix_socket_directories", true, false);

	if (sockdirs != NULL && sockdirs[0] == '/')
	{
		const char *comma = strchr(sockdirs, ',');
		Size		len = comma != NULL ? (Size) (comma - sockdirs) : strlen(sockdirs);

		return pnstrdup(sockdirs, len);
	}

	return pstrdup("localhost");
}

/*
 * Multi-consumer partial delivery.
 *
 * Two consumers block concurrently on the same channel (real libpq loopback
 * connections to our own coordinator).  One is cancelled mid-wait, standing in
 * for a broken consumer connection; the other keeps waiting.  We then publish
 * the payload and assert that the survivor receives it intact while the
 * cancelled consumer got no data -- proving delivery is per-consumer, not
 * all-or-nothing across consumers.
 */
Datum
anser_test_multi_consumer(PG_FUNCTION_ARGS)
{
	int32		value_arg = PG_GETARG_INT32(0);
	char	   *saved_host = qdHostname;
	int			saved_port = qdPostmasterPort;
	AnserChannelKey key;
	unsigned char payload[sizeof(int32)];
	PGconn	   *keep = NULL;
	PGconn	   *lost = NULL;
	bool		ok = false;

	qdHostname = anser_loopback_host();
	qdPostmasterPort = PostPortNumber;

	MemSet(&key, 0, sizeof(key));
	key.gp_session_id = 31;
	key.gp_command_count = 1;
	key.condition_id = 1;
	strlcpy(key.condition_key, "multi_consumer", ANSER_CONDITION_KEY_SIZE);

	memcpy(payload, &value_arg, sizeof(payload));

	PG_TRY();
	{
		/* Producer announces the channel (one producer expected). */
		if (AnserProducerBegin(&key, 1, GetUserId(), superuser()))
		{
			keep = anser_open_consumer(&key);
			lost = anser_open_consumer(&key);

			/* Publish only once both consumers have registered wait slots. */
			if (keep != NULL && lost != NULL &&
				anser_wait_consumer_count(&key, 2))
			{
				bool		lost_failed;

				/*
				 * Break the "lost" consumer mid-wait and let its cancel fully
				 * resolve before publishing, so the send service can never race
				 * a delivery into it.
				 */
				anser_cancel_conn(lost);
				lost_failed = !anser_consumer_returned_row(lost);

				if (lost_failed &&
					AnserPublish(&key, payload, sizeof(payload), false))
					ok = anser_consumer_got_payload(keep, payload,
													sizeof(payload));
			}
		}
	}
	PG_FINALLY();
	{
		if (keep != NULL)
			PQfinish(keep);
		if (lost != NULL)
			PQfinish(lost);
		qdHostname = saved_host;
		qdPostmasterPort = saved_port;
	}
	PG_END_TRY();

	PG_RETURN_BOOL(ok);
}

/*
 * Open a loopback connection to our coordinator and fire gp_anser_consume_wait
 * asynchronously (binary result), leaving the connection blocked server-side.
 */
static PGconn *
anser_open_consumer(const AnserChannelKey *key)
{
	const char *keywords[5];
	const char *values[5];
	const char *params[4];
	char		portstr[12];
	char		ssid[12];
	char		ccnt[12];
	char		condid[12];
	PGconn	   *conn;
	int			n = 0;

	snprintf(portstr, sizeof(portstr), "%d", qdPostmasterPort);

	keywords[n] = "host";
	values[n] = qdHostname;
	n++;
	keywords[n] = "port";
	values[n] = portstr;
	n++;
	keywords[n] = "dbname";
	values[n] = get_database_name(MyDatabaseId);
	n++;
	keywords[n] = "user";
	values[n] = GetUserNameFromId(GetUserId(), false);
	n++;
	keywords[n] = NULL;
	values[n] = NULL;

	conn = PQconnectdbParams(keywords, values, false);
	if (conn == NULL)
		return NULL;
	if (PQstatus(conn) != CONNECTION_OK)
	{
		PQfinish(conn);
		return NULL;
	}

	snprintf(ssid, sizeof(ssid), "%d", key->gp_session_id);
	snprintf(ccnt, sizeof(ccnt), "%d", key->gp_command_count);
	snprintf(condid, sizeof(condid), "%d", (int) key->condition_id);
	params[0] = ssid;
	params[1] = ccnt;
	params[2] = condid;
	params[3] = key->condition_key;

	if (!PQsendQueryParams(conn,
						   "SELECT gp_anser_consume_wait($1::int4, $2::int4, $3::int4, $4::text)",
						   4, NULL, params, NULL, NULL, 1))
	{
		PQfinish(conn);
		return NULL;
	}

	return conn;
}

/*
 * Poll the shared channel map until at least `target` consumers have subscribed
 * (or a bounded timeout elapses).  We share the coordinator's shmem, so we read
 * the count directly rather than through the connections.
 */
static bool
anser_wait_consumer_count(const AnserChannelKey *key, int target)
{
	int			i;

	for (i = 0; i < 1000; i++)	/* up to ~10s */
	{
		CHECK_FOR_INTERRUPTS();
		if (AnserChannelConsumerCount(key) >= target)
			return true;
		pg_usleep(10000);		/* 10ms */
	}

	return false;
}

static void
anser_cancel_conn(PGconn *conn)
{
	PGcancel   *cancel = PQgetCancel(conn);

	if (cancel != NULL)
	{
		char		errbuf[256];

		(void) PQcancel(cancel, errbuf, sizeof(errbuf));
		PQfreeCancel(cancel);
	}
}

/*
 * Pump a connection until its outstanding query stops being busy (result ready)
 * or a bounded timeout elapses.  Returns false on connection loss/timeout.
 */
static bool
anser_drain_until_idle(PGconn *conn)
{
	int			i;

	for (i = 0; i < 1000; i++)	/* up to ~100s worst case; resolves in ms */
	{
		CHECK_FOR_INTERRUPTS();
		if (!PQconsumeInput(conn))
			return false;
		if (!PQisBusy(conn))
			return true;

		(void) WaitLatchOrSocket(MyLatch,
								 WL_LATCH_SET | WL_SOCKET_READABLE |
								 WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								 PQsocket(conn), 100L, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
	}

	return false;
}

/* True iff the consumer returned exactly the expected payload bytes. */
static bool
anser_consumer_got_payload(PGconn *conn, const unsigned char *expected,
						   Size expected_len)
{
	PGresult   *res;
	PGresult   *tmp;
	bool		ok = false;

	if (!anser_drain_until_idle(conn))
		return false;

	res = PQgetResult(conn);
	if (res != NULL && PQresultStatus(res) == PGRES_TUPLES_OK &&
		PQntuples(res) == 1 && !PQgetisnull(res, 0, 0) &&
		(Size) PQgetlength(res, 0, 0) == expected_len &&
		memcmp(PQgetvalue(res, 0, 0), expected, expected_len) == 0)
		ok = true;

	if (res != NULL)
		PQclear(res);
	while ((tmp = PQgetResult(conn)) != NULL)
		PQclear(tmp);

	return ok;
}

/* True iff the consumer returned a non-null data row (it should not have). */
static bool
anser_consumer_returned_row(PGconn *conn)
{
	PGresult   *res;
	bool		got_row = false;

	if (!anser_drain_until_idle(conn))
		return false;

	while ((res = PQgetResult(conn)) != NULL)
	{
		if (PQresultStatus(res) == PGRES_TUPLES_OK &&
			PQntuples(res) >= 1 && !PQgetisnull(res, 0, 0))
			got_row = true;
		PQclear(res);
	}

	return got_row;
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
