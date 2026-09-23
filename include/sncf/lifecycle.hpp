// Explicit lifecycle, intent, authority, freshness and attempt state model.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_LIFECYCLE_HPP
#define SNCF_LIFECYCLE_HPP

#include <cstdint>
#include <string_view>

#include "sncf/ids.hpp"

namespace sncf {

/// Recorded deployment intent. Intent is not authority and is not effect.
enum class DesiredState : std::uint8_t {
  None = 0,
  Deployed = 1,
  Active = 2,
  Quiesced = 3,
  Withdrawn = 4,
};

/// The lifecycle classification of a function instance. The runtime never
/// collapses these: "authorized" is not "applied", "applied" is not "verified".
enum class LifecycleState : std::uint8_t {
  Unknown = 0,
  Desired = 1,
  Eligible = 2,
  Authorized = 3,
  Acknowledged = 4,
  Applied = 5,
  Verified = 6,
  Quiesced = 7,
  Failed = 8,
  Unsupported = 9,
  Stale = 10,
  Withdrawn = 11,
};

/// Control-plane authority for a scope. Revoked, expired and superseded are
/// distinct because they demand different recoveries.
enum class AuthorityState : std::uint8_t {
  None = 0,
  Granted = 1,
  Revoked = 2,
  Expired = 3,
  Superseded = 4,
};

/// Evidence freshness. PendingReverification is what survives a restart: it is
/// deliberately not Fresh, so persisted evidence cannot silently justify a
/// current decision after the coordinator epoch advances.
enum class Freshness : std::uint8_t {
  Unknown = 0,
  Fresh = 1,
  Stale = 2,
  Expired = 3,
  PendingReverification = 4,
};

/// Phase of a single bounded activation attempt.
enum class AttemptPhase : std::uint8_t {
  Unknown = 0,
  Staged = 1,
  Authorized = 2,
  Dispatched = 3,
  Acknowledged = 4,
  Applied = 5,
  Verified = 6,
  Failed = 7,
  Aborted = 8,
  Ambiguous = 9,
  Quiesced = 10,
  Withdrawn = 11,
};

/// Outcome reported by an execution/effect runtime. Only these values can move
/// an attempt to applied or verified; a missing report leaves the attempt
/// ambiguous and never becomes a failure or a success by default.
enum class EffectOutcome : std::uint8_t {
  Unknown = 0,
  Applied = 1,
  Verified = 2,
  Failed = 3,
  RefusedByExecutor = 4,
  Quiesced = 5,
  Removed = 6,
};

[[nodiscard]] std::string_view desired_state_name(DesiredState state) noexcept;
[[nodiscard]] DesiredState desired_state_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view lifecycle_state_name(LifecycleState state) noexcept;
[[nodiscard]] LifecycleState lifecycle_state_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view authority_state_name(AuthorityState state) noexcept;
[[nodiscard]] AuthorityState authority_state_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view freshness_name(Freshness freshness) noexcept;
[[nodiscard]] Freshness freshness_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view attempt_phase_name(AttemptPhase phase) noexcept;
[[nodiscard]] AttemptPhase attempt_phase_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view effect_outcome_name(EffectOutcome outcome) noexcept;
[[nodiscard]] EffectOutcome effect_outcome_from_name(std::string_view name) noexcept;

/// True when a lifecycle transition is part of the declared state machine.
[[nodiscard]] bool is_legal_lifecycle_transition(LifecycleState from, LifecycleState to) noexcept;

/// True when an attempt phase transition is part of the declared state machine.
/// Terminal phases (Failed, Aborted, Quiesced, Withdrawn, Verified) never move.
[[nodiscard]] bool is_legal_attempt_transition(AttemptPhase from, AttemptPhase to) noexcept;

/// True when the attempt phase is terminal and can never be settled again.
[[nodiscard]] bool is_terminal_attempt_phase(AttemptPhase phase) noexcept;

/// True when the lifecycle state permits new mutation under authority.
/// Quiesced, Withdrawn, Failed, Unsupported and Stale never do.
[[nodiscard]] bool permits_mutation(LifecycleState state) noexcept;

/// True when the state carries an accepted effect claim that the export may
/// surface as applied or verified. Requires enforcement-side evidence by
/// construction: only these states are ever reached through effect reports.
[[nodiscard]] bool is_effect_claim(LifecycleState state) noexcept;

/// Deterministic rank used for export ordering and conflict resolution.
[[nodiscard]] std::uint8_t lifecycle_rank(LifecycleState state) noexcept;

}  // namespace sncf

#endif  // SNCF_LIFECYCLE_HPP
