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
 * anserfilter.c
 *	  Bloom-filter payload helpers for Anser channels.
 *
 * IDENTIFICATION
 *	  src/backend/cdb/anser/anserfilter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/anserfilter.h"
#include "common/hashfn.h"
#include "port/pg_bitutils.h"

#define ANSER_BLOOM_MIN_BITSET_BITS	(1024U * 8U)
#define ANSER_BLOOM_DEFAULT_K		3

static bool AnserBloomValidateHeader(const AnserBloomPartHeader *header,
									 Size payload_len);
static uint64 AnserBloomFloorPowerOfTwo(uint64 value);

uint64
AnserBloomSeed(const char *condition_key)
{
	if (condition_key == NULL)
		return 0;

	return hash_bytes_extended((const unsigned char *) condition_key,
						   strlen(condition_key), 0);
}

uint64
AnserBloomChooseBitsetBits(int64 total_elems, Size max_payload_bytes)
{
	Size		max_bitset_bytes;
	uint64		target_bytes;
	uint64		bits;

	if (max_payload_bytes <= sizeof(AnserBloomPartHeader))
		return 0;

	max_bitset_bytes = max_payload_bytes - sizeof(AnserBloomPartHeader);
	if (max_bitset_bytes < ANSER_BLOOM_MIN_BITSET_BITS / BITS_PER_BYTE)
		return 0;

	/* Prefer about two bytes per estimated element, capped by the payload. */
	target_bytes = Max((uint64) ANSER_BLOOM_MIN_BITSET_BITS / BITS_PER_BYTE,
					   (uint64) Max(total_elems, 1) * 2);
	target_bytes = Min(target_bytes, (uint64) max_bitset_bytes);

	bits = AnserBloomFloorPowerOfTwo(target_bytes * BITS_PER_BYTE);
	if (bits < ANSER_BLOOM_MIN_BITSET_BITS)
		return 0;

	return bits;
}

bloom_filter *
AnserBloomCreate(int64 total_elems, Size max_payload_bytes, uint64 seed)
{
	uint64		bits;

	bits = AnserBloomChooseBitsetBits(total_elems, max_payload_bytes);
	if (bits == 0)
		return NULL;

	return bloom_create_with_params(bits, ANSER_BLOOM_DEFAULT_K, seed);
}

Size
AnserBloomSerializedSize(const bloom_filter *filter)
{
	if (filter == NULL)
		return 0;

	return sizeof(AnserBloomPartHeader) + bloom_bitset_bytes(filter);
}

/*
 * Does this payload look like a serialized bloom part?  Used by the coordinator
 * to tell a runtime-filter part (which it folds/unions) from opaque channel
 * bytes (which it concatenates).  A false positive is effectively impossible: a
 * part must carry the ABF1 magic, a known version, a power-of-two bitset, and a
 * self-consistent length.
 */
bool
AnserBloomLooksLikePart(const void *payload, Size payload_len)
{
	if (payload == NULL || payload_len < sizeof(AnserBloomPartHeader))
		return false;

	return AnserBloomValidateHeader((const AnserBloomPartHeader *) payload,
									payload_len);
}

/*
 * Fold one serialized bloom part into a running accumulator, on the coordinator.
 *
 * `acc`/`acc_len` is the current merged part (acc_len == 0 for the first fold);
 * `part`/`part_len` is the incoming per-segment part.  Both are unioned (bitwise
 * OR -- the parts share bitset params derived from the condition key) and the
 * result is returned as a freshly palloc'd single merged part (caller frees),
 * with part_index 0 and total_parts set to how many parts have been folded so
 * far.  This keeps the channel payload a single filter-sized chunk regardless of
 * the segment count, and lets consumers deserialize one part instead of unioning
 * N.  Returns NULL on malformed input or a parameter mismatch (the caller then
 * cancels the channel and consumers fail open).
 */
