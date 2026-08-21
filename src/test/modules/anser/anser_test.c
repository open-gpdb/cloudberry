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
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "commands/dbcommands.h"
#include "executor/nodeAnserBloomFilter.h"
#include "fmgr.h"
#include "lib/bloomfilter.h"
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
PG_FUNCTION_INFO_V1(anser_test_bloom_fold);
PG_FUNCTION_INFO_V1(anser_test_payload_combine);
PG_FUNCTION_INFO_V1(anser_test_bloom_rejects_tiny);
PG_FUNCTION_INFO_V1(anser_test_node_roundtrip);
PG_FUNCTION_INFO_V1(anser_test_client_roundtrip);
PG_FUNCTION_INFO_V1(anser_test_multi_consumer);
PG_FUNCTION_INFO_V1(anser_test_abandoned_consumer_recycles);
PG_FUNCTION_INFO_V1(anser_test_set_sweep);
PG_FUNCTION_INFO_V1(anser_test_sweep);
PG_FUNCTION_INFO_V1(anser_test_max_channels_stable_across_slices);

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
static bool anser_wait_channel_consumed(const AnserChannelKey *key);

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
										expected_producers_arg,
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

/*
 * Coordinator-side fold: two segment parts folded (one at a time) must yield a
 * single merged part -- sized like one part regardless of the fold count, with
 * total_parts recording how many were combined -- whose filter contains both
 * segments' elements.  This is the master-side union that lets each consumer
 * receive one chunk instead of N.
 */
Datum
anser_test_bloom_fold(PG_FUNCTION_ARGS)
{
	char	   *key = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int32		left_arg = PG_GETARG_INT32(1);
	int32		right_arg = PG_GETARG_INT32(2);
	Datum		left_value = Int32GetDatum(left_arg);
	Datum		right_value = Int32GetDatum(right_arg);
	uint64		seed = AnserBloomSeed(key);
	bloom_filter *left;
	bloom_filter *right;
	bloom_filter *merged_filter;
	char	   *left_payload;
	char	   *right_payload;
	void	   *merged1;
	void	   *merged2;
	Size		left_size;
	Size		right_size;
	Size		left_len = 0;
	Size		right_len = 0;
	Size		merged1_len = 0;
	Size		merged2_len = 0;
	uint32		part_index = 0;
	uint32		total_parts = 0;
	bool		ok;

	left = AnserBloomCreate(32, 1024 * 1024, seed);
	right = AnserBloomCreate(32, 1024 * 1024, seed);
	if (left == NULL || right == NULL)
		PG_RETURN_BOOL(false);

	bloom_add_element(left, (unsigned char *) &left_value, sizeof(Datum));
	bloom_add_element(right, (unsigned char *) &right_value, sizeof(Datum));
	left_size = AnserBloomSerializedSize(left);
	right_size = AnserBloomSerializedSize(right);
	left_payload = palloc(left_size);
	right_payload = palloc(right_size);
	ok = AnserBloomSerializePart(left, 0, 2, left_payload, left_size, &left_len) &&
		AnserBloomSerializePart(right, 1, 2, right_payload, right_size,
								&right_len);
	bloom_free(left);
	bloom_free(right);
	if (!ok)
		PG_RETURN_BOOL(false);

	merged1 = AnserBloomFoldPart(NULL, 0, left_payload, left_len, &merged1_len);
	if (merged1 == NULL)
		PG_RETURN_BOOL(false);
	merged2 = AnserBloomFoldPart(merged1, merged1_len, right_payload, right_len,
								 &merged2_len);
	pfree(merged1);
	if (merged2 == NULL)
		PG_RETURN_BOOL(false);

	merged_filter = AnserBloomDeserializePart(merged2, merged2_len,
											  &part_index, &total_parts);
	ok = merged_filter != NULL &&
		AnserBloomLooksLikePart(merged2, merged2_len) &&
		merged2_len == left_size &&		/* one chunk, size independent of count */
		part_index == 0 &&
		total_parts == 2 &&
		!bloom_lacks_element(merged_filter, (unsigned char *) &left_value,
							 sizeof(Datum)) &&
		!bloom_lacks_element(merged_filter, (unsigned char *) &right_value,
							 sizeof(Datum));
	if (merged_filter != NULL)
		bloom_free(merged_filter);
	PG_RETURN_BOOL(ok);
}

/*
 * AnserCombinePayload dispatch: an opaque payload is appended (concatenated),
 * while a serialized bloom part is folded (unioned) into a single merged part.
 * This is the pure combine policy extracted from AnserStorePayloadDSM, so it can
 * be exercised on plain byte buffers without shared memory.
 */
