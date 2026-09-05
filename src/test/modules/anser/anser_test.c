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

/*
 * Bloom sizing used by the test helpers.  bloom_create floors every filter at
 * 1 MB, so these are the smallest filters we can build; producer and consumer
 * sides must pass the identical pair (that is the whole point of carrying the
 * parameters in the node rather than on the wire).
 */
#define ANSER_TEST_ELEMS		32
#define ANSER_TEST_MAX_PAYLOAD	(1024 * 1024)

PG_FUNCTION_INFO_V1(anser_test_register_condition);
PG_FUNCTION_INFO_V1(anser_test_subscribe);
PG_FUNCTION_INFO_V1(anser_test_publish);
PG_FUNCTION_INFO_V1(anser_test_publish_value);
PG_FUNCTION_INFO_V1(anser_test_consume);
PG_FUNCTION_INFO_V1(anser_test_consume_has);
PG_FUNCTION_INFO_V1(anser_test_state);
PG_FUNCTION_INFO_V1(anser_test_cancel_query);
PG_FUNCTION_INFO_V1(anser_test_bloom_roundtrip);
PG_FUNCTION_INFO_V1(anser_test_bloom_fold_inplace);
PG_FUNCTION_INFO_V1(anser_test_bloom_rejects_mismatch);
PG_FUNCTION_INFO_V1(anser_test_node_roundtrip);
PG_FUNCTION_INFO_V1(anser_test_client_roundtrip);
PG_FUNCTION_INFO_V1(anser_test_token_roundtrip);
PG_FUNCTION_INFO_V1(anser_test_multi_consumer);
PG_FUNCTION_INFO_V1(anser_test_abandoned_consumer_recycles);
PG_FUNCTION_INFO_V1(anser_test_dsm_free_on_success);
PG_FUNCTION_INFO_V1(anser_test_dsm_free_on_timeout);
PG_FUNCTION_INFO_V1(anser_test_dsm_free_on_cancel);
PG_FUNCTION_INFO_V1(anser_test_set_sweep);
PG_FUNCTION_INFO_V1(anser_test_sweep);
PG_FUNCTION_INFO_V1(anser_test_max_channels_stable_across_slices);

static bool build_test_key(FunctionCallInfo fcinfo, AnserChannelKey *key);
static char *anser_make_test_part(const char *condition_key, int32 value,
								  Size *len_out);
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
	AnserChannelKey key;

	if (condition_id_arg < 0 || expected_producers_arg <= 0)
		PG_RETURN_BOOL(false);

	if (strlen(condition_key) >= ANSER_CONDITION_KEY_SIZE)
		PG_RETURN_BOOL(false);

	/*
	 * Drive the production registration entry point (AnserProducerBegin) rather
	 * than a test-only variant, so the state machine we exercise below is the
	 * one real producers use.
	 */
	MemSet(&key, 0, sizeof(key));
	key.gp_session_id = gp_session_id;
	key.gp_command_count = gp_command_count;
	key.condition_id = (uint32) condition_id_arg;
	strlcpy(key.condition_key, condition_key, ANSER_CONDITION_KEY_SIZE);

	PG_RETURN_BOOL(AnserProducerBegin(&key, expected_producers_arg,
									  GetUserId(), superuser()));
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

/*
 * Publish a real serialized bloom part carrying a single int value.  Multiple
 * producers on one channel each call this; the coordinator stores the first part
 * and OR-folds the rest (all same size), so the merged filter contains every
 * published value.  Needed by the state-machine test that drives >1 producer:
 * the coordinator only combines serialized bloom parts, which a raw SQL bytea
 * literal cannot express.
 */
Datum
anser_test_publish_value(PG_FUNCTION_ARGS)
{
	AnserChannelKey key;
	int32		value = PG_GETARG_INT32(4);
	char	   *part;
	Size		len = 0;
	bool		ok;

	if (!build_test_key(fcinfo, &key))
		PG_RETURN_BOOL(false);

	part = anser_make_test_part(key.condition_key, value, &len);
	if (part == NULL)
		PG_RETURN_BOOL(false);

	ok = AnserPublish(&key, part, len, false);
	pfree(part);
	PG_RETURN_BOOL(ok);
}