void *
AnserBloomFoldPart(const void *acc, Size acc_len,
				   const void *part, Size part_len, Size *out_len)
{
	bloom_filter *bf_part;
	bloom_filter *result;
	uint32		pi;
	uint32		tp;
	uint32		folded;
	Size		sz;
	Size		written = 0;
	void	   *out;

	if (out_len != NULL)
		*out_len = 0;

	bf_part = AnserBloomDeserializePart(part, part_len, &pi, &tp);
	if (bf_part == NULL)
		return NULL;

	if (acc == NULL || acc_len == 0)
	{
		result = bf_part;
		folded = 1;
	}
	else
	{
		uint32		acc_pi;
		uint32		acc_tp;
		bloom_filter *bf_acc;

		bf_acc = AnserBloomDeserializePart(acc, acc_len, &acc_pi, &acc_tp);
		if (bf_acc == NULL)
		{
			bloom_free(bf_part);
			return NULL;
		}
		if (!bloom_union(bf_acc, bf_part))
		{
			bloom_free(bf_acc);
			bloom_free(bf_part);
			return NULL;
		}
		bloom_free(bf_part);
		result = bf_acc;
		folded = acc_tp + 1;
	}

	sz = AnserBloomSerializedSize(result);
	out = palloc(sz);
	if (!AnserBloomSerializePart(result, 0, folded, out, sz, &written))
	{
		pfree(out);
		bloom_free(result);
		return NULL;
	}
	bloom_free(result);

	if (out_len != NULL)
		*out_len = written;
	return out;
}

/*
 * Combine an opaque payload into the accumulator by appending (concatenating).
 * This preserves the generic channel contract used by lower-level callers/tests
 * for payloads that are not bloom parts.  `acc` may be NULL/0 for the first one;
 * returns the concatenation as freshly palloc'd bytes (caller frees), length in
 * *out_len.  The incoming payload must be non-empty -- the caller validates that
 * before dispatching here (AnserStorePayloadDSM / AnserCombinePayload).
 */
static void *
AnserOpaqueFoldPart(const void *acc, Size acc_len,
					const void *incoming, Size incoming_len, Size *out_len)
{
	char	   *out;
	Size		total;

	Assert(incoming != NULL && incoming_len > 0);

	total = acc_len + incoming_len;
	out = palloc(total);
	if (acc != NULL && acc_len > 0)
		memcpy(out, acc, acc_len);
	memcpy(out + acc_len, incoming, incoming_len);

	if (out_len != NULL)
		*out_len = total;
	return out;
}

/*
 * Combine an incoming payload into a channel's current payload.
 *
 * `acc`/`acc_len` is the channel's existing payload (NULL/0 for the first one);
 * `incoming`/`incoming_len` is the new payload.  Returns the combined result as
 * freshly palloc'd bytes (caller frees) with its length in *out_len, by
 * dispatching on the incoming payload's kind:
 *
 *   - a serialized bloom part is folded (bitwise-OR unioned) into the running
 *     merged part, so the payload stays one filter-sized chunk regardless of how
 *     many parts arrive (AnserBloomFoldPart);
 *   - any other, opaque payload is appended (AnserOpaqueFoldPart).
 *
 * Within one channel every payload is the same kind, so acc and incoming always
 * agree.  The incoming payload must be non-empty -- the caller
 * (AnserStorePayloadDSM) validates that.  Returns NULL on malformed bloom input.
 * This is the pure payload-combination policy, free of shared memory, so it can
 * be unit-tested directly.
 */
void *
AnserCombinePayload(const void *acc, Size acc_len,
					const void *incoming, Size incoming_len, Size *out_len)
{
	Assert(incoming != NULL && incoming_len > 0);

	if (AnserBloomLooksLikePart(incoming, incoming_len))
		return AnserBloomFoldPart(acc, acc_len, incoming, incoming_len, out_len);

	return AnserOpaqueFoldPart(acc, acc_len, incoming, incoming_len, out_len);
}

bool
AnserBloomSerializePart(const bloom_filter *filter, uint32 part_index,
						uint32 total_parts, void *buffer, Size buffer_size,
						Size *payload_len)
{
	AnserBloomPartHeader header;
	Size		bitset_bytes;
	Size		total_len;

	if (payload_len != NULL)
		*payload_len = 0;

	if (filter == NULL || buffer == NULL || total_parts == 0 ||
		part_index >= total_parts)
		return false;

	bitset_bytes = bloom_bitset_bytes(filter);
	total_len = sizeof(AnserBloomPartHeader) + bitset_bytes;
	if (buffer_size < total_len)
		return false;

	MemSet(&header, 0, sizeof(header));
	header.magic = ANSER_BLOOM_PART_MAGIC;
	header.version = ANSER_BLOOM_PART_VERSION;
	header.k_hash_funcs = bloom_k_hash_funcs(filter);
	header.seed = bloom_seed(filter);
	header.bitset_bits = bloom_bitset_bits(filter);
	header.part_index = part_index;
	header.total_parts = total_parts;

	memcpy(buffer, &header, sizeof(header));
	memcpy((char *) buffer + sizeof(header), bloom_bitset_data(filter),
		   bitset_bytes);

	if (payload_len != NULL)
		*payload_len = total_len;
	return true;
}

