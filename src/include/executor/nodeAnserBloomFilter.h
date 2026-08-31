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
 * nodeAnserBloomFilter.h
 *	  Standalone Anser Bloom filter producer/consumer executor helpers.
 *
 * IDENTIFICATION
 *	  src/include/executor/nodeAnserBloomFilter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODE_ANSER_BLOOM_FILTER_H
#define NODE_ANSER_BLOOM_FILTER_H

#include "postgres.h"

#include "cdb/anser.h"
#include "lib/bloomfilter.h"

/*
 * Opaque executor state handles for the Bloom filter producer and
 * consumer; the struct definitions are private to
 * nodeAnserBloomFilterProduce.c and nodeAnserBloomFilterConsume.c.
 */
typedef struct AnserBloomFilterProduceState AnserBloomFilterProduceState;
typedef struct AnserBloomFilterConsumeState AnserBloomFilterConsumeState;

/* Producer side: build one Bloom filter part and publish it to the channel. */

/*
 * `token` is the QD session token used to authenticate the segment -> QD
 * libpq connection (parallel-retrieve-cursor model); NULL means connect
 * without it and rely on pg_hba.  Ignored on the coordinator-local path.
 */
extern AnserBloomFilterProduceState *ExecInitAnserBloomFilterProduce(
										const AnserChannelKey *channel_key,
										int64 total_elems,
										Size max_payload_bytes,
										uint32 part_index,
										uint32 total_parts,
										const char *token);
extern void ExecAnserBloomFilterProduceAddDatum(AnserBloomFilterProduceState *state,
										 Datum value, bool isnull);
extern bool ExecAnserBloomFilterProducePublish(AnserBloomFilterProduceState *state);
extern bool ExecAnserBloomFilterProduceCancel(AnserBloomFilterProduceState *state);
extern void ExecEndAnserBloomFilterProduce(AnserBloomFilterProduceState *state);

/* Consumer side: gather all parts of a channel into one Bloom filter. */
extern AnserBloomFilterConsumeState *ExecInitAnserBloomFilterConsume(
										const AnserChannelKey *channel_key,
										int64 total_elems,
										Size max_payload_bytes,
										uint32 expected_parts,
										const char *token);
extern bool ExecAnserBloomFilterConsume(AnserBloomFilterConsumeState *state,
									long registration_timeout_ms);
extern bloom_filter *ExecAnserBloomFilterConsumerGetFilter(
									AnserBloomFilterConsumeState *state);
extern uint32 ExecAnserBloomFilterConsumerReceivedParts(
									AnserBloomFilterConsumeState *state);
extern bool ExecAnserBloomFilterConsumerWasCancelled(
								  AnserBloomFilterConsumeState *state);
extern void ExecEndAnserBloomFilterConsume(AnserBloomFilterConsumeState *state);

#endif							/* NODE_ANSER_BLOOM_FILTER_H */