Datum
anser_test_consume(PG_FUNCTION_ARGS)
{
	AnserChannelKey key;
	int32		timeout_arg = PG_GETARG_INT32(4);
	char	   *buffer;
	Size		payload_len = 0;
	bool		cancelled = false;
	bytea	   *result;

	if (timeout_arg < 0)
		PG_RETURN_NULL();

	if (!build_test_key(fcinfo, &key))
		PG_RETURN_NULL();

	/*
	 * Consume through the production path -- AnserWaitReady + AnserConsumeReady,
	 * the same pair the executor's bloom consumer uses.  The timeout argument is
	 * advisory here: a channel that never becomes READY is cancelled by the
	 * gather service's stale-channel sweep after gp_anser_timeout_ms, which wakes
	 * this wait with cancelled = true (so a "timeout" returns NULL).
	 */
	if (!AnserWaitReady(&key, &cancelled) || cancelled)
		PG_RETURN_NULL();

	buffer = (char *) palloc((Size) gp_anser_max_info_size);
	if (!AnserConsumeReady(&key, buffer, (Size) gp_anser_max_info_size,
						   &payload_len, &cancelled) || cancelled)
		PG_RETURN_NULL();

	result = (bytea *) palloc(VARHDRSZ + payload_len);
	SET_VARSIZE(result, VARHDRSZ + payload_len);
	if (payload_len > 0)
		memcpy(VARDATA(result), buffer, payload_len);

	PG_RETURN_BYTEA_P(result);
}

/*
 * Consume the merged bloom payload and test membership of a single value.  Like
 * anser_test_consume, but rebuilds the filter from the shared (ANSER_TEST_ELEMS,
 * ANSER_TEST_MAX_PAYLOAD, key-derived seed) parameters -- exactly how the real
 * consumer node reconstructs it, with the parameters carried by the node rather
 * than the wire.  Returns true iff the value is present in the received filter.
 */
Datum
anser_test_consume_has(PG_FUNCTION_ARGS)
{
	AnserChannelKey key;
	int32		value = PG_GETARG_INT32(4);
	Datum		d = Int32GetDatum(value);
	char	   *buffer;
	Size		payload_len = 0;
	bool		cancelled = false;
	bloom_filter *filter;
	bool		has;

	if (!build_test_key(fcinfo, &key))
		PG_RETURN_BOOL(false);

	if (!AnserWaitReady(&key, &cancelled) || cancelled)
		PG_RETURN_BOOL(false);

	buffer = (char *) palloc((Size) gp_anser_max_info_size);
	if (!AnserConsumeReady(&key, buffer, (Size) gp_anser_max_info_size,
						   &payload_len, &cancelled) || cancelled)
	{
		pfree(buffer);
		PG_RETURN_BOOL(false);
	}

	filter = AnserBloomDeserializePart(buffer, payload_len,
									   ANSER_TEST_ELEMS, ANSER_TEST_MAX_PAYLOAD,
									   AnserBloomSeed(key.condition_key),
									   NULL, NULL);
	pfree(buffer);
	if (filter == NULL)
		PG_RETURN_BOOL(false);

	has = !bloom_lacks_element(filter, (unsigned char *) &d, sizeof(Datum));
	bloom_free(filter);
	PG_RETURN_BOOL(has);
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
									   32, 1024 * 1024, seed,
									   &part_index, &total_parts);
	if (roundtrip == NULL)
		PG_RETURN_BOOL(false);

	lacks = bloom_lacks_element(roundtrip, (unsigned char *) &value,
							 sizeof(Datum));
	bloom_free(filter);
	bloom_free(roundtrip);
	PG_RETURN_BOOL(!lacks && part_index == 0 && total_parts == 1);
}

