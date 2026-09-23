// Evidence envelopes: provenance, generations, freshness and payloads supplied
// by adjacent runtimes. This runtime consumes evidence and never fabricates it.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_EVIDENCE_HPP
#define SNCF_EVIDENCE_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "sncf/ids.hpp"
#include "sncf/lifecycle.hpp"
#include "sncf/time.hpp"

namespace sncf {

/// Which adjacent runtime produced a piece of evidence. Provenance is part of
/// the accepted record: evidence for one claim from the wrong source is refused
/// rather than reinterpreted.
enum class EvidenceSource : std::uint8_t {
  Unknown = 0,
  TopologyRuntime = 1,
  DeviceObservationRuntime = 2,
  CapabilityEvidenceProvider = 3,
  PolicyRuntime = 4,
  AuthorityRuntime = 5,
  ExecutionRuntime = 6,
  Operator = 7,
  SyntheticFixture = 8,
};

enum class EvidenceKind : std::uint8_t {
  Unknown = 0,
  CapabilityReport = 1,
  DeviceObservation = 2,
  ExecutionEffectReport = 3,
  QuiesceConfirmation = 4,
  RemovalConfirmation = 5,
};

[[nodiscard]] std::string_view evidence_source_name(EvidenceSource source) noexcept;
[[nodiscard]] EvidenceSource evidence_source_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view evidence_kind_name(EvidenceKind kind) noexcept;
[[nodiscard]] EvidenceKind evidence_kind_from_name(std::string_view name) noexcept;

/// The provenance fields every piece of evidence must carry. Nothing here has a
/// default that would let absent data look like a valid report.
struct EvidenceEnvelope {
  EvidenceId id;
  EvidenceKind kind = EvidenceKind::Unknown;
  EvidenceSource source = EvidenceSource::Unknown;
  Digest256 payload_digest;
  TimestampNs observed_at;
  TimestampNs accepted_at;
  CoordinatorEpoch accepted_epoch;
  PolicyGeneration policy_generation;
  Freshness freshness = Freshness::Unknown;
  /// True when the report was produced by a synthetic fixture rather than a real
  /// device or runtime. Carried through to the export so claims stay labelled.
  bool synthetic = false;
};

/// Capability evidence for one device incarnation, produced by a capability
/// evidence provider (or a labelled synthetic fixture).
struct CapabilityEvidence {
  EvidenceEnvelope envelope;
  SmartNicId smartnic;
  DeviceId device;
  DeviceIncarnation incarnation;
  DeviceModelId model;
  CapabilityGeneration capability_generation;
  CompatibilityGeneration compatibility_generation;
  SemVer firmware;
  CapabilityMask capabilities;
};

/// Observation of device liveness/presence for one incarnation.
struct ObservationEvidence {
  EvidenceEnvelope envelope;
  SmartNicId smartnic;
  DeviceId device;
  DeviceIncarnation incarnation;
  bool present = false;
  CapabilityGeneration capability_generation;
  CompatibilityGeneration compatibility_generation;
};

/// Effect report from an execution runtime. This is the only input that can move
/// an attempt to applied, verified, quiesced, removed or failed.
struct EffectEvidence {
  EvidenceEnvelope envelope;
  FunctionInstanceId instance;
  AttemptId attempt;
  DeploymentGeneration deployment_generation;
  FencingToken token;
  EffectOutcome outcome = EffectOutcome::Unknown;
  /// Bounded operator-facing label from the executor; never parsed for meaning.
  std::string detail;
};

/// Freshness policy. All comparisons are against an explicit clock reading and
/// explicit generations; there is no implicit "recent enough" test.
struct FreshnessPolicy {
  DurationNs capability_max_age = seconds(300);
  DurationNs observation_max_age = seconds(60);
  DurationNs effect_max_age = seconds(300);
};

/// Evaluates freshness for a newly ingested item. Future-dated observations are
/// invalid (RefusedInvalidRange) rather than fresh.
[[nodiscard]] Result<Freshness> evaluate_freshness(TimestampNs observed_at, TimestampNs now,
                                                   DurationNs max_age) noexcept;

/// Re-evaluates a persisted envelope under the live epoch. Persisted evidence
/// never returns to Fresh here: it becomes PendingReverification until the
/// source confirms it again, and it is Stale when its generation was superseded.
[[nodiscard]] Freshness refresh_after_restart(Freshness persisted, CoordinatorEpoch persisted_epoch,
                                              CoordinatorEpoch live_epoch) noexcept;

}  // namespace sncf

#endif  // SNCF_EVIDENCE_HPP
