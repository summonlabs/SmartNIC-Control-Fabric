// Public runtime API for the SmartNIC Control Fabric.
//
// Boundary: this runtime owns control-plane state for programmable NIC
// functions - registration, compatibility gating, deployment intent, activation
// authority, quiescence, replacement, withdrawal and observed/verified effect.
// It does not implement firmware, packet processing, vendor SDKs, route
// computation, host networking or telemetry collection, and it consumes
// topology, observation, capability, policy, authority and effect evidence from
// adjacent runtimes without re-implementing them.
//
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_FABRIC_HPP
#define SNCF_FABRIC_HPP

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sncf/authority.hpp"
#include "sncf/evidence.hpp"
#include "sncf/explain.hpp"
#include "sncf/model.hpp"
#include "sncf/persistence.hpp"

namespace sncf {

/// Every mutating call carries a caller-chosen command identity. Re-delivering
/// the same command from the same principal returns the recorded outcome and
/// changes nothing.
struct CommandHeader {
  CommandId command;
  PrincipalId principal;
};

// --- registration requests --------------------------------------------------

struct RegisterSmartNicRequest {
  CommandHeader header;
  std::string label;
  std::uint32_t port_count = 0;
};

struct RegisterDeviceRequest {
  CommandHeader header;
  SmartNicId smartnic;
  DeviceModelId model;
  DeviceIncarnation incarnation;
  std::string serial;
  PortId port;
};

struct RegisterPackageRequest {
  CommandHeader header;
  std::string name;
  SemVer version;
  Digest256 digest;
  std::vector<DeviceModelId> supported_models;
  std::vector<CapabilityCode> required_capabilities;
  CapabilityGeneration min_capability_generation;
  CompatibilityRange compatibility;
  SemVer min_firmware;
  ResourceDemand demand;
  bool exclusive_scope = true;
  LayoutRevision layout_revision;
};

// --- evidence requests ------------------------------------------------------

struct CapabilityEvidenceRequest {
  CommandHeader header;
  EvidenceSource source = EvidenceSource::Unknown;
  Digest256 payload_digest;
  TimestampNs observed_at;
  bool synthetic = false;
  SmartNicId smartnic;
  DeviceId device;
  DeviceIncarnation incarnation;
  DeviceModelId model;
  CapabilityGeneration capability_generation;
  CompatibilityGeneration compatibility_generation;
  SemVer firmware;
  std::vector<CapabilityCode> capabilities;
};

struct ObservationRequest {
  CommandHeader header;
  EvidenceSource source = EvidenceSource::Unknown;
  Digest256 payload_digest;
  TimestampNs observed_at;
  bool synthetic = false;
  SmartNicId smartnic;
  DeviceId device;
  DeviceIncarnation incarnation;
  bool present = false;
  CapabilityGeneration capability_generation;
  CompatibilityGeneration compatibility_generation;
};

/// Revalidates persisted evidence under the live epoch, using a fresh reading
/// from the same source. Without this a restarted coordinator refuses to act on
/// the evidence it loaded.
struct ReverifyEvidenceRequest {
  CommandHeader header;
  EvidenceSource source = EvidenceSource::Unknown;
  Digest256 payload_digest;
  TimestampNs observed_at;
  DeviceId device;
  DeviceIncarnation incarnation;
};

// --- authority requests -----------------------------------------------------

struct AcquireAuthorityRequest {
  CommandHeader header;
  FunctionInstanceId instance;
  ExclusiveScope scope;
  DurationNs ttl;
};

struct ReleaseAuthorityRequest {
  CommandHeader header;
  FunctionInstanceId instance;
  LeaseId lease;
  FencingToken token;
};

// --- planning and activation ------------------------------------------------

struct PlanRequest {
  SmartNicId smartnic;
  DeviceId device;
  DeviceIncarnation incarnation;
  FunctionPackageId package;
  PortId port;
  QueueId queue;
  FunctionInstanceId instance;
  PolicyGeneration policy_generation;
};

enum class PlanVerdict : std::uint8_t {
  Accepted = 0,
  Refused = 1,
  CreatesInstance = 2,
};

struct PlanStep {
  ReasonCode reason = ReasonCode::Unknown;
  std::string detail;
  bool has_evidence = false;
  Digest256 evidence_digest;
  bool has_generation = false;
  std::uint64_t generation = 0;
};

struct DeploymentPlan {
  PlanVerdict verdict = PlanVerdict::Refused;
  ReasonCode primary_reason = ReasonCode::Unknown;
  std::string detail;
  FunctionInstanceId instance;
  bool creates_instance = false;
  bool replaces_instance = false;
  DeploymentGeneration generation;
  DeploymentGeneration previous_generation;
  FunctionPackageId package;
  FunctionPackageId previous_package;
  ExclusiveScope scope;
  FencingToken required_token;
  std::vector<PlanStep> steps;