/*
 * In-place fold: folding an equally-sized part into a merged part is a bitwise
 * OR of the bitset plus a fold-count bump, mutating the buffer without realloc.
 * This is the coordinator's only combine path: the first part is stored
 * verbatim, every later part folds in here.  A differently-sized part is
 * rejected and leaves the accumulator untouched.
 */
Datum
anser_test_bloom_fold_inplace(PG_FUNCTION_ARGS)
{
	uint64		seed = AnserBloomSeed("inplace_bloom");
	bloom_filter *left;
	bloom_filter *right;
	bloom_filter *big;
	bloom_filter *merged;
	Datum		left_value = Int32GetDatum(7);
	Datum		right_value = Int32GetDatum(9);
	char	   *acc;
	char	   *part;
	char	   *big_part;
	Size		acc_size;
	Size		part_size;
	Size		big_size;
	Size		acc_len = 0;
	Size		part_len = 0;
	Size		big_len = 0;
	uint32		part_index = 0;
	uint32		total_parts = 0;
	uint32		tp_before = 0;
	uint32		tp_after = 0;
	bool		same_ok;
	bool		mismatch_rejected;

	/* Two same-parameter parts: acc is the running merged part, part folds in. */
	left = AnserBloomCreate(32, 1024 * 1024, seed);
	right = AnserBloomCreate(32, 1024 * 1024, seed);
	if (left == NULL || right == NULL)
		PG_RETURN_BOOL(false);
	bloom_add_element(left, (unsigned char *) &left_value, sizeof(Datum));
	bloom_add_element(right, (unsigned char *) &right_value, sizeof(Datum));
	acc_size = AnserBloomSerializedSize(left);
	part_size = AnserBloomSerializedSize(right);
	acc = palloc(acc_size);
	part = palloc(part_size);
	if (!AnserBloomSerializePart(left, 0, 1, acc, acc_size, &acc_len) ||
		!AnserBloomSerializePart(right, 0, 1, part, part_size, &part_len))
	{
		bloom_free(left);
		bloom_free(right);
		PG_RETURN_BOOL(false);
	}
	bloom_free(left);
	bloom_free(right);

	same_ok = AnserBloomFoldPartInPlace(acc, acc_len, part, part_len);
	merged = same_ok ?
		AnserBloomDeserializePart(acc, acc_len, 32, 1024 * 1024, seed,
								  &part_index, &total_parts) : NULL;
	same_ok = same_ok &&
		acc_len == acc_size &&			/* size unchanged, folded in place */
		merged != NULL &&
		part_index == 0 &&
		total_parts == 2 &&				/* one more part folded */
		!bloom_lacks_element(merged, (unsigned char *) &left_value,
							 sizeof(Datum)) &&
		!bloom_lacks_element(merged, (unsigned char *) &right_value,
							 sizeof(Datum));
	if (merged != NULL)
		bloom_free(merged);

	/*
	 * A differently-sized part must be rejected and leave acc untouched.  Since
	 * bloom_create floors every filter at 1 MB, we need a genuinely larger
	 * cardinality/budget to get a bigger (2 MB) bitset than the 1 MB acc.
	 */
	big = AnserBloomCreate(1500000, 4 * 1024 * 1024, seed);
	if (big == NULL)
		PG_RETURN_BOOL(false);
	big_size = AnserBloomSerializedSize(big);
	big_part = palloc(big_size);
	if (!AnserBloomSerializePart(big, 0, 1, big_part, big_size, &big_len))
	{
		bloom_free(big);
		PG_RETURN_BOOL(false);
	}
	bloom_free(big);

	tp_before = ((const AnserBloomPartHeader *) acc)->total_parts;
	mismatch_rejected = big_len != acc_len &&
		!AnserBloomFoldPartInPlace(acc, acc_len, big_part, big_len);
	tp_after = ((const AnserBloomPartHeader *) acc)->total_parts;
	mismatch_rejected = mismatch_rejected && tp_before == tp_after;

	PG_RETURN_BOOL(same_ok && mismatch_rejected);
}

