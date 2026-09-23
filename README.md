# SmartNIC Control Fabric

A standalone, vendor-neutral C++20 runtime that owns the control-plane state and
authority for programmable SmartNIC functions.

The fabric decides **what may run, where, and under whose authority**, records
what was actually observed, and refuses everything it cannot justify. It does not
implement firmware, packet processing, vendor SDKs, route computation, host
networking or telemetry collection, and it does not absorb those neighbouring
products.

```
registration ──▶ compatibility ──▶ deployment intent ──▶ activation authority
                                                              │
                          verified effect ◀── observed effect ◀─┘
```

## What this runtime owns

| Owned here | Owned by an adjacent runtime |
| --- | --- |
| SmartNIC, device, package and function-instance identity | Physical topology discovery |
| Capability and firmware/runtime compatibility gating | Capability measurement |
| Deployment intent, planning and replacement | Policy authoring |
| Exclusive scope, leases and durable fencing tokens | Execution of the function |
| Quiesce, drain, withdrawal and rollback intent | Packet processing and data path |
| Observed/verified effect bookkeeping | Telemetry collection |
| Coordinator epoch, boot identity and restart recovery | Cluster membership and election |

Evidence flows in from adjacent runtimes (topology, device observation,
capability, policy, authority, execution) as typed reports with provenance and
generations. This runtime never fabricates the evidence it consumes.

## Core state model

Identities are strongly typed: SmartNIC, physical device, device incarnation,
device model, function package, function instance, port, queue, principal,
capability generation, compatibility generation, deployment generation, policy
generation, coordinator epoch, boot identity, attempt, lease, fencing token,
command and record sequence. They cannot be substituted for one another.

Orthogonal state axes are kept distinct rather than collapsed:

* **Desired state** — `none`, `deployed`, `active`, `quiesced`, `withdrawn`
* **Lifecycle state** — `unknown`, `desired`, `eligible`, `authorized`,
  `acknowledged`, `applied`, `verified`, `quiesced`, `failed`,
  `unsupported`, `stale`, `withdrawn`
* **Authority state** — `none`, `granted`, `revoked`, `expired`, `superseded`
* **Freshness** — `unknown`, `fresh`, `stale`, `expired`, `pending_reverification`
* **Attempt phase** — `unknown`, `staged`, `authorized`, `dispatched`,
  `acknowledged`, `applied`, `verified`, `failed`, `aborted`, `ambiguous`,
  `quiesced`, `withdrawn`

Every accepted or refused decision carries one stable numeric reason code
(`include/sncf/status.hpp`, names in `src/status.cpp`). Codes are never
renumbered; new ones are appended.

## Guarantees

* **One authoritative instance per exclusive scope.** A scope is a port of a
  device, or the whole device when the function is not port scoped. Two live
  instances can never hold the same scope; the second activation is refused with
  `refused_scope_conflict`.
* **Stale incarnations are fenced.** Fencing tokens are strictly monotonic per
  scope and durable. Advancing a device incarnation fences every function bound
  to the old one, and a restart advances the mark for every scope that carried
  authority.
* **Incompatible functions never activate.** Capability generation, required
  capability bits, device model, compatibility generation, firmware version,
  freshness and provenance are all gated, in a fixed order, with distinct refusal
  codes.
* **Restart never resurrects liveness or authority.** Opening a store advances
  the coordinator epoch and boot identity, supersedes every lease, revokes every
  granted authority, and marks every in-flight attempt `ambiguous`. Persisted
  evidence becomes `pending_reverification` and cannot justify a decision until
  the source revalidates it.
* **Ambiguity stays ambiguous.** A crash between "intent dispatched" and "effect
  reported" leaves the attempt explicitly ambiguous. Only a matching
  enforcement-side report — same instance, attempt, generation and token — can
  resolve it, and it resolves to whatever the enforcement side reports.
* **Applied and verified claims require enforcement-side evidence.** A digest of
  the execution report is mandatory. Acknowledgement is not application, and
  application is not verification.
* **Missing evidence is never zero, false or success.** Absent, stale, expired,
  superseded, conflicting and unknown each have their own reason code.
* **Duplicate delivery is idempotent.** Every mutating command carries a caller
  chosen command identity; replay returns the recorded outcome and changes
  nothing, across restarts as well as within a process. When a command falls out
  of the bounded idempotency window it is refused with
  `refused_replay_window_exceeded` rather than re-executed, so a retry can
  degrade to a refusal but never to a second deployment.
