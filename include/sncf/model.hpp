// Durable record types, bounded counters and the coordinator state container.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_MODEL_HPP
#define SNCF_MODEL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "sncf/authority.hpp"
#include "sncf/evidence.hpp"
#include "sncf/ids.hpp"
#include "sncf/lifecycle.hpp"
#include "sncf/time.hpp"

namespace sncf {

// --- registration records ---------------------------------------------------

struct SmartNicRecord {
  SmartNicId id;
  std::string label;
  std::uint32_t port_count = 0;
  TimestampNs registered_at;
};

struct DeviceRecord {
  DeviceId id;
  SmartNicId smartnic;
  DeviceModelId model;
  DeviceIncarnation incarnation;
  std::string serial;
  PortId port;
  bool present = false;
  CapabilityGeneration capability_generation;
  CompatibilityGeneration compatibility_generation;
  SemVer firmware;
  CapabilityMask capabilities;
  EvidenceId capability_evidence;
  Freshness capability_freshness = Freshness::Unknown;
  TimestampNs last_observed_at;
  TimestampNs registered_at;
};

struct ResourceDemand {
  std::uint32_t ports = 1;
  std::uint32_t queues = 1;
  std::uint64_t memory_bytes = 0;
};

struct PackageRecord {
  FunctionPackageId id;
  std::string name;
  SemVer version;
  Digest256 digest;
  std::vector<DeviceModelId> supported_models;  // sorted, unique
  CapabilityMask required_capabilities;
  CapabilityGeneration min_capability_generation;
  CompatibilityRange compatibility;
  SemVer min_firmware;
  ResourceDemand demand;
  bool exclusive_scope = true;
  LayoutRevision layout_revision;
  TimestampNs registered_at;
};

// --- evidence records -------------------------------------------------------

struct CapabilityEvidenceRecord {
  CapabilityEvidence evidence;
  RecordSequence sequence;
};

struct ObservationRecord {
  ObservationEvidence evidence;
  RecordSequence sequence;
};

// --- attempt and instance records ------------------------------------------

struct AttemptRecord {
  AttemptId id;
  AttemptPhase phase = AttemptPhase::Unknown;
  DeploymentGeneration deployment_generation;
  FencingToken token;
  LeaseId lease;
  FunctionPackageId package;
  TimestampNs staged_at;
  TimestampNs dispatched_at;
  TimestampNs settled_at;
  EffectOutcome outcome = EffectOutcome::Unknown;
  /// Digest of the enforcement-side report that settled this attempt. An
  /// applied or verified claim without a digest cannot exist.
  Digest256 effect_digest;
  bool effect_synthetic = false;
  ReasonCode terminal_reason = ReasonCode::Unknown;
  std::string detail;
};

struct InstanceRecord {
  FunctionInstanceId id;
  SmartNicId smartnic;
  DeviceId device;
  DeviceIncarnation incarnation;
  FunctionPackageId package;
  PortId port;
  QueueId queue;
  DesiredState desired = DesiredState::None;
  LifecycleState lifecycle = LifecycleState::Unknown;
  AuthorityState authority = AuthorityState::None;
  Freshness freshness = Freshness::Unknown;
  DeploymentGeneration deployment_generation;
  PolicyGeneration policy_generation;
  FencingToken token;
  LeaseId lease;
  PrincipalId holder;
  AttemptId current_attempt;
  AttemptId last_settled_attempt;
  std::vector<AttemptRecord> attempts;  // bounded history, ordered by attempt id
  FunctionPackageId previous_package;
  DeploymentGeneration previous_deployment_generation;
  /// True when a crash boundary left the last attempt's effect undetermined.
  /// The claim stays ambiguous until an execution-side report resolves it.
  bool pending_intent_ambiguous = false;
  ReasonCode last_reason = ReasonCode::Unknown;
  Digest256 last_effect_digest;
  EffectOutcome last_effect_outcome = EffectOutcome::Unknown;
  TimestampNs created_at;
  TimestampNs updated_at;