/*
 * Safety regression for the size/format check in AnserBloomDeserializePart.
 *
 * The consumer rebuilds the filter from its OWN (total_elems, max_payload, seed)
 * parameters, then requires the received bitset to be exactly the size those
 * parameters imply and the wire header to carry the expected magic.  A
 * well-formed part must load; a truncated one, an oversized one, and one with a
 * corrupted magic must all be rejected (NULL) so the consumer fails open rather
 * than loading a wrongly-shaped bitset.  Returns true iff the good part loads and
 * every bad one is rejected.
 */
Datum
anser_test_bloom_rejects_mismatch(PG_FUNCTION_ARGS)
{
	uint64		seed = AnserBloomSeed("reject_mismatch");
	bloom_filter *filter;
	char	   *good;
	Size		good_size;
	Size		good_len = 0;
	bloom_filter *ok_load;
	bloom_filter *short_load;
	bloom_filter *long_load;
	bloom_filter *magic_load;
	AnserBloomPartHeader *hdr;
	uint32		saved_magic;
	bool		ok;

	filter = AnserBloomCreate(ANSER_TEST_ELEMS, ANSER_TEST_MAX_PAYLOAD, seed);
	if (filter == NULL)
		PG_RETURN_BOOL(false);

	good_size = AnserBloomSerializedSize(filter);
	good = palloc(good_size);
	if (!AnserBloomSerializePart(filter, 0, 1, good, good_size, &good_len))
	{
		bloom_free(filter);
		PG_RETURN_BOOL(false);
	}
	bloom_free(filter);

	/* Well-formed: loads. */
	ok_load = AnserBloomDeserializePart(good, good_len, ANSER_TEST_ELEMS,
										ANSER_TEST_MAX_PAYLOAD, seed, NULL, NULL);

	/* One byte short of the expected bitset: rejected. */
	short_load = AnserBloomDeserializePart(good, good_len - 1, ANSER_TEST_ELEMS,
										   ANSER_TEST_MAX_PAYLOAD, seed, NULL, NULL);

	/* Claiming more bytes than the expected bitset: rejected. */
	long_load = AnserBloomDeserializePart(good, good_len + 1, ANSER_TEST_ELEMS,
										  ANSER_TEST_MAX_PAYLOAD, seed, NULL, NULL);

	/* Corrupted wire magic: rejected before the size check. */
	hdr = (AnserBloomPartHeader *) good;
	saved_magic = hdr->magic;
	hdr->magic = saved_magic ^ 0xFFFFFFFFU;
	magic_load = AnserBloomDeserializePart(good, good_len, ANSER_TEST_ELEMS,
										   ANSER_TEST_MAX_PAYLOAD, seed, NULL, NULL);
	hdr->magic = saved_magic;

	ok = ok_load != NULL && short_load == NULL && long_load == NULL &&
		magic_load == NULL;

	if (ok_load != NULL)
		bloom_free(ok_load);
	if (short_load != NULL)
		bloom_free(short_load);
	if (long_load != NULL)
		bloom_free(long_load);
	if (magic_load != NULL)
		bloom_free(magic_load);
	pfree(good);

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

	MemSet(&key, 0, sizeof(key));
	key.gp_session_id = 99;
	key.gp_command_count = 1;
	key.condition_id = 1;
	strlcpy(key.condition_key, "node_roundtrip", ANSER_CONDITION_KEY_SIZE);
	if (!AnserProducerBegin(&key, 1, GetUserId(), superuser()))
		PG_RETURN_BOOL(false);
	if (!AnserSubscribe(&key))
		PG_RETURN_BOOL(false);

	producer = ExecInitAnserBloomFilterProduce(&key, 32, 1024 * 1024, 0, 1,
											   NULL);
	if (producer == NULL)
		PG_RETURN_BOOL(false);
	ExecAnserBloomFilterProduceAddDatum(producer, value, false);
	ok = ExecAnserBloomFilterProducePublish(producer);
	ExecEndAnserBloomFilterProduce(producer);
	if (!ok)
		PG_RETURN_BOOL(false);

	consumer = ExecInitAnserBloomFilterConsume(&key, 32, 1024 * 1024, 1,
											   NULL);
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
		if (AnserClientPublish(&key, 1, payload, sizeof(payload), false, NULL) &&
			AnserClientConsumeWait(&key, &out, &out_len, &cancelled, NULL) &&
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
 * Session-token round trip: register this session's token, prove it validates
 * for this session user, that a bogus token and a bogus user are rejected, and
 * that a second call returns the same token (one token per session).
 */
Datum
anser_test_token_roundtrip(PG_FUNCTION_ARGS)
{
	Oid			user = GetSessionUserId();
	char	   *token = AnserGetOrCreateSessionToken(user);
	char	   *again;
	bool		ok;

	if (token == NULL)
		PG_RETURN_BOOL(false);

	ok = AnserSessionTokenIsValid(user, token) &&
		!AnserSessionTokenIsValid(user, "00000000000000000000000000000000") &&
		!AnserSessionTokenIsValid(InvalidOid, token);

	again = AnserGetOrCreateSessionToken(user);
	ok = ok && again != NULL && strcmp(again, token) == 0;

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
 * Regression guard: an abandoned consumer must not block channel recycling.
 *
 * Same shape as anser_test_multi_consumer, but the assertion is specifically
 * that the channel does NOT leave stale data behind: after one consumer is
 * cancelled mid-wait and the surviving consumers are delivered, the channel
 * must recycle to CONSUMED.  The cancelled consumer must not count toward the
 * expected consumer total, or done_consumers would never catch up and the
 * channel would wedge in READY forever (never reclaimable); this helper would
 * then time out waiting for CONSUMED and return false.
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
 * Payload-DSM lifetime, scenario (1): 5 producers, N (= segment count) consumers,
 * successful delivery.  The shared payload DSM must survive past the last consume
 * (the recycle to CONSUMED does not free it) and be released only when the sweep
 * reclaims the drained channel.  We keep the sweep paused to observe the deferred
 * state, then sweep explicitly.
 */
Datum
anser_test_dsm_free_on_success(PG_FUNCTION_ARGS)
{
	char	   *saved_host = qdHostname;
	int			saved_port = qdPostmasterPort;
	AnserChannelKey key;
	char	   *part;
	Size		part_len = 0;
	int			nseg = getgpsegmentCount();
	PGconn	  **cons;
	int			i;
	bool		all_read = true;
	bool		present_after_consume = false;
	bool		gone_after_sweep = false;

	if (nseg < 1)
		nseg = 1;

	AnserSetSweepEnabled(false);	/* observe the deferred free ourselves */
	qdHostname = anser_loopback_host();
	qdPostmasterPort = PostPortNumber;

	MemSet(&key, 0, sizeof(key));
	key.gp_session_id = 40;
	key.gp_command_count = 1;
	key.condition_id = 1;
	strlcpy(key.condition_key, "dsm_success", ANSER_CONDITION_KEY_SIZE);
	part = anser_make_test_part(key.condition_key, 7, &part_len);

	cons = (PGconn **) palloc0(sizeof(PGconn *) * nseg);

	PG_TRY();
	{
		bool		ready = false;

		/* 5 producers publish until the channel is READY (payload allocated). */
		if (part != NULL && AnserProducerBegin(&key, 5, GetUserId(), superuser()))
		{
			int			p;

			ready = true;
			for (p = 0; p < 5; p++)
				if (!AnserPublish(&key, part, part_len, false))
					ready = false;
		}

		if (ready)
		{
			bool		all_open = true;

			for (i = 0; i < nseg; i++)
			{
				cons[i] = anser_open_consumer(&key);
				if (cons[i] == NULL)
					all_open = false;
			}

			if (all_open && anser_wait_consumer_count(&key, nseg))
			{
				/* Every consumer receives and copies out the shared payload. */
				for (i = 0; i < nseg; i++)
					if (!anser_consumer_returned_row(cons[i]))
						all_read = false;

				/* Consumed, but the payload DSM is still pinned (freed by sweep). */
				present_after_consume = AnserChannelPayloadBytes(&key) > 0;

				AnserSetSweepEnabled(true);
				AnserServiceMaintenance();
				AnserSetSweepEnabled(false);

				gone_after_sweep = AnserChannelPayloadBytes(&key) < 0;
			}
		}
	}
	PG_FINALLY();
	{
		for (i = 0; i < nseg; i++)
			if (cons[i] != NULL)
				PQfinish(cons[i]);
		qdHostname = saved_host;
		qdPostmasterPort = saved_port;
	}
	PG_END_TRY();

	PG_RETURN_BOOL(all_read && present_after_consume && gone_after_sweep);
}

/*
 * Payload-DSM lifetime, scenario (2): only 3 of 5 producers publish, so the
 * channel never reaches READY.  It stays COLLECTING with a partial payload until
 * the produce deadline elapses, at which point the gather maintenance cancels it
 * and frees the payload.  (Consumers are omitted: a COLLECTING channel is never
 * delivered, so nothing borrows the payload -- the free needs no consumers.)
 */
Datum
anser_test_dsm_free_on_timeout(PG_FUNCTION_ARGS)
{
	AnserChannelKey key;
	char	   *part;
	Size		part_len = 0;
	char		saved_timeout[32];
	bool		present_collecting = false;
	bool		freed_after_timeout = false;

	AnserSetSweepEnabled(false);
	snprintf(saved_timeout, sizeof(saved_timeout), "%d", gp_anser_timeout_ms);
	SetConfigOption("gp_anser_timeout_ms", "100", PGC_USERSET, PGC_S_SESSION);

	MemSet(&key, 0, sizeof(key));
	key.gp_session_id = 41;
	key.gp_command_count = 1;
	key.condition_id = 1;
	strlcpy(key.condition_key, "dsm_timeout", ANSER_CONDITION_KEY_SIZE);
	part = anser_make_test_part(key.condition_key, 5, &part_len);

	PG_TRY();
	{
		int			p;

		if (part != NULL && AnserProducerBegin(&key, 5, GetUserId(), superuser()))
		{
			for (p = 0; p < 3; p++)
				(void) AnserPublish(&key, part, part_len, false);

			present_collecting = AnserChannelPayloadBytes(&key) > 0;

			/*
			 * Past the produce deadline the gather maintenance cancels the
			 * still-COLLECTING channel and frees its partial payload.  Drive one
			 * gather cycle after the timeout so this is deterministic.
			 */
			pg_usleep(200000L);		/* 200 ms > gp_anser_timeout_ms (100 ms) */
			AnserGatherServiceCycle();

			freed_after_timeout = AnserChannelPayloadBytes(&key) <= 0;
		}
	}
	PG_FINALLY();
	{
		SetConfigOption("gp_anser_timeout_ms", saved_timeout,
						PGC_USERSET, PGC_S_SESSION);
		AnserSetSweepEnabled(true);
		AnserServiceMaintenance();	/* reclaim the cancelled entry */
		AnserSetSweepEnabled(false);
	}
	PG_END_TRY();

	PG_RETURN_BOOL(present_collecting && freed_after_timeout);
}

/*
 * Payload-DSM lifetime, scenario (3): 5 producers, N consumers, then the query is
 * cancelled while consumers are attached.  The cancel must NOT free the payload
 * DSM (a consumer may still be borrowing it); it is released only after every
 * consumer slot has drained and the sweep reclaims the channel.
 */
Datum
anser_test_dsm_free_on_cancel(PG_FUNCTION_ARGS)
{
	char	   *saved_host = qdHostname;
	int			saved_port = qdPostmasterPort;
	AnserChannelKey key;
	char	   *part;
	Size		part_len = 0;
	int			nseg = getgpsegmentCount();
	PGconn	  **cons;
	int			i;
	bool		present_after_cancel = false;
	bool		gone_after_sweep = false;

	if (nseg < 1)
		nseg = 1;

	AnserSetSweepEnabled(false);
	qdHostname = anser_loopback_host();
	qdPostmasterPort = PostPortNumber;

	MemSet(&key, 0, sizeof(key));
	key.gp_session_id = 42;
	key.gp_command_count = 1;
	key.condition_id = 1;
	strlcpy(key.condition_key, "dsm_cancel", ANSER_CONDITION_KEY_SIZE);
	part = anser_make_test_part(key.condition_key, 9, &part_len);

	cons = (PGconn **) palloc0(sizeof(PGconn *) * nseg);

	PG_TRY();
	{
		bool		ready = false;

		if (part != NULL && AnserProducerBegin(&key, 5, GetUserId(), superuser()))
		{
			int			p;

			ready = true;
			for (p = 0; p < 5; p++)
				if (!AnserPublish(&key, part, part_len, false))
					ready = false;
		}

		if (ready)
		{
			bool		all_open = true;

			for (i = 0; i < nseg; i++)
			{
				cons[i] = anser_open_consumer(&key);
				if (cons[i] == NULL)
					all_open = false;
			}

			if (all_open && anser_wait_consumer_count(&key, nseg))
			{
				/*
				 * Cancel with consumers attached.  This marks the channel
				 * cancelled but must leave the payload DSM pinned -- a consumer
				 * may still be borrowing it -- so it is still present right after.
				 */
				AnserCancelQuery(key.gp_session_id, key.gp_command_count);
				present_after_cancel = AnserChannelPayloadBytes(&key) > 0;

				/* Drain every consumer (each reads its copy or gets cancelled). */
				for (i = 0; i < nseg; i++)
					(void) anser_consumer_returned_row(cons[i]);

				/* Slots drained: the sweep may now reclaim and free the payload. */
				AnserSetSweepEnabled(true);
				AnserServiceMaintenance();
				AnserSetSweepEnabled(false);

				gone_after_sweep = AnserChannelPayloadBytes(&key) < 0;
			}
		}
	}
	PG_FINALLY();
	{
		for (i = 0; i < nseg; i++)
			if (cons[i] != NULL)
				PQfinish(cons[i]);
		qdHostname = saved_host;
		qdPostmasterPort = saved_port;
	}
	PG_END_TRY();

	PG_RETURN_BOOL(present_after_cancel && gone_after_sweep);
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

/* Send a cancel request for conn's in-flight query (best effort). */
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

/*
 * Build a serialized single bloom part (index 0 of 1) carrying one int value,
 * seeded from condition_key so every part on the same channel is byte-identical
 * in size and parameters (letting the coordinator OR-fold them in place).  The
 * caller frees the returned buffer; *len_out gets the serialized length.
 */
static char *
anser_make_test_part(const char *condition_key, int32 value, Size *len_out)
{
	uint64		seed = AnserBloomSeed(condition_key);
	bloom_filter *filter = AnserBloomCreate(ANSER_TEST_ELEMS,
											ANSER_TEST_MAX_PAYLOAD, seed);
	Datum		d = Int32GetDatum(value);
	Size		sz;
	Size		len = 0;
	char	   *buf;

	bloom_add_element(filter, (unsigned char *) &d, sizeof(Datum));
	sz = AnserBloomSerializedSize(filter);
	buf = palloc(sz);
	if (!AnserBloomSerializePart(filter, 0, 1, buf, sz, &len))
	{
		bloom_free(filter);
		pfree(buf);
		return NULL;
	}
	bloom_free(filter);
	*len_out = len;
	return buf;
}

/*
 * Fill key from the common leading args (session id, command count, condition
 * id, condition key) shared by most test functions.  Returns false on invalid
 * input.
 */
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

/* Printable name for a channel state ("UNKNOWN" when out of range). */
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
