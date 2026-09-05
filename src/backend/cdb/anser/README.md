# Anser — adaptive information sharing

Anser is a runtime pub/sub facility for MPP query execution. Producers on the
segments publish a small piece of information about a query (today: a bloom
filter over a join-build key), the coordinator unions the per-segment parts into
one global payload, and consumers on the segments receive it and use it to prune
work (today: skip probe rows that cannot join). The shared state lives in a
fixed coordinator-resident shared-memory **channel map**, serviced by two
background workers (gather + send).

This document covers the architecture, the segment→coordinator network
transport and its token authentication, the configuration surface, what a
*channel* is, and the *channel state machine*. For the plan-tree integration
see `anserplan.c`; for the payload/bloom protocol see `anserfilter.c` and
`lib/bloomfilter.c`.

## Architecture

All Anser state lives in fixed **coordinator shared memory**, allocated once at
postmaster start. Producers and consumers are ordinary query backends
(coordinator-resident, or on segments reaching the coordinator over libpq); they
never talk to each other directly and never own the shared state — they only
hand work to, or wait on, two **background workers** that do.

Three shared structures, two hand-off points:

- **Channel map** — the hash of channels (one per runtime condition per query),
  holding each channel's state, accounting, and payload. The single source of
  truth.
- **Submission queue** — the producer → gather hand-off. A producer copies its
  serialized part into a free queue entry, signals the gather worker, and blocks
  for an ACK; it never touches the channel payload itself.
- **Wait table** — the send → consumer hand-off, an array of **slots**. A *slot*
  is one consumer's reservation on a channel: it records the consumer's key, a
  pointer to that backend's latch, and a place for the send worker to stamp the
  delivered payload (or a cancel). A blocked consumer owns one slot and sleeps on
  its latch until the send worker flips it.

The two **background workers** exist because the shared state has to keep moving
independent of any one transient/blocked backend:

- **Gather service** — drains the submission queue: for each part it folds
  (bitwise-OR unions) the data into the target channel's single payload, advances
  the channel toward `READY`, and ACKs the producer. It also runs periodic
  maintenance: time out stragglers (`gp_anser_timeout_ms`) and sweep terminal or
  orphaned channels.
- **Send service** — delivers: once a channel is `READY` it copies the combined
  payload into every waiting slot and wakes those consumers' latches; when all
  expected consumers are served it recycles the channel to `CONSUMED`.

Both workers sleep on a latch and wake on demand — a producer's submission sets
the gather latch, a publish/registration sets the send latch — plus a periodic
timeout so maintenance runs even when idle. Concurrency is guarded by two
LWLocks, always taken in the order `AnserChannelLock` → `AnserRingLock`.

The pay-off of this split: a producer can publish and leave, a consumer can block
without pinning anything, and the coordinator still unions once and fans the
result out — see the data-flow section below.

### Gather-service wakeup cycle

The gather worker owns the **gather latch** and sleeps on it between passes.
Setting that latch is the "producer work is pending" signal — raised by
`AnserRegisterCondition`, `AnserProducerBegin`, `AnserPublish`, and (the common
one) `AnserEnqueueSubmission` when a remote producer drops a part into a free
submission-queue slot and blocks for its ACK.

```
producer backend                         gather worker (looping)
────────────────                         ───────────────────────
enqueue part → slot = PENDING
SetLatch(gather_latch) ───────────────►  WaitLatch(gather_latch) returns
block on own latch                       ResetLatch(gather_latch)
   │                                     AnserGatherServiceCycle():
   │                                       for each PENDING slot:
   │                                         AnserGatherApply()  ← fold/union part,
   │                                             advance channel toward READY
   │                                         slot = ACCEPTED/REJECTED
   ▼                                         SetLatch(producer_latch) ─┐
wake, read ACK, free slot  ◄───────────────────────────────────────── ┘
                                           AnserCancelStaleChannels()   (timeouts)
                                           AnserReapSubmissionSlots()
                                         AnserServiceMaintenance()      (orphan sweep)
                                         SetLatch(send_latch) on READY ─► send worker
                                         WaitLatch(gather_latch) …      (sleep again)
```