  [[nodiscard]] bool is_quiesced_or_withdrawn() const noexcept {
    return lifecycle == LifecycleState::Quiesced || lifecycle == LifecycleState::Withdrawn;
  }
};

// --- dedupe table -----------------------------------------------------------

struct DedupeEntry {
  CommandId command;
  PrincipalId principal;
  ReasonCode outcome = ReasonCode::Unknown;
  std::string subject;
  RecordSequence sequence;
};

// --- events -----------------------------------------------------------------

struct FabricEvent {
  EventSequence sequence;
  TimestampNs at;
  ReasonCode reason = ReasonCode::Unknown;
  std::string subject;
  std::string detail;
};

/// Per-reason tallies. Every refusal path records here, so a refusal can never
/// be silent.
class ReasonCounters {
 public:
  void record(ReasonCode code) noexcept;
  void clear() noexcept;
  [[nodiscard]] std::uint64_t count(ReasonCode code) const noexcept;
  [[nodiscard]] std::uint64_t total_acceptances() const noexcept;
  [[nodiscard]] std::uint64_t total_refusals() const noexcept;
  [[nodiscard]] Value to_value() const;

 private:
  std::vector<std::pair<ReasonCode, std::uint64_t>> counts_;  // sorted by code
};

/// Operational counters for bounded structures, truncations and evictions.
/// Each field names the structure whose bound it accounts for.
struct FabricCounters {
  std::uint64_t commands_considered = 0;
  std::uint64_t commands_accepted = 0;
  std::uint64_t commands_refused = 0;
  std::uint64_t commands_deduplicated = 0;

  std::uint64_t journal_records_appended = 0;
  std::uint64_t journal_records_replayed = 0;
  std::uint64_t journal_records_dropped_on_recovery = 0;
  std::uint64_t journal_compactions = 0;
  std::uint64_t journal_bytes_retained = 0;
  std::uint64_t journal_max_records = 0;

  std::uint64_t attempts_staged = 0;
  std::uint64_t attempts_settled = 0;
  std::uint64_t attempts_ambiguous = 0;
  std::uint64_t attempt_history_evictions = 0;

  std::uint64_t lease_evictions = 0;
  std::uint64_t leases_granted = 0;
  std::uint64_t leases_revoked = 0;
  std::uint64_t leases_expired = 0;
  std::uint64_t tokens_advanced_at_recovery = 0;

  std::uint64_t evidence_accepted = 0;
  std::uint64_t evidence_superseded = 0;
  std::uint64_t evidence_reverified = 0;
  std::uint64_t evidence_left_pending_reverification = 0;

  std::uint64_t events_emitted = 0;
  std::uint64_t events_evicted = 0;
  std::uint64_t dedupe_entries = 0;
  std::uint64_t dedupe_evictions = 0;
  std::uint64_t dedupe_floor_evictions = 0;
  std::uint64_t replayed_commands_fenced = 0;
  std::uint64_t history_entries = 0;
  std::uint64_t history_evictions = 0;

  std::uint64_t instances_created = 0;
  std::uint64_t instances_replaced = 0;
  std::uint64_t instances_withdrawn = 0;

  std::uint64_t decode_truncations = 0;
  std::uint64_t decode_oversized = 0;
  std::uint64_t decode_malformed = 0;
  std::uint64_t internal_apply_failures = 0;

  [[nodiscard]] Value to_value() const;
};

/// Bounds. Every one of these is enforced before allocation, and every
/// enforcement is visible in the counters and the event stream.
struct FabricConfig {
  std::size_t max_smartnics = 64;
  std::size_t max_devices = 256;
  std::size_t max_packages = 256;
  std::size_t max_instances = 512;
  std::size_t max_attempts_per_instance = 16;
  std::size_t max_events = 512;
  std::size_t max_history = 1024;
  std::size_t max_dedupe_entries = 512;
  std::size_t max_leases = 512;
  std::size_t max_concurrent_attempts_per_instance = 1;
  std::size_t max_record_bytes = 256U * 1024U;
  std::size_t max_journal_records = 4096;
  std::uint64_t max_journal_bytes = 8U * 1024U * 1024U;
  DurationNs lease_ttl = seconds(30);
  FreshnessPolicy freshness;
};

/// The coordinator state. All containers are sorted by their identity so that
/// iteration, export and replay are deterministic.
struct FabricState {
  CoordinatorEpoch epoch;
  BootId boot;
  PolicyGeneration policy_generation;
  RecordSequence last_sequence;

  std::uint64_t next_smartnic = 1;
  std::uint64_t next_device = 1;
  std::uint64_t next_package = 1;
  std::uint64_t next_instance = 1;
  std::uint64_t next_lease = 1;
  std::uint64_t next_evidence = 1;
  std::uint64_t next_event = 1;

