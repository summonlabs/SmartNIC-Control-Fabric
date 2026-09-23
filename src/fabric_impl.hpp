// Internal coordinator implementation shared by the operation and recovery units.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_SRC_FABRIC_IMPL_HPP
#define SNCF_SRC_FABRIC_IMPL_HPP

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sncf/fabric.hpp"

namespace sncf {

/// Identifier bundle produced by a planning pass that allocates authority.
struct ActivationOutcome {
  FunctionInstanceId instance;
  AttemptId attempt;
  DeploymentGeneration generation;
  FencingToken token;
  LeaseId lease;
  ExclusiveScope scope;
  TimestampNs expires_at;
  bool creates = false;
  bool replaces = false;
};

/// A decision computed before anything durable happens. Either the whole plan
/// is applied or nothing is: there is no partially applied command.
struct Decision {
  bool accept = false;
  ReasonCode reason = ReasonCode::RefusedMalformedInput;
  std::string detail;
  std::string subject;
  std::vector<ExplanationFactor> factors;
  RecordKind kind = RecordKind::Unknown;
  Value::object_type body;
  /// Entity identifiers the operation returns to the caller.
  ActivationOutcome activation;
  /// Allocator high-water marks this decision advances. They are merged into a
  /// single allocator patch so that two allocations in one command can never
  /// collide in the canonical object encoding.
  std::vector<std::pair<std::string, std::uint64_t>> allocator_overrides;

  void override_allocator(std::string key, std::uint64_t value) {
    for (auto& entry : allocator_overrides) {
      if (entry.first == key) {
        entry.second = value;
        return;
      }
    }
    allocator_overrides.emplace_back(std::move(key), value);
  }

  /// Bounded structures that evicted an entry while forming this decision; the
  /// caller accounts for them once the decision is durable.
  std::uint64_t attempt_history_evictions = 0;
  std::uint64_t lease_evictions = 0;
  bool supersedes_evidence = false;

  void note(ReasonCode code, std::string subject_text, std::string detail_text) {
    factors.push_back(factor_of(code, std::move(subject_text), std::move(detail_text)));
  }
  void note_with_evidence(ReasonCode code, std::string subject_text, std::string detail_text,
                          const Digest256& digest) {
    factors.push_back(factor_with_evidence(code, std::move(subject_text), std::move(detail_text), digest));
  }
  void note_with_generation(ReasonCode code, std::string subject_text, std::string detail_text,
                            std::uint64_t generation) {
    factors.push_back(factor_with_generation(code, std::move(subject_text), std::move(detail_text), generation));
  }
  void refuse(ReasonCode code, std::string detail_text) {
    accept = false;
    reason = code;
    detail = std::move(detail_text);
    factors.push_back(factor_of(code, subject, detail));
  }
};

class Fabric::Impl {
 public:
  Impl(Fabric::Options options_in, std::unique_ptr<Journal> journal_in) noexcept;

  Fabric::Options options;
  std::shared_ptr<Clock> clock;
  std::unique_ptr<Journal> journal;
  FabricState state;
  RecoveryReport recovery;
  mutable std::mutex mutex;
  bool running = true;
  bool has_last_decision = false;
  Explanation last_decision;

  // --- shared plumbing (called with mutex held) -----------------------------
  [[nodiscard]] TimestampNs now() const noexcept;
  void emit_event(ReasonCode reason, std::string subject, std::string detail) noexcept;
  void record_reason(ReasonCode reason) noexcept;
  void remember_decision(const Explanation& explanation) noexcept;
  [[nodiscard]] Explanation make_explanation(const Decision& decision, std::string operation,
                                             TimestampNs at, CoordinatorEpoch epoch) const;
  /// Records a refusal: the decision is final, so it is counted and emitted
  /// immediately.
  void note_refusal(const Decision& decision, std::string_view operation) noexcept;

  /// Journals and applies an accepted decision. Statistics, the event and the
  /// remembered explanation are recorded only after the decision is durable, so
  /// a failed commit is never accounted as an acceptance.
  [[nodiscard]] Result<RecordSequence> commit(const Decision& decision, const CommandHeader& header,
                                              ReasonCode outcome, std::string_view operation) noexcept;
  [[nodiscard]] Result<void> apply(const JournalRecord& record) noexcept;
  /// How a command identity relates to the retained idempotency window.
  enum class DedupeState : std::uint8_t { New = 0, Replay = 1, BelowFloor = 2 };

  [[nodiscard]] DedupeState dedupe_state(const CommandHeader& header, ReasonCode& outcome,
                                         std::string& subject) const noexcept;
  [[nodiscard]] bool dedupe_lookup(const CommandHeader& header, ReasonCode& outcome,
                                   std::string& subject) const noexcept;
  [[nodiscard]] std::optional<CommandId> dedupe_floor_for(PrincipalId principal) const noexcept;
  void raise_dedupe_floor(PrincipalId principal, CommandId command) noexcept;
  void push_dedupe(const JournalRecord& record) noexcept;
  void push_history(const JournalRecord& record) noexcept;
  [[nodiscard]] std::optional<CommandOutcome> dedupe_outcome(const CommandHeader& header) const noexcept;