  [[nodiscard]] bool accepted() const noexcept {
    return verdict == PlanVerdict::Accepted || verdict == PlanVerdict::CreatesInstance;
  }
  [[nodiscard]] Value to_value() const;
};

struct ActivateRequest {
  CommandHeader header;
  SmartNicId smartnic;
  DeviceId device;
  DeviceIncarnation incarnation;
  FunctionPackageId package;
  PortId port;
  QueueId queue;
  /// Nil to create a new instance; otherwise an existing instance that must
  /// already hold live authority.
  FunctionInstanceId instance;
  AuthorityClaim claim;
  PolicyGeneration policy_generation;
  DurationNs lease_ttl;
};

struct AcknowledgeRequest {
  CommandHeader header;
  FunctionInstanceId instance;
  AttemptId attempt;
  DeploymentGeneration generation;
  FencingToken token;
  EvidenceSource source = EvidenceSource::Unknown;
  TimestampNs observed_at;
};

struct EffectReportRequest {
  CommandHeader header;
  FunctionInstanceId instance;
  AttemptId attempt;
  DeploymentGeneration generation;
  FencingToken token;
  EffectOutcome outcome = EffectOutcome::Unknown;
  EvidenceSource source = EvidenceSource::Unknown;
  Digest256 payload_digest;
  TimestampNs observed_at;
  bool synthetic = false;
  std::string detail;
};

struct QuiesceRequest {
  CommandHeader header;
  FunctionInstanceId instance;
  AuthorityClaim claim;
  ReasonCode reason = ReasonCode::Accepted;
};

struct WithdrawRequest {
  CommandHeader header;
  FunctionInstanceId instance;
  AuthorityClaim claim;
  ReasonCode reason = ReasonCode::Accepted;
};

struct RollbackRequest {
  CommandHeader header;
  FunctionInstanceId instance;
  AuthorityClaim claim;
  DurationNs lease_ttl;
};

// --- results ----------------------------------------------------------------

struct CommandOutcome {
  ReasonCode reason = ReasonCode::Accepted;
  RecordSequence sequence;
  bool duplicate = false;

  [[nodiscard]] bool accepted() const noexcept { return is_acceptance(reason); }
};

struct SmartNicRegistration {
  CommandOutcome outcome;
  SmartNicId id;
};

struct DeviceRegistration {
  CommandOutcome outcome;
  DeviceId id;
};

struct PackageRegistration {
  CommandOutcome outcome;
  FunctionPackageId id;
};

struct EvidenceAcceptance {
  CommandOutcome outcome;
  EvidenceId id;
  Freshness freshness = Freshness::Unknown;
};

struct AuthorityGrant {
  CommandOutcome outcome;
  LeaseId lease;
  FencingToken token;
  TimestampNs expires_at;
};

struct ActivationGrant {
  CommandOutcome outcome;
  FunctionInstanceId instance;
  AttemptId attempt;
  DeploymentGeneration generation;
  FencingToken token;
  LeaseId lease;
  ExclusiveScope scope;
};

struct IntentAcceptance {
  CommandOutcome outcome;
  DesiredState desired = DesiredState::None;
  LifecycleState lifecycle = LifecycleState::Unknown;
  AuthorityState authority = AuthorityState::None;
};

// --- runtime ----------------------------------------------------------------

class Fabric {
 public:
  struct Options {
    FabricConfig config;
    std::filesystem::path store_directory;
    std::shared_ptr<Clock> clock;
    bool sync_on_commit = true;
    /// Opens the store for inspection only. No recovery record is written and
    /// every mutating call is refused, so a diagnostic cannot revoke the
    /// authority of a coordinator that owns the store.
    bool read_only = false;
  };