Datum
anser_test_payload_combine(PG_FUNCTION_ARGS)
{
	uint64		seed = AnserBloomSeed("combine_bloom");
	bloom_filter *left;
	bloom_filter *right;
	bloom_filter *merged;
	Datum		left_value = Int32GetDatum(11);
	Datum		right_value = Int32GetDatum(22);
	char	   *left_part;
	char	   *right_part;
	Size		left_size;
	Size		right_size;
	Size		left_len = 0;
	Size		right_len = 0;
	void	   *o1;
	void	   *o2;
	void	   *c1;
	void	   *c2;
	Size		o1_len = 0;
	Size		o2_len = 0;
	Size		c1_len = 0;
	Size		c2_len = 0;
	uint32		part_index = 0;
	uint32		total_parts = 0;
	bool		opaque_ok;
	bool		bloom_ok;

	/* Opaque path: first payload copied verbatim, second appended. */
	o1 = AnserCombinePayload(NULL, 0, "aa", 2, &o1_len);
	o2 = (o1 != NULL) ? AnserCombinePayload(o1, o1_len, "bb", 2, &o2_len) : NULL;
	opaque_ok = o1 != NULL && o1_len == 2 && memcmp(o1, "aa", 2) == 0 &&
		o2 != NULL && o2_len == 4 && memcmp(o2, "aabb", 4) == 0;

	/* Bloom path: two parts combine into one merged, single-part chunk. */
	left = AnserBloomCreate(32, 1024 * 1024, seed);
	right = AnserBloomCreate(32, 1024 * 1024, seed);
	if (left == NULL || right == NULL)
		PG_RETURN_BOOL(false);

	bloom_add_element(left, (unsigned char *) &left_value, sizeof(Datum));
	bloom_add_element(right, (unsigned char *) &right_value, sizeof(Datum));
	left_size = AnserBloomSerializedSize(left);
	right_size = AnserBloomSerializedSize(right);
	left_part = palloc(left_size);
	right_part = palloc(right_size);
	if (!AnserBloomSerializePart(left, 0, 2, left_part, left_size, &left_len) ||
		!AnserBloomSerializePart(right, 1, 2, right_part, right_size, &right_len))
	{
		bloom_free(left);
		bloom_free(right);
		PG_RETURN_BOOL(false);
	}
	bloom_free(left);
	bloom_free(right);

	c1 = AnserCombinePayload(NULL, 0, left_part, left_len, &c1_len);
	c2 = (c1 != NULL) ?
		AnserCombinePayload(c1, c1_len, right_part, right_len, &c2_len) : NULL;
	merged = (c2 != NULL) ?
		AnserBloomDeserializePart(c2, c2_len, &part_index, &total_parts) : NULL;

	bloom_ok = c1 != NULL &&
		c2 != NULL &&
		c2_len == left_size &&		/* one merged chunk, single-part size */
		merged != NULL &&
		part_index == 0 &&
		total_parts == 2 &&
		!bloom_lacks_element(merged, (unsigned char *) &left_value,
							 sizeof(Datum)) &&
		!bloom_lacks_element(merged, (unsigned char *) &right_value,
							 sizeof(Datum));
	if (merged != NULL)
		bloom_free(merged);

	PG_RETURN_BOOL(opaque_ok && bloom_ok);
}

/*
 * Security regression: a crafted bloom part with a tiny bitset (e.g. bits = 4,
 * so bitset_bytes == 0) must be rejected rather than producing a filter with no
 * bitset storage (which membership tests would index out of bounds).  This
 * payload path is reachable from the PUBLIC gp_anser_publish builtin, so all
 * three entry points must return NULL for such a header.  Returns true iff the
 * crafted header is safely rejected everywhere (no crash, no filter built).
 */