* **A failed append leaves no trace.** The journal rolls back to its pre-write
  length on any failure after the write begins, so a torn frame can never strand
  later records and an unacknowledged record can never be replayed into state.
* **Liveness is evidence, not a fact.** A restart invalidates device presence and
  observations age out of their window, so a vanished device cannot keep
  authorising deployments.
* **Inspection never mutates.** `sncf verify`, `inspect`, `export` and
  `explain` open the store read-only: they write no recovery record and refuse
  every mutation, so a diagnostic cannot revoke a live coordinator's authority.
* **Every bounded truncation, refusal and eviction is accounted for.** Counters
  name the structure whose bound was hit, and a bounded event ring records the
  decision.
* **Deterministic explanations and exports.** The same evidence and policy always
  produce byte-identical canonical JSON, and explanations list the ordered
  factors — with evidence digests and generations — that made a decision legal.

## Build

Requirements: CMake 3.20+, a C++20 compiler, and threads. On Windows the project
is validated with MSVC 19.44 (`/W4 /WX /permissive-`).

```sh
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `SNCF_BUILD_TESTS` | ON when top level | Build the test suite |
| `SNCF_BUILD_TOOLS` | ON when top level | Build the `sncf` command line tool |
| `SNCF_STRICT_WARNINGS` | ON | Apply the strict first-party warning set |
| `SNCF_ENABLE_ASAN` | OFF | Build first-party targets with AddressSanitizer |

Strict warning flags are applied `PRIVATE` to first-party targets only and never
appear in the exported package.

## Install and consume

```sh
cmake --install build/dev --prefix /path/to/prefix
```

```cmake
find_package(sncf 1.0 REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE sncf::sncf)
```

`examples/downstream` is a complete independent consumer that builds against an
installed prefix only, with its own warning policy, and runs a full synthetic
lifecycle.

## Command line

```
sncf inspect  --store DIR [--json]      summarise the durable state (read-only)
sncf export   --store DIR               print the canonical export  (read-only)
sncf verify   --store DIR               open the store and report recovery (read-only)
sncf explain  --store DIR --instance N  explain an instance (read-only)
sncf serve    --store DIR [--port N] [--connections N] [--workers N]
sncf selftest --store DIR               run a scripted lifecycle check
```

## Service surface

`sncf serve` exposes the framed control-fabric protocol on a bounded TCP
listener (one accept thread, a fixed worker pool, a bounded connection queue).
Frames carry a magic, version, kind, payload length, CRC-32C and a request
identity; payloads are canonical JSON documents. A malformed frame is refused
and its connection closed, because a damaged frame cannot be resynchronised.

## Persistence

The store is a directory holding an append-only journal and an optional snapshot:

* `journal.sncf` — header (magic, format version, semantics revision, first
  sequence) followed by length-prefixed, CRC-32C checked canonical JSON records.
* `snapshot.sncf` — header plus a CRC-32C checked canonical JSON state document.

Recovery classifies the outcome explicitly: `fresh_store`, `clean_reopen`,
`torn_tail_recovered`, `corrupt_tail_discarded`, `snapshot_rejected`,
`incompatible_version_refused`, `semantics_mismatch_refused`,
`corrupt_refused`, `sequence_gap_refused`, `truncated_refused`. A recoverable
tail is truncated and accounted for; damage anywhere else refuses the store
instead of guessing. Compaction writes the snapshot first and rewrites the
journal second, so an interrupted compaction replays correctly from either side.

## Documentation

* `docs/ARCHITECTURE.md` — module layout, state machine, locking and shutdown order
* `docs/PERSISTENCE.md` — on-disk formats, commit semantics, recovery classification
* `docs/PROTOCOL.md` — framing, operations and refusal behaviour
* `docs/VALIDATION.md` — proof obligations mapped to tests, and REAL/SYNTHETIC/UNSUPPORTED

## Validation status

Debug and Release are built with `/W4 /WX /permissive-`; AddressSanitizer is run
where the toolchain supports it. The suite covers unit, property-based,
adversarial, concurrency, multi-process transport, and real crash-boundary
restart tests. Real hardware is not exercised: every device, capability report
and effect report in this repository is a labelled **SYNTHETIC** fixture.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