  /// Opens (or creates) the durable store, replays accepted facts and applies
  /// the conservative recovery pass. A store that fails integrity validation is
  /// refused: the caller never receives a Fabric backed by damaged state.
  [[nodiscard]] static Result<std::unique_ptr<Fabric>> open(Options options) noexcept;

  ~Fabric();
  Fabric(const Fabric&) = delete;
  Fabric& operator=(const Fabric&) = delete;

  // Registration.
  [[nodiscard]] Result<SmartNicRegistration> register_smartnic(RegisterSmartNicRequest request) noexcept;
  [[nodiscard]] Result<DeviceRegistration> register_device(RegisterDeviceRequest request) noexcept;
  [[nodiscard]] Result<PackageRegistration> register_package(RegisterPackageRequest request) noexcept;

  // Evidence.
  [[nodiscard]] Result<EvidenceAcceptance> submit_capability_evidence(
      CapabilityEvidenceRequest request) noexcept;
  [[nodiscard]] Result<EvidenceAcceptance> submit_observation(ObservationRequest request) noexcept;
  [[nodiscard]] Result<EvidenceAcceptance> reverify_evidence(ReverifyEvidenceRequest request) noexcept;

  // Authority.
  [[nodiscard]] Result<AuthorityGrant> acquire_authority(AcquireAuthorityRequest request) noexcept;
  [[nodiscard]] Result<CommandOutcome> release_authority(ReleaseAuthorityRequest request) noexcept;

  // Planning and activation.
  [[nodiscard]] Result<DeploymentPlan> plan_deployment(const PlanRequest& request) const noexcept;
  [[nodiscard]] Result<ActivationGrant> activate(ActivateRequest request) noexcept;
  [[nodiscard]] Result<CommandOutcome> acknowledge(AcknowledgeRequest request) noexcept;
  [[nodiscard]] Result<CommandOutcome> report_effect(EffectReportRequest request) noexcept;
  [[nodiscard]] Result<IntentAcceptance> request_quiesce(QuiesceRequest request) noexcept;
  [[nodiscard]] Result<IntentAcceptance> request_withdraw(WithdrawRequest request) noexcept;
  [[nodiscard]] Result<IntentAcceptance> request_rollback(RollbackRequest request) noexcept;

  // Inspection.
  [[nodiscard]] Result<InstanceRecord> inspect_instance(FunctionInstanceId instance) const noexcept;
  [[nodiscard]] Result<Explanation> explain_instance(FunctionInstanceId instance) const noexcept;
  [[nodiscard]] Result<Explanation> explain_last_decision() const noexcept;
  [[nodiscard]] std::vector<FabricEvent> events() const noexcept;
  [[nodiscard]] std::vector<FabricEvent> history() const noexcept;
  [[nodiscard]] FabricCounters counters() const noexcept;
  [[nodiscard]] ReasonCounters reasons() const noexcept;
  [[nodiscard]] RecoveryReport recovery() const noexcept;
  [[nodiscard]] CoordinatorEpoch epoch() const noexcept;
  [[nodiscard]] BootId boot() const noexcept;
  [[nodiscard]] const FabricConfig& config() const noexcept;

  /// Canonical machine-readable export. Deterministic: the same durable state
  /// always encodes to identical bytes.
  [[nodiscard]] Value export_value() const;
  [[nodiscard]] std::string export_canonical() const;
  /// Digest of the durable portion alone, which is what a restart round-trip
  /// must preserve exactly.
  [[nodiscard]] Digest256 durable_digest() const;

  /// Forces a snapshot and journal rewrite. Returns the sequence the snapshot
  /// covers.
  [[nodiscard]] Result<RecordSequence> compact() noexcept;

  /// Idempotent shutdown. After it returns, every mutating call is refused with
  /// RefusedShuttingDown and the store is closed.
  void shutdown() noexcept;
  [[nodiscard]] bool is_running() const noexcept;

 private:
  class Impl;
  explicit Fabric(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

/// Canonical subject key for an entity, e.g. "instance:7". Stable across
/// releases and used by dedupe, history and explanations.
[[nodiscard]] std::string subject_of(FunctionInstanceId id);
[[nodiscard]] std::string subject_of(DeviceId id);
[[nodiscard]] std::string subject_of(SmartNicId id);
[[nodiscard]] std::string subject_of(FunctionPackageId id);
[[nodiscard]] std::string subject_of(LeaseId id);

}  // namespace sncf

#endif  // SNCF_FABRIC_HPP
