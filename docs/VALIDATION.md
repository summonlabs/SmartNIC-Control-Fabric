# Validation

## How to reproduce

```sh
# Debug
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure

# Release
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
build/release/bin/sncf_tests

# AddressSanitizer
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSNCF_ENABLE_ASAN=ON
cmake --build build/asan
build/asan/bin/sncf_tests

# Installed package plus independent consumer
cmake --install build/release --prefix build/install
cmake -S examples/downstream -B build/downstream -G Ninja -DCMAKE_PREFIX_PATH=build/install
cmake --build build/downstream
build/downstream/consumer.exe <scratch-store>
```

There are no test timeouts anywhere. Every test synchronises on completed work,
so a hang is a defect rather than a skipped test.

## Proof obligations mapped to tests

| Obligation | Where it is proved |
| --- | --- |
| One authoritative instance per exclusive scope | `fabric_authority_scope_is_exclusive`, `transport_proves_multiprocess_operation_over_real_sockets` (two OS processes contend for one scope) |
| Stale device/function incarnations are fenced | `fabric_registration_and_duplicate_delivery`, `fabric_evidence_incarnation_replay_is_stale`, `fabric_authority_stale_token_is_fenced` |
| Incompatible functions never activate | `fabric_evidence_gating_is_exact` (five independent gates), `adversarial_oversized_inputs_are_refused` |
| Restart never resurrects liveness or authority | `fabric_restart_fences_authority_and_advances_epoch`, `fabric_repeated_restart_is_monotonic_and_never_resurrects_authority`, `restart_child_process_is_killed_at_every_durable_boundary` |
| Quiesced/revoked authority cannot mutate | `fabric_quiesce_and_withdraw_revoke_authority_immediately`, `fabric_authority_lease_holder_is_exclusive` |
| Ambiguous crash boundaries stay ambiguous until reverified | `restart_child_process_is_killed_at_every_durable_boundary`, `restart_ambiguity_is_resolved_only_by_enforcement_evidence` |
| Applied/verified claims need enforcement-side evidence | `fabric_effect_reports_require_matching_evidence`, `fabric_acknowledgement_is_not_application` |
| Accepted state is deterministic from accepted evidence | `property_scripted_state_is_deterministic`, `export_is_byte_stable_for_identical_operation_streams` |
| Stale or superseded evidence cannot justify authority | `fabric_evidence_expiry_and_future_dating`, `fabric_evidence_survives_restart_only_as_pending`, `fabric_evidence_gating_is_exact` |
| Missing evidence never becomes false/zero/success | `fabric_evidence_absence_is_not_success`, `canonical_field_helpers_distinguish_absence_from_zero`, `adversarial_corrupt_state_documents_are_refused` |
| Duplicate delivery is idempotent or explicitly fenced | `fabric_registration_and_duplicate_delivery`, `concurrency_duplicate_delivery_applies_once`, `adversarial_replayed_command_with_a_different_payload_is_stable` |
| Bounded truncation/refusal/eviction is observable | `concurrency_bounded_structures_evict_and_account`, `persistence_bounds_growth_and_refuses_when_full`, `persistence_rejects_oversized_records`, `transport_connection_bound_is_enforced` |
| Persistence round-trips correctness-critical state | `persistence_snapshot_round_trip_and_compaction`, `property_snapshot_and_replay_preserve_every_field`, `persistence_record_document_round_trip` |
| Conservative restart does not resurrect liveness | `property_reopening_a_quiet_store_is_stable`, `fabric_repeated_restart_is_monotonic_and_never_resurrects_authority` |
| Malformed/corrupt/truncated/oversized input cannot look successful | `canonical_rejects_*`, `persistence_refuses_*`, `transport_malformed_frames_are_refused_without_state_change`, `transport_truncated_frame_never_yields_a_success` |

## Test inventory

| File | Kind |
| --- | --- |
| `test_digest.cpp` | unit: SHA-256 and CRC-32C known vectors, streaming equality, strict hex |
| `test_canonical.cpp` | unit + adversarial: canonical encoding, strict parsing, bounds |
| `test_lifecycle.cpp` | unit: state machines, reason tables, checked arithmetic |
| `test_persistence.cpp` | integration: framing, torn tail, corruption, versions, snapshots |
| `test_fabric_lifecycle.cpp` | end-to-end: registration through verification, replacement, rollback |
| `test_fabric_authority.cpp` | end-to-end: scope exclusivity, leases, fencing, restart |
| `test_fabric_evidence.cpp` | end-to-end: gating, provenance, freshness, revalidation, dry runs |
| `test_property.cpp` | property: seeded generation, determinism, snapshot equivalence |
| `test_adversarial.cpp` | adversarial: integer extremes, oversized input, collisions |
| `test_restart.cpp` | restart: real child processes killed at durable boundaries |
| `test_concurrency.cpp` | concurrency: parallel ingest, duplicate delivery, bounded structures |
| `test_transport.cpp` | transport: framing, overload, malformed frames, multi-process over real sockets |
| `test_explain_export.cpp` | deterministic explanations and canonical export |
| `test_benchmark.cpp` | benchmarks that measure completed work and assert completion |
| `test_hardening.cpp` | regressions for every defect the adversarial audit found |

## Crash boundaries exercised

The durability tests spawn a real child process of the test binary and terminate
it at an exact boundary:

| Boundary | Verified outcome |
| --- | --- |
| `journal_before_write` | the decision left no trace; no instance exists |
| `journal_after_write_before_sync` | the record survived a process kill; the attempt is ambiguous, never verified |
| `journal_after_sync_before_ack` | durable but unacknowledged; ambiguous until an enforcement-side report |
| hard exit after commit | ambiguous, never fabricated as success |
| `snapshot_before_rename` | stale snapshot plus complete journal replays identically |
| `snapshot_after_rename_before_journal_rewrite` | snapshot at *S* plus a full journal replays identically |
| `shutdown_before_close` | the store reopens with its committed state |

## Adversarial audit and hardening

After the first fully green run the implementation was attacked deliberately, by
an independent read-only review and by hand. Every reproducible material defect
was fixed and locked in with a regression test in `test_hardening.cpp`:

| Defect | Fix | Regression test |
| --- | --- | --- |
| A failed append left a torn frame on disk and reused the sequence, which made the store unopenable or stranded later records behind mid-file damage | The append records its start offset and rolls the file back on any failure after the write begins; if the rollback fails the journal is marked unusable | `hardening_failed_append_leaves_no_trace` |
| A journal starting past sequence 1 was accepted when its snapshot was missing or damaged, silently replaying only a suffix of history | Recovery now requires the journal to start at 1 or exactly where a present snapshot ends, and refuses otherwise | `hardening_journal_without_its_snapshot_prefix_is_refused` |
| The live lease table was unbounded while the snapshot decoder bounded it, so compaction could write a store this build refused to open | Grants prune revoked/expired rows deterministically and refuse at the bound; removals are journaled patches | `hardening_lease_table_stays_inside_its_bound` |
| Re-registering a device with a higher incarnation revoked only one of its leases, leaving a live lease on a stale scope | All of the device's live leases are revoked as one array patch | `hardening_device_reincarnation_revokes_every_lease_of_the_device` |
| `ServerStats` was written under two different mutexes, racing with `Server::stats()` | All statistics live in one lock domain | covered by `transport_serves_concurrent_clients_from_several_threads` |
| Every `sncf` inspection command ran the recovery pass, so `sncf inspect` revoked a live coordinator's authority | Read-only store mode: no recovery record is written and every mutation is refused | `hardening_read_only_open_changes_nothing` |
| A redelivered command older than the bounded idempotency window was re-executed as new work | Per-principal durable replay floor; an out-of-window replay is refused | `hardening_replay_outside_the_window_is_fenced_not_re_executed` |
| A device's durability bit survived a restart and never aged out, so a vanished device could still be activated | Restart invalidates presence, and the observation window is evaluated at decision time | `hardening_liveness_is_re_observed_after_restart`, `hardening_observation_freshness_is_evaluated_at_decision_time` |
| A decision was counted as accepted before it was durable, and `sncf verify` reported zero dropped records | Accounting follows a successful commit; discarded frames are counted | `persistence_recovers_a_torn_tail`, `export_reflects_refusals_in_the_reason_counters` |

Areas the audit examined and found sound, and which the suite already covers:
the canonical parser's bounds and UTF-8 validation, frame and request decoding
bounds, the single-mutex coordinator with no re-entrant public entry point, the
transport shutdown ordering (no lock held across a join, every wait reachable by
an explicit wakeup), and the rule that applied/verified claims exist only when an
enforcement-side report produced them.

## REAL / SYNTHETIC / UNSUPPORTED

**REAL** — exercised by this repository on the stated platform:

* The C++20 runtime, its persistence, its protocol, and its CLI as built artifacts.
* Real operating-system processes (a spawned child of the test binary and the
  `sncf` server/client) communicating over real TCP sockets on the loopback
  interface.
* Real process termination at durable boundaries (`std::_Exit`, no unwinding).
* Real file system effects: torn tails, truncated headers, flipped bytes, damage
  in the middle of a journal, snapshot damage, and directory reopens.
* Real concurrency: multiple threads and multiple processes contending for the
  same coordinator, exclusively and simultaneously.
* Real durable-state damage: torn tails, truncated headers, flipped bytes,
  mid-journal corruption, damaged snapshots, and a deleted snapshot.
* Real injected write failure: a partial journal write followed by a failure,
  which is how the append rollback is proved.

**SYNTHETIC** — every device-facing input in this repository is a labelled
fixture, and it is labelled everywhere it surfaces:

* SmartNICs, physical devices, device models, incarnations, ports and serial
  numbers are synthetic registrations.
* Capability reports and device observations come from
  `EvidenceSource::SyntheticFixture` with `synthetic = true`; the flag is stored
  in the evidence envelope, persisted, exported, and carried into attempt records
  as `effect_synthetic`.
* Effect reports are synthetic executions: no firmware is loaded, no packet is
  processed, no vendor SDK is called.

**UNSUPPORTED / NOT CLAIMED** — deliberately outside this repository:

* No switch, ASIC, NIC, SmartNIC, DPU, RDMA, InfiniBand, NVLink, GPU, CUDA,
  multi-host or vendor-protocol behaviour is exercised or claimed.
* No firmware, packet processing, route computation, host networking or
  telemetry collection is implemented.
* No distributed consensus, cluster membership or leader election is
  implemented: the coordinator is a single process with a durable epoch and boot
  identity.
* AddressSanitizer coverage is reported only for the toolchain configuration in
  which it actually ran.
