# Persistence and restart

## Directory layout

```
<store>/
  journal.sncf    append-only framed records
  snapshot.sncf   optional compacted state document
  journal.tmp     transient, only during compaction
  snapshot.tmp    transient, only during compaction
```

## Journal format

```
header  "SNCFJRN1" | u32 format version | u32 semantics revision | u64 first sequence
record  u32 payload length | u32 CRC-32C | payload bytes
```

The payload is a canonical JSON record document. Lengths are bounded by
`FabricConfig::max_record_bytes` before any allocation, and CRC-32C covers the
payload so a damaged record is detected before it is interpreted.

## Snapshot format

```
header  "SNCFSNP1" | u32 format version | u32 semantics revision |
        u64 sequence | u64 payload length | u32 CRC-32C | u32 reserved
body    canonical JSON state document
```

The body is exactly the durable portion of the canonical export, so the snapshot
codec and the export codec cannot drift apart.

## Commit semantics

`Journal::append` writes the frame, flushes it to the operating system, and then
synchronises it to stable storage before returning. The caller applies the record
only after the append reports success, so:

* a failure before the append leaves no trace;
* a crash after the flush but before the synchronise is exactly the ambiguity the
  command identity resolves — the record may or may not survive a machine
  failure, and the caller never received an acknowledgement;
* a crash after the synchronise but before the acknowledgement leaves a durable
  decision that a redelivered command will find in the dedupe table.

## Compaction

`compact()` writes a snapshot covering sequence *S*, replaces the snapshot file,
and only then rewrites the journal with first sequence *S+1*. The ordering makes
every interruption safe:

| Interrupted after | Result on reopen |
| --- | --- |
| snapshot write, before replace | stale snapshot plus complete journal; replays correctly |
| snapshot replace | snapshot at *S*; records up to *S* are filtered out of the journal |
| journal rewrite begins | snapshot at *S* plus the old journal; records up to *S* are skipped |
| journal removed, before its replacement is renamed | snapshot at *S* alone; a fresh journal starts at *S+1* |

Filtering records whose sequence is at or below the snapshot sequence makes the
snapshot-plus-journal combination idempotent regardless of where compaction
stopped.

## Recovery classification

| Disposition | Meaning |
| --- | --- |
| `fresh_store` | no journal existed; the store was created |
| `clean_reopen` | every frame validated |
| `torn_tail_recovered` | a partial trailing frame was truncated |
| `corrupt_tail_discarded` | the final frame failed CRC and was truncated |
| `snapshot_rejected` | the snapshot was unusable; the journal covered the history |
| `incompatible_version_refused` | format version is not supported |
| `semantics_mismatch_refused` | records were written under different semantics |
| `corrupt_refused` | damage outside a recoverable tail |
| `sequence_gap_refused` | a sequence gap or a replayed sequence |
| `truncated_refused` | the header itself is incomplete |
| `history_incomplete_refused` | the journal starts past sequence 1 with no snapshot covering the prefix |
| `append_rollback_failed` | a failed append could not be rolled back, so the file is no longer trustworthy |

The first five are usable; the rest refuse the store, and `Fabric::open`
returns the reason code instead of a runtime.

Discarded frames are counted, not merely discarded: `records_dropped` reports
how many whole frames were removed with a damaged tail, and `bytes_dropped`
reports the bytes. A recovery that silently dropped a record is not possible.

### History continuity

A journal either starts at sequence 1, or continues exactly where a snapshot that
is present on disk ends. Anything else means a prefix of history is missing, and
replaying the remainder would serve a silently partial state — so the store is
refused with `history_incomplete_refused`. This is what makes deleting or
damaging a snapshot a detectable fault rather than a silent loss of every entity
recorded before it.

### A failed append leaves no trace

An append captures the file offset before it writes. Every failure after the
write begins — short write, flush failure, synchronise failure — rolls the file
back to that offset, so:

* the failed record leaves no bytes behind, not even a torn frame;
* its sequence is not consumed, so the next commit reuses it and no replayed
  sequence can ever appear;
* a record that was never acknowledged can never be replayed into state.

If the rollback itself fails, the journal is marked unusable and every later
append is refused, because a file that cannot be truncated back to a known-good
length can no longer be trusted. `fail_next_append_once()` in `sncf/crash.hpp`
injects exactly this failure deterministically for the test suite.

## Conservative restart

Every open, including a clean one, advances durable state before serving:

1. The coordinator epoch and boot identity increase by one (`epoch_advanced`).
2. Every lease that was not revoked is revoked
   (`recovery_superseded_authority`).
3. Every instance that held authority becomes `superseded`; its lease binding is
   cleared.
4. Every in-flight attempt (staged, authorized, dispatched, acknowledged)
   becomes `ambiguous`, and the instance is marked
   `pending_intent_ambiguous`.
5. Every persisted capability evidence record becomes
   `pending_reverification` unless it was already stale or expired
   (`recovery_evidence_pending_reverification`).
6. Every scope that could have carried authority gets a strictly higher fencing
   token (`fencing_advanced`).

These are ordinary journal records, so the recovered state is exactly the state a
later replay reproduces. Nothing here can resurrect liveness or authority: an
instance never becomes `granted` again, a token never decreases, and an ambiguous
attempt stays ambiguous until an enforcement-side report matches its instance,
attempt, deployment generation and token.

## Evidence after restart

Persisted evidence keeps its original `observed_at`, `accepted_at` and the epoch
under which it was accepted. After a restart its freshness is
`pending_reverification`: it is neither fresh nor silently zero, and it cannot
justify authority. The source revalidates it with `reverify_evidence`, which
records a new observation and moves it to `fresh`.

## Bounded growth

The journal is bounded by `max_journal_records` and `max_journal_bytes`. When
either bound is reached, further appends are refused with `refused_store_full`
until compaction runs, so growth is bounded and the refusal is observable rather
than silent.

The lease table is bounded by `max_leases`, the same bound the snapshot decoder
enforces, so any snapshot the runtime writes is one it can read back. Before a
grant, revoked and expired lease rows are pruned deterministically and the
removal is recorded as a `leases_removed` patch; pruning is counted in
`lease_evictions`. If the table is full of unexpired leases the grant is refused
with `refused_capacity_exceeded`.

## Liveness is not durable

`DeviceRecord::present` records what was last observed, not a standing fact.
Two rules keep it honest:

* A restart invalidates it: the conservative pass sets `present = false` and
  marks the observation `pending_reverification`
  (`recovery_observation_invalidated`). Activation then refuses with
  `refused_evidence_source_unavailable` until the device is observed again.
* Decision time checks the observation window: activation refuses with
  `refused_expired_evidence` when the stored observation is older than
  `observation_max_age`, so a device that vanished after the last report cannot
  keep authorising deployments.

## Idempotency window

The command idempotency table is bounded by `max_dedupe_entries`. When an entry
is evicted, the highest evicted command identity for that principal becomes a
durable floor. A command at or below its principal floor can no longer be told
apart from new work, so it is refused with `refused_replay_window_exceeded`
rather than re-executed — a duplicate can degrade to a refusal, never to a second
deployment. The floor survives restart and is part of the durable state.