bloom_filter *
AnserBloomDeserializePart(const void *payload, Size payload_len,
						  uint32 *part_index, uint32 *total_parts)
{
	const AnserBloomPartHeader *header;
	const unsigned char *bitset;
	Size		bitset_bytes;
	bloom_filter *filter;

	if (part_index != NULL)
		*part_index = 0;
	if (total_parts != NULL)
		*total_parts = 0;

	if (payload == NULL || payload_len < sizeof(AnserBloomPartHeader))
		return NULL;

	header = (const AnserBloomPartHeader *) payload;
	if (!AnserBloomValidateHeader(header, payload_len))
		return NULL;

	bitset_bytes = (Size) (header->bitset_bits / BITS_PER_BYTE);
	bitset = (const unsigned char *) payload + sizeof(AnserBloomPartHeader);

	filter = bloom_create_with_params(header->bitset_bits,
								   header->k_hash_funcs,
								   header->seed);
	if (filter == NULL)
		return NULL;

	bloom_set_bitset_data(filter, bitset, bitset_bytes);
	if (part_index != NULL)
		*part_index = header->part_index;
	if (total_parts != NULL)
		*total_parts = header->total_parts;
	return filter;
}

bloom_filter *
AnserBloomUnionParts(const void *payload, Size payload_len,
					 uint32 expected_parts, uint32 *received_parts)
{
	const char *ptr = (const char *) payload;
	Size		remaining = payload_len;
	bloom_filter *result = NULL;
	uint32		seen = 0;

	if (received_parts != NULL)
		*received_parts = 0;

	if (payload == NULL || expected_parts == 0)
		return NULL;

	while (remaining > 0)
	{
		const AnserBloomPartHeader *header;
		Size		part_len;
		uint32		part_index;
		uint32		total_parts;
		bloom_filter *part;

		if (remaining < sizeof(AnserBloomPartHeader))
			goto fail;

		header = (const AnserBloomPartHeader *) ptr;
		if (!AnserBloomValidateHeader(header, remaining))
			goto fail;

		part_len = sizeof(AnserBloomPartHeader) +
			(Size) (header->bitset_bits / BITS_PER_BYTE);
		part = AnserBloomDeserializePart(ptr, part_len, &part_index,
										 &total_parts);
		if (part == NULL)
			goto fail;

		if (total_parts != expected_parts || part_index >= expected_parts)
		{
			bloom_free(part);
			goto fail;
		}

		if (result == NULL)
			result = part;
		else
		{
			if (!bloom_union(result, part))
			{
				bloom_free(part);
				goto fail;
			}
			bloom_free(part);
		}

		seen++;
		ptr += part_len;
		remaining -= part_len;
	}

	if (received_parts != NULL)
		*received_parts = seen;

	if (seen != expected_parts)
		goto fail;

	return result;

fail:
	if (received_parts != NULL)
		*received_parts = seen;
	if (result != NULL)
		bloom_free(result);
	return NULL;
}

static uint64
AnserBloomFloorPowerOfTwo(uint64 value)
{
	uint64		result = 1;

	while (result <= value / 2 && result < (PG_UINT32_MAX + UINT64CONST(1)))
		result <<= 1;

	return result;
}

static bool
AnserBloomValidateHeader(const AnserBloomPartHeader *header, Size payload_len)
{
	Size		bitset_bytes;

	if (header == NULL)
		return false;

	if (header->magic != ANSER_BLOOM_PART_MAGIC ||
		header->version != ANSER_BLOOM_PART_VERSION)
		return false;

	if (header->total_parts == 0 || header->part_index >= header->total_parts)
		return false;

	if (header->bitset_bits < ANSER_BLOOM_MIN_BITSET_BITS ||
		header->bitset_bits > (PG_UINT32_MAX + UINT64CONST(1)) ||
		((header->bitset_bits - 1) & header->bitset_bits) != 0)
		return false;

	if (header->k_hash_funcs < 1 || header->k_hash_funcs > 10)
		return false;

	bitset_bytes = (Size) (header->bitset_bits / BITS_PER_BYTE);
	return payload_len >= sizeof(AnserBloomPartHeader) + bitset_bytes;
}