  std::vector<SmartNicRecord> smartnics;
  std::vector<DeviceRecord> devices;
  std::vector<PackageRecord> packages;
  std::vector<InstanceRecord> instances;
  std::vector<CapabilityEvidenceRecord> capability_evidence;
  std::vector<ObservationRecord> observations;
  std::vector<Lease> leases;
  std::vector<DedupeEntry> dedupe;
  /// Highest command identity evicted from the idempotency window, per
  /// principal. A command at or below its principal floor can no longer be told
  /// apart from new work and is refused instead of re-executed.
  std::vector<std::pair<PrincipalId, CommandId>> dedupe_floor;
  std::vector<FabricEvent> events;
  std::vector<FabricEvent> history;
  FencingLedger fencing;
  ReasonCounters reasons;
  FabricCounters counters;

  [[nodiscard]] const SmartNicRecord* find_smartnic(SmartNicId id) const noexcept;
  [[nodiscard]] SmartNicRecord* find_smartnic(SmartNicId id) noexcept;
  [[nodiscard]] const DeviceRecord* find_device(DeviceId id) const noexcept;
  [[nodiscard]] DeviceRecord* find_device(DeviceId id) noexcept;
  [[nodiscard]] const PackageRecord* find_package(FunctionPackageId id) const noexcept;
  [[nodiscard]] PackageRecord* find_package(FunctionPackageId id) noexcept;
  [[nodiscard]] const InstanceRecord* find_instance(FunctionInstanceId id) const noexcept;
  [[nodiscard]] InstanceRecord* find_instance(FunctionInstanceId id) noexcept;
  [[nodiscard]] const CapabilityEvidenceRecord* find_capability_evidence(DeviceId id) const noexcept;
  [[nodiscard]] const ObservationRecord* find_observation(DeviceId id) const noexcept;
  [[nodiscard]] const Lease* find_lease(LeaseId id) const noexcept;
  [[nodiscard]] Lease* find_lease(LeaseId id) noexcept;

  /// Which instance currently owns a scope, if any live instance does.
  [[nodiscard]] FunctionInstanceId scope_owner(const ExclusiveScope& scope) const noexcept;
};

/// Serializes the full coordinator state. Used for snapshots and canonical
/// export; the decoder is the exact inverse for every correctness-critical field.
[[nodiscard]] Value state_to_value(const FabricState& state);

/// Decodes a state document. Every collection is bounded by the supplied
/// configuration and must be in canonical order; unsorted or oversized input is
/// refused rather than silently reordered or truncated.
[[nodiscard]] Result<void> state_from_value(const Value& value, FabricState& state,
                                            const FabricConfig& config) noexcept;

// Record codecs. Each pair is an exact inverse over every correctness-critical
// field; the decoders refuse missing, mistyped, out-of-range and
// non-canonically-ordered input instead of substituting defaults.
[[nodiscard]] Value instance_to_value(const InstanceRecord& record);
[[nodiscard]] Result<InstanceRecord> instance_from_value(const Value& value) noexcept;
[[nodiscard]] Value smartnic_to_value(const SmartNicRecord& record);
[[nodiscard]] Result<SmartNicRecord> smartnic_from_value(const Value& value) noexcept;
[[nodiscard]] Value device_to_value(const DeviceRecord& record);
[[nodiscard]] Result<DeviceRecord> device_from_value(const Value& value) noexcept;
[[nodiscard]] Value package_to_value(const PackageRecord& record);
[[nodiscard]] Result<PackageRecord> package_from_value(const Value& value) noexcept;
[[nodiscard]] Value attempt_to_value(const AttemptRecord& record);
[[nodiscard]] Result<AttemptRecord> attempt_from_value(const Value& value) noexcept;
[[nodiscard]] Value capability_evidence_to_value(const CapabilityEvidenceRecord& record);
[[nodiscard]] Result<CapabilityEvidenceRecord> capability_evidence_from_value(const Value& value) noexcept;
[[nodiscard]] Value observation_to_value(const ObservationRecord& record);
[[nodiscard]] Result<ObservationRecord> observation_from_value(const Value& value) noexcept;
[[nodiscard]] Value lease_to_value(const Lease& lease);
[[nodiscard]] Result<Lease> lease_from_value(const Value& value) noexcept;

}  // namespace sncf

#endif  // SNCF_MODEL_HPP