Key properties:

- **No lost wakeups.** If the latch is set while the worker is mid-pass (not yet
  waiting), it stays set and the next `WaitLatch` returns immediately.
- **Two hand-offs.** The cycle wakes each producer via *its own* latch (the ACK),
  and wakes the **send** worker via the send latch once a channel reaches `READY`
  — the gather worker never delivers to consumers itself.
- **Timed fallback.** The same cycle also runs every
  `ANSER_SERVICE_WAKEUP_INTERVAL_MS` even with no latch set, so stale
  `COLLECTING` channels time out and orphaned channels get swept while idle.

### Send-service wakeup cycle

The send worker owns the **send latch** and sleeps on it. Setting that latch
means "a channel is now deliverable or cancellable, or a consumer is now
waiting" — raised by the gather worker when a channel reaches `READY`
(`AnserGatherApply`), by the publish/cancel/timeout paths when a channel is
cancelled, and by a consumer when it subscribes (`AnserConsumerWait`) or abandons
its wait (`AnserAbandonWaitSlot`).

```
gather worker / canceller /               send worker (looping)                consumer backend
consumer subscribe                        ─────────────────────                ────────────────
──────────────────────────                                                     subscribe: slot = WAITING
channel → READY | CANCELLED | CONSUMED
SetLatch(send_latch) ───────────────────► WaitLatch(send_latch) returns
                                          ResetLatch(send_latch)
                                          AnserSendServiceCycle():
                                            for each READY/terminal channel:
                                              for each WAITING slot on it:
                                                READY  → copy payload → slot,
                                                          slot = DELIVERED ────► wake, read payload,
                                                terminal → slot = CANCELLED ───► (or cancel → fail open),
                                                SetLatch(consumer_latch)         free slot
                                                done_consumers++
                                              all served → recycle CONSUMED
                                            AnserReapWaitSlots()
                                          WaitLatch(send_latch) …  (sleep again)
```

Key properties:

- **Per-consumer delivery.** Each `WAITING` slot gets its *own* pinned copy of the
  merged payload, so delivery is per-consumer — one consumer's cancel (or a DSM
  shortage that cancels just it) never affects another's delivery.
- **Straggler safety.** A consumer that registers on an already-terminal channel
  is handed a cancel and fails open, instead of blocking on a channel the sweep
  would otherwise never reclaim.
- **Recycle.** Once `done_consumers` reaches `expected_consumers` (one per
  segment) the channel becomes `CONSUMED` and its payload is freed.
- **No lost wakeups / timed fallback.** Like the gather worker: a latch set
  mid-pass is honored next loop, and the same cycle runs every
  `ANSER_SERVICE_WAKEUP_INTERVAL_MS` so straggler cancels and recycling still
  happen while idle.

## Network transport: how segments connect and authenticate

Coordinator-resident producers/consumers touch the channel map directly.
Segment executors cannot — the map lives in the coordinator's shared memory — so
they open an **ordinary libpq connection back to the QD** and drive the
`gp_anser_producer_begin` / `gp_anser_publish` / `gp_anser_consume_wait` built-in
functions from that backend (`anserclient.c`). The QD address comes from
`gp_qd_hostname` / `gp_qd_port`, which the dispatcher injects into every QE; the
connection reuses the query's database and the **session user**
(`MyProcPort->user_name` — the authenticated login role, unaffected by
`SET ROLE`), and sets `application_name=anser_rf` so these backends are
identifiable on the coordinator.

### Authentication: per-session token (the parallel-retrieve-cursor model)

The backward connection must not depend on `pg_hba.conf`: a stock
`gpinitsystem` cluster grants `trust` to coordinator IPs on the *segments* (that
is what makes QD→QE dispatch connections work), but never adds segment hosts to
the *coordinator's* pg_hba — so a segment→QD connection would be rejected by
default. Anser therefore authenticates these connections the same way
`PARALLEL RETRIEVE CURSOR` retrieve sessions do (`retrieve_conn_authentication`
in `libpq/auth.c`):