  // --- recovery -------------------------------------------------------------
  [[nodiscard]] Result<void> initialize() noexcept;
  [[nodiscard]] Result<void> load_snapshot_state() noexcept;
  [[nodiscard]] Result<void> replay() noexcept;
  [[nodiscard]] Result<void> conservative_recovery() noexcept;

  // --- operations -----------------------------------------------------------
  [[nodiscard]] Result<SmartNicRegistration> register_smartnic(RegisterSmartNicRequest request) noexcept;
  [[nodiscard]] Result<DeviceRegistration> register_device(RegisterDeviceRequest request) noexcept;
  [[nodiscard]] Result<PackageRegistration> register_package(RegisterPackageRequest request) noexcept;
  [[nodiscard]] Result<EvidenceAcceptance> submit_capability_evidence(
      CapabilityEvidenceRequest request) noexcept;
  [[nodiscard]] Result<EvidenceAcceptance> submit_observation(ObservationRequest request) noexcept;
  [[nodiscard]] Result<EvidenceAcceptance> reverify_evidence(ReverifyEvidenceRequest request) noexcept;
  [[nodiscard]] Result<AuthorityGrant> acquire_authority(AcquireAuthorityRequest request) noexcept;
  [[nodiscard]] Result<CommandOutcome> release_authority(ReleaseAuthorityRequest request) noexcept;
  [[nodiscard]] Result<DeploymentPlan> plan_deployment(const PlanRequest& request) const noexcept;
  [[nodiscard]] Result<ActivationGrant> activate(ActivateRequest request) noexcept;
  [[nodiscard]] Result<CommandOutcome> acknowledge(AcknowledgeRequest request) noexcept;
  [[nodiscard]] Result<CommandOutcome> report_effect(EffectReportRequest request) noexcept;
  [[nodiscard]] Result<IntentAcceptance> request_quiesce(QuiesceRequest request) noexcept;
  [[nodiscard]] Result<IntentAcceptance> request_withdraw(WithdrawRequest request) noexcept;
  [[nodiscard]] Result<IntentAcceptance> request_rollback(RollbackRequest request) noexcept;

  // --- decisions ------------------------------------------------------------
  [[nodiscard]] Decision decide_register_smartnic(const RegisterSmartNicRequest& request) const;
  [[nodiscard]] Decision decide_register_device(const RegisterDeviceRequest& request) const;
  [[nodiscard]] Decision decide_register_package(const RegisterPackageRequest& request) const;
  [[nodiscard]] Decision decide_capability_evidence(const CapabilityEvidenceRequest& request) const;
  [[nodiscard]] Decision decide_observation(const ObservationRequest& request) const;
  [[nodiscard]] Decision decide_reverify(const ReverifyEvidenceRequest& request) const;
  [[nodiscard]] Decision decide_acquire(const AcquireAuthorityRequest& request) const;
  [[nodiscard]] Decision decide_release(const ReleaseAuthorityRequest& request) const;
  [[nodiscard]] Decision decide_activate(const ActivateRequest& request, bool for_plan) const;
  [[nodiscard]] Decision decide_acknowledge(const AcknowledgeRequest& request) const;
  [[nodiscard]] Decision decide_effect(const EffectReportRequest& request) const;
  [[nodiscard]] Decision decide_intent(FunctionInstanceId instance, const AuthorityClaim& claim,
                                       DesiredState desired, ReasonCode reason, bool rollback) const;
  [[nodiscard]] DeploymentPlan plan_from_decision(const PlanRequest& request, const Decision& decision) const;

  /// Deterministically removes revoked or expired lease rows until the lease
  /// table is inside its bound, recording the removals as a patch. Returns false
  /// when the table cannot be brought inside the bound.
  [[nodiscard]] bool prune_leases(Decision& decision) const;

  [[nodiscard]] Result<void> check_authority(const AuthorityClaim& claim, const InstanceRecord& instance,
                                             Decision& decision, bool require_authority) const;
  [[nodiscard]] Result<void> check_compatibility(const InstanceRecord& instance, const PackageRecord& package,
                                                 Decision& decision) const;
  [[nodiscard]] const CapabilityEvidenceRecord* live_capability_evidence(const DeviceRecord& device,
                                                                        Decision& decision) const;

  [[nodiscard]] Result<RecordSequence> compact_locked() noexcept;
  [[nodiscard]] Value export_value_locked() const;
};

/// Encodes the allocator high-water marks so that replay restores them exactly.
[[nodiscard]] Value allocator_value(const FabricState& state);

/// Allocator snapshot with one counter advanced. Decisions carry this so that a
/// replay restores the exact identity high-water marks the live path used.
[[nodiscard]] Value::object_type allocator_advanced(const FabricState& state, std::string_view key,
                                                    std::uint64_t value);
[[nodiscard]] Result<void> apply_allocator(const Value& value, FabricState& state) noexcept;

/// Parses "kind:id" subjects produced by the subject_of helpers.
[[nodiscard]] std::optional<std::uint64_t> parse_subject_id(std::string_view subject) noexcept;
[[nodiscard]] bool subject_has_prefix(std::string_view subject, std::string_view prefix) noexcept;

}  // namespace sncf

#endif  // SNCF_SRC_FABRIC_IMPL_HPP