Datum
anser_test_bloom_rejects_tiny(PG_FUNCTION_ARGS)
{
	int32		bits_arg = PG_GETARG_INT32(0);
	AnserBloomPartHeader header;
	uint32		part_index = 0;
	uint32		total_parts = 0;
	uint32		received = 0;
	bloom_filter *from_part;
	bloom_filter *from_union;
	bloom_filter *from_params;
	bool		ok;

	MemSet(&header, 0, sizeof(header));
	header.magic = ANSER_BLOOM_PART_MAGIC;
	header.version = ANSER_BLOOM_PART_VERSION;
	header.k_hash_funcs = 3;
	header.seed = 0;
	header.bitset_bits = (uint64) bits_arg;	/* tiny -> bitset_bytes == 0 */
	header.part_index = 0;
	header.total_parts = 1;

	/* The header itself is the whole payload (bitset_bytes == 0). */
	from_part = AnserBloomDeserializePart(&header, sizeof(header),
										  &part_index, &total_parts);
	from_union = AnserBloomUnionParts(&header, sizeof(header), 1, &received);
	from_params = bloom_create_with_params(header.bitset_bits, 3, 0);

	ok = (from_part == NULL && from_union == NULL && from_params == NULL);

	if (from_part != NULL)
		bloom_free(from_part);
	if (from_union != NULL)
		bloom_free(from_union);
	if (from_params != NULL)
		bloom_free(from_params);

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
 * Regression guard for the abandoned-consumer recycle bug.
 *
 * Same shape as anser_test_multi_consumer, but the assertion is specifically
 * that the channel does NOT leave stale data behind: after one consumer is
 * cancelled mid-wait and the surviving consumer is delivered, the channel must
 * recycle to CONSUMED.  Before the fix, the cancelled consumer kept counting
 * toward the expected consumer total, so done_consumers never caught up and the
 * channel lingered in READY forever (and was never reclaimable) -- this helper
 * would then time out waiting for CONSUMED and return false.
 */
Datum
anser_test_abandoned_consumer_recycles(PG_FUNCTION_ARGS)
{
	int32		value_arg = PG_GETARG_INT32(0);
	char	   *saved_host = qdHostname;
	int			saved_port = qdPostmasterPort;
	AnserChannelKey key;
	unsigned char payload[sizeof(int32)];
	int			nseg = getgpsegmentCount();
	PGconn	  **keep;
	PGconn	   *lost = NULL;
	int			i;
	bool		ok = false;

	if (nseg < 1)
		nseg = 1;

	qdHostname = anser_loopback_host();
	qdPostmasterPort = PostPortNumber;

	MemSet(&key, 0, sizeof(key));
	key.gp_session_id = 32;
	key.gp_command_count = 1;
	key.condition_id = 1;
	strlcpy(key.condition_key, "abandon_recycle", ANSER_CONDITION_KEY_SIZE);

	memcpy(payload, &value_arg, sizeof(payload));

	/*
	 * A channel recycles to CONSUMED once expected_consumers (== segment count,
	 * one consumer per segment) have been delivered.  Open exactly that many
	 * surviving consumers plus one that abandons mid-wait: the abandoned one must
	 * neither receive data nor block the recycle once the survivors are served.
	 */
	keep = (PGconn **) palloc0(sizeof(PGconn *) * nseg);

	PG_TRY();
	{
		bool		all_open = true;

		if (AnserProducerBegin(&key, 1, GetUserId(), superuser()))
		{
			for (i = 0; i < nseg; i++)
			{
				keep[i] = anser_open_consumer(&key);
				if (keep[i] == NULL)
					all_open = false;
			}
			lost = anser_open_consumer(&key);

			if (all_open && lost != NULL &&
				anser_wait_consumer_count(&key, nseg + 1))
			{
				anser_cancel_conn(lost);
				(void) anser_consumer_returned_row(lost);

				if (AnserPublish(&key, payload, sizeof(payload), false))
				{
					bool		all_got = true;

					for (i = 0; i < nseg; i++)
					{
						if (!anser_consumer_got_payload(keep[i], payload,
														sizeof(payload)))
							all_got = false;
					}
					if (all_got)
						ok = anser_wait_channel_consumed(&key);
				}
			}
		}
	}
	PG_FINALLY();
	{
		for (i = 0; i < nseg; i++)
			if (keep[i] != NULL)
				PQfinish(keep[i]);
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

/*
 * Poll (bounded) until the channel recycles to CONSUMED, or has already been
 * reclaimed entirely.  Either outcome means it did not leave stale data behind;
 * a channel wedged in READY never reaches this and the poll times out.
 */
static bool
anser_wait_channel_consumed(const AnserChannelKey *key)
{
	int			i;

	for (i = 0; i < 1000; i++)	/* up to ~10s */
	{
		bool		found = false;
		AnserChannelState state = AnserChannelGetState(key, &found);

		if (!found || state == ANSER_CHANNEL_CONSUMED)
			return true;

		CHECK_FOR_INTERRUPTS();
		pg_usleep(10000);		/* 10ms */
	}

	return false;
}

/*
 * Pause or resume the background maintenance sweep.  With it paused, terminal
 * (CANCELLED/CONSUMED) channels stay in the map so tests can assert their state
 * without racing the gather/send services.
 */
Datum
anser_test_set_sweep(PG_FUNCTION_ARGS)
{
	bool		enabled = PG_GETARG_BOOL(0);

	AnserSetSweepEnabled(enabled);
	PG_RETURN_VOID();
}

/*
 * Guard regression for AnserMaxChannels()'s memoization.
 *
 * The channel map is sized once at postmaster start; AnserMaxChannels() caches
 * that result so a later per-session SET gp_max_slices cannot report a size that
 * disagrees with the shared memory actually allocated (which would let the Len
 * functions index past the arrays).  Read the effective size, change
 * gp_max_slices to a value that -- absent the cache -- would grow the auto-sized
 * map by orders of magnitude, read again, and assert it did not budge.  Runs
 * entirely inside this one backend so the two reads bracket the SET.
 */
Datum
anser_test_max_channels_stable_across_slices(PG_FUNCTION_ARGS)
{
	int			before = AnserMaxChannels();
	char		saved[32];
	bool		stable;

	/* Preserve the session value so the test leaves no residue behind. */
	snprintf(saved, sizeof(saved), "%d", gp_max_slices);

	/* MaxConnections * 1000000 would dwarf any real map if recomputed live. */
	SetConfigOption("gp_max_slices", "1000000", PGC_USERSET, PGC_S_SESSION);

	stable = (AnserMaxChannels() == before && before > 0);

	SetConfigOption("gp_max_slices", saved, PGC_USERSET, PGC_S_SESSION);

	PG_RETURN_BOOL(stable);
}

/*
 * Run one maintenance sweep synchronously in this backend (respects the enable
 * flag), so a test can prove that reclamation clears terminal channels.
 */
Datum
anser_test_sweep(PG_FUNCTION_ARGS)
{
	AnserServiceMaintenance();
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