1. **Token registration (QD, plan time).** When the planner pass injects a
   runtime filter into a query, the QD registers a **per-session token**:
   128 bits of `pg_strong_random`, hex-encoded, stored in the shared-memory
   *session token hash* keyed by `(gp_session_id, session user)`
   (`AnserGetOrCreateSessionToken` in `anser.c`). One token per session; the
   entry is removed when the session exits.
2. **Delivery to segments.** The token travels inside the dispatched plan (a
   `String` in the producer/consumer `CustomScan.custom_private`), so it only
   crosses the already-trusted QD→QE dispatch channel.
3. **Connection (segment).** The segment executor connects with the startup
   marker `gp_anser_conn=true` (passed via the libpq `options` keyword) and the
   token as the connection `password`.
4. **Verification (QD, auth time).** `ClientAuthentication` checks the marker
   **before** pg_hba is consulted and runs `anser_conn_authentication`: it
   requests the password, resolves `user_name` to a role OID, and calls
   `AnserSessionTokenIsValid`, which scans the token hash for a matching
   `(user, token)` pair. On match the connection becomes an ordinary backend
   for that user (`FakeClientAuthentication`); on mismatch it is rejected with
   `FATAL`.

```
segment executor                     coordinator
────────────────                     ───────────
libpq connect: user=<session user>,
  options="-c gp_anser_conn=true",
  password=<token>
                ── startup ────────► ClientAuthentication:
                                     marker seen → skip pg_hba
                ◄── AUTH_REQ_PASSWORD
token ─────────────────────────────► AnserSessionTokenIsValid(user, token)
                                       scans session token hash (shmem)
                ◄── OK / FATAL
SELECT gp_anser_producer_begin(...) ─► ... runs as the session user
```

Properties and limits of this model:

- **No pg_hba change needed** on the coordinator for segment hosts; no password
  of the user ever leaves the client.
- The token is **per session, not per query**, and grants a *full* SQL backend
  as that user (unlike retrieve sessions, which are utility-mode and
  `RETRIEVE`-only). Anyone who learns a live session's token can connect as its
  user — the token never leaves the trusted dispatch channel, but it does
  appear in debug-level plan dumps (`debug_print_plan`), so treat those logs as
  sensitive.
- **Channel-level access control is unchanged and independent**: a channel is
  bound to the role that created it (`AnserChannelEntry.creator_role`), so even
  an authenticated connection can only produce/consume on channels its own role
  created (or any, if superuser).
- **Fail open.** If no token was registered (subsystem off, token hash full) or
  authentication fails for any reason, the connection attempt returns NULL and
  the segment runs unfiltered — never an error, never wrong results.
- **Without a token** the client omits the marker and password, and the
  connection goes through ordinary pg_hba authentication (previous behavior);
  this also covers hand-built or test deployments where the admin chose to
  provision pg_hba entries instead.

The `gp_anser_conn` GUC itself is a marker only (`PGC_BACKEND`, not settable in
`postgresql.conf`, not synced to segments); its value is read from the raw
startup options during authentication.

## GUCs

| GUC | Default | Context | Meaning |
| --- | --- | --- | --- |
| `gp_anser_enable` | `off` | POSTMASTER | Master switch. When on, the channel-map shared memory is sized/created and the gather + send background workers are started at postmaster start. Off = the whole subsystem is absent (zero shmem, no workers). |
| `gp_anser_runtime_filter` | `off` | USERSET | Enables the post-planning pass that injects bloom-filter producer/consumer nodes into a matching plan. Requires `gp_anser_enable`; without it the pass is a no-op even when the subsystem is up. |
| `gp_anser_max_channels` | `0` (auto) | POSTMASTER | Number of channels the map can hold; sizes the channel hash, the producer submission queue, and (× `gp_anser_max_consumers_per_channel`) the consumer wait table. `0` auto-sizes to `max_connections * gp_max_slices` — at most `max_connections` concurrent queries, each opening up to `gp_max_slices` runtime-filter channels — falling back to a fixed per-connection budget (8) when `gp_max_slices` is unbounded (`0`). Captured once at postmaster start so it is stable across all backends. |
| `gp_anser_max_info_size` | `65 MB` | POSTMASTER | Maximum serialized payload (unioned bloom filter + part header) a channel may hold; caps per-channel memory and bounds the effective bloom-filter size. The default is `64 MB + 1 MB` so a full 64 MB power-of-two bitset fits with its header; `bloom_create` also floors every bitset at 1 MB. |
| `gp_anser_max_consumers_per_channel` | `64` | POSTMASTER | Wait-table slots reserved per channel; the consumer wait table is sized `gp_anser_max_channels * this`. Bounds how many consumers can block on one channel at once. |
| `gp_anser_timeout_ms` | `1000` | USERSET | Produce/collect deadline. A channel that is still collecting parts when this elapses is cancelled by the maintenance sweep, so waiting consumers fail open (run unfiltered) rather than hang. |

