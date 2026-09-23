// Copyright 2026 Summon Software Labs.
#include "sncf/evidence.hpp"

#include <array>

namespace sncf {
namespace {

constexpr std::array<std::string_view, 9> kSourceNames{"unknown",       "topology_runtime",
                                                      "device_observation_runtime", "capability_evidence_provider",
                                                      "policy_runtime", "authority_runtime",
                                                      "execution_runtime", "operator",
                                                      "synthetic_fixture"};
constexpr std::array<std::string_view, 6> kKindNames{"unknown", "capability_report", "device_observation",
                                                     "execution_effect_report", "quiesce_confirmation",
                                                     "removal_confirmation"};

template <class Enum, std::size_t N>
Enum from_names(const std::array<std::string_view, N>& names, std::string_view name, Enum fallback) noexcept {
  for (std::size_t i = 0; i < N; ++i) {
    if (names[i] == name) {
      return static_cast<Enum>(i);
    }
  }
  return fallback;
}

}  // namespace

std::string_view evidence_source_name(EvidenceSource source) noexcept {
  const auto index = static_cast<std::size_t>(source);
  return index < kSourceNames.size() ? kSourceNames[index] : std::string_view{"unknown"};
}

EvidenceSource evidence_source_from_name(std::string_view name) noexcept {
  return from_names(kSourceNames, name, EvidenceSource::Unknown);
}

std::string_view evidence_kind_name(EvidenceKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  return index < kKindNames.size() ? kKindNames[index] : std::string_view{"unknown"};
}

EvidenceKind evidence_kind_from_name(std::string_view name) noexcept {
  return from_names(kKindNames, name, EvidenceKind::Unknown);
}

Result<Freshness> evaluate_freshness(TimestampNs observed_at, TimestampNs now, DurationNs max_age) noexcept {
  if (observed_at.value() > now.value()) {
    return Status(ReasonCode::RefusedInvalidRange, "evidence is dated in the future relative to the clock");
  }
  const auto age = timestamp_difference(now, observed_at);
  if (!age.has_value()) {
    return Status(ReasonCode::RefusedInvalidRange, "evidence age could not be computed");
  }
  if (age->value() > max_age.value()) {
    return Freshness::Expired;
  }
  return Freshness::Fresh;
}

Freshness refresh_after_restart(Freshness persisted, CoordinatorEpoch persisted_epoch,
                                CoordinatorEpoch live_epoch) noexcept {
  switch (persisted) {
    case Freshness::Fresh:
      // A restart advances the epoch, so evidence accepted by a previous
      // incarnation never returns to Fresh on its own.
      return persisted_epoch == live_epoch ? Freshness::Fresh : Freshness::PendingReverification;
    case Freshness::PendingReverification:
      // Still awaiting revalidation: repeated restarts must not blur this into
      // "unknown", which would lose the reason it cannot be used.
      return Freshness::PendingReverification;
    case Freshness::Stale:
    case Freshness::Expired:
    case Freshness::Unknown:
      return persisted;
  }
  return Freshness::Unknown;
}

}  // namespace sncf
