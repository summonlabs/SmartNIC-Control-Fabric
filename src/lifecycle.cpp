// Copyright 2026 Summon Software Labs.
#include "sncf/lifecycle.hpp"

#include <array>

namespace sncf {
namespace {

constexpr std::size_t kStateCount = 12;

using TransitionTable = std::array<std::array<bool, kStateCount>, kStateCount>;

constexpr TransitionTable make_transitions() noexcept {
  TransitionTable table{};
  for (std::size_t i = 0; i < kStateCount; ++i) {
    table[i][i] = true;  // re-observing the same state is a no-op, not a transition
  }
  const auto allow = [&table](LifecycleState from, std::initializer_list<LifecycleState> to) constexpr {
    for (const auto target : to) {
      table[static_cast<std::size_t>(from)][static_cast<std::size_t>(target)] = true;
    }
  };
  allow(LifecycleState::Unknown, {LifecycleState::Desired});
  allow(LifecycleState::Desired,
        {LifecycleState::Eligible, LifecycleState::Unsupported, LifecycleState::Failed,
         LifecycleState::Withdrawn, LifecycleState::Stale, LifecycleState::Quiesced});
  allow(LifecycleState::Eligible,
        {LifecycleState::Authorized, LifecycleState::Unsupported, LifecycleState::Failed,
         LifecycleState::Stale, LifecycleState::Withdrawn});
  allow(LifecycleState::Authorized,
        {LifecycleState::Acknowledged, LifecycleState::Applied, LifecycleState::Verified,
         LifecycleState::Failed, LifecycleState::Stale, LifecycleState::Quiesced, LifecycleState::Withdrawn});
  allow(LifecycleState::Acknowledged,
        {LifecycleState::Applied, LifecycleState::Verified, LifecycleState::Failed, LifecycleState::Stale,
         LifecycleState::Quiesced, LifecycleState::Withdrawn});
  allow(LifecycleState::Applied,
        {LifecycleState::Verified, LifecycleState::Failed, LifecycleState::Stale,
         LifecycleState::Quiesced, LifecycleState::Withdrawn});
  allow(LifecycleState::Verified,
        {LifecycleState::Applied, LifecycleState::Failed, LifecycleState::Stale,
         LifecycleState::Quiesced, LifecycleState::Withdrawn});
  allow(LifecycleState::Quiesced, {LifecycleState::Withdrawn, LifecycleState::Desired, LifecycleState::Stale});
  allow(LifecycleState::Failed, {LifecycleState::Desired, LifecycleState::Withdrawn});
  allow(LifecycleState::Unsupported, {LifecycleState::Desired, LifecycleState::Withdrawn});
  allow(LifecycleState::Stale, {LifecycleState::Desired, LifecycleState::Withdrawn});
  allow(LifecycleState::Withdrawn, {LifecycleState::Desired});
  return table;
}

constexpr TransitionTable kTransitions = make_transitions();

constexpr std::size_t kAttemptPhaseCount = 12;

using AttemptTable = std::array<std::array<bool, kAttemptPhaseCount>, kAttemptPhaseCount>;

constexpr AttemptTable make_attempt_transitions() noexcept {
  AttemptTable table{};
  const auto allow = [&table](AttemptPhase from, std::initializer_list<AttemptPhase> to) constexpr {
    for (const auto target : to) {
      table[static_cast<std::size_t>(from)][static_cast<std::size_t>(target)] = true;
    }
  };
  allow(AttemptPhase::Unknown, {AttemptPhase::Staged, AttemptPhase::Aborted});
  allow(AttemptPhase::Staged,
        {AttemptPhase::Authorized, AttemptPhase::Aborted, AttemptPhase::Ambiguous, AttemptPhase::Failed});
  allow(AttemptPhase::Authorized,
        {AttemptPhase::Dispatched, AttemptPhase::Ambiguous, AttemptPhase::Aborted, AttemptPhase::Failed});
  allow(AttemptPhase::Dispatched,
        {AttemptPhase::Acknowledged, AttemptPhase::Applied, AttemptPhase::Verified, AttemptPhase::Failed,
         AttemptPhase::Ambiguous, AttemptPhase::Quiesced, AttemptPhase::Withdrawn});
  allow(AttemptPhase::Acknowledged,
        {AttemptPhase::Applied, AttemptPhase::Verified, AttemptPhase::Failed, AttemptPhase::Ambiguous,
         AttemptPhase::Quiesced, AttemptPhase::Withdrawn});
  allow(AttemptPhase::Applied,
        {AttemptPhase::Verified, AttemptPhase::Failed, AttemptPhase::Quiesced, AttemptPhase::Withdrawn,
         AttemptPhase::Ambiguous});
  allow(AttemptPhase::Ambiguous,
        {AttemptPhase::Applied, AttemptPhase::Verified, AttemptPhase::Failed, AttemptPhase::Quiesced,
         AttemptPhase::Withdrawn, AttemptPhase::Aborted});
  return table;
}

constexpr AttemptTable kAttemptTransitions = make_attempt_transitions();

constexpr std::array<std::string_view, 5> kDesiredNames{"none", "deployed", "active", "quiesced", "withdrawn"};
constexpr std::array<std::string_view, 12> kLifecycleNames{"unknown", "desired",   "eligible", "authorized",
                                                          "acknowledged", "applied", "verified", "quiesced",
                                                          "failed", "unsupported", "stale", "withdrawn"};
constexpr std::array<std::string_view, 5> kAuthorityNames{"none", "granted", "revoked", "expired", "superseded"};
constexpr std::array<std::string_view, 5> kFreshnessNames{"unknown", "fresh", "stale", "expired",
                                                          "pending_reverification"};
constexpr std::array<std::string_view, 12> kAttemptPhaseNames{"unknown",   "staged", "authorized", "dispatched",
                                                              "acknowledged", "applied", "verified", "failed",
                                                              "aborted", "ambiguous", "quiesced", "withdrawn"};
constexpr std::array<std::string_view, 7> kEffectOutcomeNames{"unknown", "applied",   "verified",    "failed",
                                                              "refused_by_executor", "quiesced", "removed"};

template <class Enum, std::size_t N>
Enum enum_from_name(const std::array<std::string_view, N>& names, std::string_view name, Enum fallback) noexcept {
  for (std::size_t i = 0; i < N; ++i) {
    if (names[i] == name) {
      return static_cast<Enum>(i);
    }
  }
  return fallback;
}

}  // namespace

std::string_view desired_state_name(DesiredState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  return index < kDesiredNames.size() ? kDesiredNames[index] : std::string_view{"unknown"};
}

DesiredState desired_state_from_name(std::string_view name) noexcept {
  return enum_from_name(kDesiredNames, name, DesiredState::None);
}

std::string_view lifecycle_state_name(LifecycleState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  return index < kLifecycleNames.size() ? kLifecycleNames[index] : std::string_view{"unknown"};
}

LifecycleState lifecycle_state_from_name(std::string_view name) noexcept {
  return enum_from_name(kLifecycleNames, name, LifecycleState::Unknown);
}

std::string_view authority_state_name(AuthorityState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  return index < kAuthorityNames.size() ? kAuthorityNames[index] : std::string_view{"none"};
}

AuthorityState authority_state_from_name(std::string_view name) noexcept {
  return enum_from_name(kAuthorityNames, name, AuthorityState::None);
}

std::string_view freshness_name(Freshness freshness) noexcept {
  const auto index = static_cast<std::size_t>(freshness);
  return index < kFreshnessNames.size() ? kFreshnessNames[index] : std::string_view{"unknown"};
}

Freshness freshness_from_name(std::string_view name) noexcept {
  return enum_from_name(kFreshnessNames, name, Freshness::Unknown);
}

std::string_view attempt_phase_name(AttemptPhase phase) noexcept {
  const auto index = static_cast<std::size_t>(phase);
  return index < kAttemptPhaseNames.size() ? kAttemptPhaseNames[index] : std::string_view{"unknown"};
}

AttemptPhase attempt_phase_from_name(std::string_view name) noexcept {
  return enum_from_name(kAttemptPhaseNames, name, AttemptPhase::Unknown);
}

std::string_view effect_outcome_name(EffectOutcome outcome) noexcept {
  const auto index = static_cast<std::size_t>(outcome);
  return index < kEffectOutcomeNames.size() ? kEffectOutcomeNames[index] : std::string_view{"unknown"};
}

EffectOutcome effect_outcome_from_name(std::string_view name) noexcept {
  return enum_from_name(kEffectOutcomeNames, name, EffectOutcome::Unknown);
}

bool is_legal_lifecycle_transition(LifecycleState from, LifecycleState to) noexcept {
  const auto from_index = static_cast<std::size_t>(from);
  const auto to_index = static_cast<std::size_t>(to);
  if (from_index >= kStateCount || to_index >= kStateCount) {
    return false;
  }
  return kTransitions[from_index][to_index];
}

bool is_legal_attempt_transition(AttemptPhase from, AttemptPhase to) noexcept {
  const auto from_index = static_cast<std::size_t>(from);
  const auto to_index = static_cast<std::size_t>(to);
  if (from_index >= kAttemptPhaseCount || to_index >= kAttemptPhaseCount) {
    return false;
  }
  if (from == to) {
    return true;
  }
  return kAttemptTransitions[from_index][to_index];
}

bool is_terminal_attempt_phase(AttemptPhase phase) noexcept {
  switch (phase) {
    case AttemptPhase::Failed:
    case AttemptPhase::Aborted:
    case AttemptPhase::Quiesced:
    case AttemptPhase::Withdrawn:
    case AttemptPhase::Verified:
      return true;
    case AttemptPhase::Unknown:
    case AttemptPhase::Staged:
    case AttemptPhase::Authorized:
    case AttemptPhase::Dispatched:
    case AttemptPhase::Acknowledged:
    case AttemptPhase::Applied:
    case AttemptPhase::Ambiguous:
      return false;
  }
  return true;
}

bool permits_mutation(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::Unknown:
    case LifecycleState::Desired:
    case LifecycleState::Eligible:
    case LifecycleState::Authorized:
    case LifecycleState::Acknowledged:
    case LifecycleState::Applied:
    case LifecycleState::Verified:
      return true;
    case LifecycleState::Quiesced:
    case LifecycleState::Failed:
    case LifecycleState::Unsupported:
    case LifecycleState::Stale:
    case LifecycleState::Withdrawn:
      return false;
  }
  return false;
}

bool is_effect_claim(LifecycleState state) noexcept {
  return state == LifecycleState::Applied || state == LifecycleState::Verified;
}

std::uint8_t lifecycle_rank(LifecycleState state) noexcept {
  return static_cast<std::uint8_t>(state);
}

}  // namespace sncf