## Data flow: producer → gather (bitwise union) → consumer

The parts from all segments are combined into **one** payload by a **bitwise OR
on the coordinator**, and that single combined payload is delivered to every
consumer. This is the core of Anser and worth stating precisely, because it is
*not* a concatenation:

```
segment 0 producer:  bitset 0000 0001  ┐
segment 1 producer:  bitset 0000 0010  ├─ libpq ─► gather service (coordinator)
segment N producer:        ...         ┘             │
                                                     │  fold each part into the
                                                     │  running merged bitset:
                                                     │     0000 0001
                                                     │  OR 0000 0010
                                                     ▼  = 0000 0011   (one part)
                                            channel payload = single merged bitset
                                                     │
                              send service ──────────┼───────────────┐
                                       ▼             ▼               ▼
                              consumer seg 0   consumer seg 1 ... consumer seg N
                              each receives the SAME combined 0000 0011
```

Step by step:

1. **Produce (per segment, in parallel).** Each segment's producer builds a bloom
   filter over its local build keys (`bloom_create` from `total_elems` /
   `max_payload` / a `condition_key`-derived seed, all carried in the plan node —
   *not* on the wire) and serializes it as one *part*. Because every producer and
   the consumer pass the identical parameters, every part has a byte-for-byte
   identical size and shape. Segment producers push their parts to the coordinator
   concurrently over their own libpq connections — the network transfer is
   parallel, and the submission queue is sized `channels * per-channel producers`
   so they hand off without serializing.

2. **Gather (coordinator, once per part).** The coordinator never reconstructs a
   filter — it works on raw bytes. The **first** part is stored verbatim; every
   later part is folded into the channel's payload with an in-place **bitwise OR**
   of the bitset (`AnserBloomFoldPartInPlace` in `anserfilter.c`, from
   `AnserStorePayloadDSM` in `anser.c`). The payload is therefore always a
   **single merged bitset**, the size of one filter — it does **not** grow with
   the segment count. The OR requires the incoming part to be the same size as the
   accumulator (guaranteed by the shared parameters); a mismatch makes the fold
   fail and the channel is cancelled (consumers fail open).

3. **Deliver (coordinator → every consumer).** Once every expected part is folded
   (channel `READY`), the send service delivers a copy of that one combined
   bitset to each waiting consumer. Delivery is O(segments) bytes, not
   O(segments²), and the union work is done once on the master rather than
   repeated in every consumer.

4. **Consume (per segment).** Each consumer rebuilds an empty filter from its own
   plan parameters (the same `total_elems` / `max_payload` / seed the producers
   used) and loads the received bitset into it (`AnserBloomDeserializePart`),
   requiring the received length to match exactly (else it fails open). It does
   **not** re-union anything; the merged header's part count is surfaced as the
   `Rows Removed by Bloom Filter` / parts-received EXPLAIN stats.

Correctness note: the combined filter is the OR (super-set) of every segment's
build keys, so it can only ever have *false positives*, never false negatives —
a probe row it rejects genuinely cannot join. Anser therefore only changes
performance, never results; any failure along this path degrades to "no filter"
(fail open).

## Channel

A **channel** is one rendezvous point between the producers and consumers of a
single piece of runtime information, for a single query. It is a shared-memory
entry (`AnserChannelEntry`) in the coordinator's channel hash, addressed by an
`AnserChannelKey`:

```
AnserChannelKey = { gp_session_id, gp_command_count, condition_id, condition_key[64] }
```

- `gp_session_id` + `gp_command_count` scope the channel to one query execution,
  so keys never collide across sessions or across statements in a session.
- `condition_id` distinguishes multiple filters within the same query.
- `condition_key` is an opaque string describing the filtered condition (today a
  synthetic `rf:<build>.<attno>=<probe>.<attno>` string). Both sides derive it
  independently and must agree — it is what makes a producer and a consumer meet
  on the same channel.

The entry also tracks bookkeeping used by the state machine: `expected_producers`
/ `done_producers` (one part per segment), `consumers` / `expected_consumers` /
`done_consumers` (delivery accounting), the `creator_role` (only that role or a
superuser may produce/consume on it), the payload (`dsm_handle` + `data_len`),
and `created_at` / `updated_at` timestamps used by the maintenance sweep.

The channel map is finite and fixed-size. Terminal channels are reclaimed by the
background maintenance sweep (or, under map pressure, by emergency reclamation on
registration) so their slots can be reused; a fresh registration landing on a
terminal entry resets it in place.

## `AnserChannelState` and the state flow

A channel moves through five states (`AnserChannelState`):

| State | Meaning |
| --- | --- |
| `PENDING` | Registered; no producer part received yet. |
| `COLLECTING` | At least one part received; still waiting for the rest. |
| `READY` | All expected parts collected and unioned; payload deliverable. |
| `CANCELLED` | Aborted (timeout / explicit cancel / owner death / query cancel). Terminal. Consumers fail open. |
| `CONSUMED` | Every expected consumer has been delivered the payload. Terminal. |

```
                         register (RegisterCondition / ProducerBegin)
                                    │
                                    ▼
                             ┌─────────────┐
                             │   PENDING   │
                             └─────────────┘
                                    │  first part published
                                    ▼
                             ┌─────────────┐
              ┌──────────────│ COLLECTING  │
              │              └─────────────┘
              │                     │  done_producers == expected_producers
              │                     ▼
              │              ┌─────────────┐
   cancel /   │              │    READY    │
   timeout /  │              └─────────────┘
   owner death│                     │  done_consumers == expected_consumers
   / query    │                     ▼
   cancel     │              ┌─────────────┐
              │              │  CONSUMED   │ (terminal)
              ▼              └─────────────┘
       ┌─────────────┐              │
       │  CANCELLED  │ (terminal)   │
       └─────────────┘              │
              │                     │
              └──────────┬──────────┘
                         ▼
             maintenance sweep reclaims slot  (→ NOT_FOUND)
             or a fresh register() resets the entry to PENDING
```

**Transitions:**

- **create → `PENDING`** — `AnserRegisterCondition` / `AnserProducerBegin` insert
  the entry (or reset a terminal one) with `expected_producers` set.
- **`PENDING` → `COLLECTING`** — the first part is published (`AnserPublish` /
  the gather service applying a submitted part). The part is unioned into the
  payload and `done_producers` is incremented.
- **`COLLECTING` → `READY`** — the part that makes `done_producers` reach
  `expected_producers` completes the union; the global payload is now
  deliverable and the send service wakes waiting consumers.
- **`READY` → `CONSUMED`** — the send service delivers the payload to each
  waiting consumer; when `done_consumers` reaches `expected_consumers` (one per
  segment) the channel is recycled to `CONSUMED` and its payload freed. Abandoned
  consumers (cancelled mid-wait) stop counting so this can still be reached.
- **`PENDING`/`COLLECTING` → `CANCELLED`** — via `gp_anser_timeout_ms` expiry
  (maintenance sweep on a still-`COLLECTING` channel), a producer publishing a
  cancel part, a whole-query `AnserCancelQuery`, or the creator backend dying.
  Any consumer blocked on the channel is woken with a cancel and **fails open**
  (runs unfiltered) — Anser never changes results, only performance.
- **terminal (`CANCELLED`/`CONSUMED`) → gone** — the background maintenance sweep
  (unless paused via the test-only `sweep_enabled` knob) removes terminal and
  orphaned channels, freeing the slot; a later registration reusing the same key
  starts over at `PENDING`.
