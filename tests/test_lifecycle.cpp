// Copyright 2026 Summon Software Labs.
#include <set>
#include <string>

#include "sncf/authority.hpp"
#include "sncf/lifecycle.hpp"
#include "sncf/status.hpp"
#include "test_framework.hpp"

namespace {

SNCF_TEST(lifecycle_reason_table_is_complete_and_unique) {
  std::set<std::string> names;
  for (std::uint16_t raw = 0; raw <= sncf::kMaxReasonCodeValue; ++raw) {
    const auto code = static_cast<sncf::ReasonCode>(raw);
    const std::string name(sncf::reason_name(code));
    if (raw == 0U) {
      SNCF_CHECK_EQ(name, std::string("unknown"));
      continue;
    }
    if (name == "unknown") {
      continue;  // not a defined enumerator
    }
    SNCF_CHECK(names.insert(name).second);
    SNCF_CHECK(!sncf::reason_description(code).empty());
    SNCF_CHECK(sncf::reason_from_name(name) == code);
  }
  SNCF_CHECK(names.size() > 80U);
  SNCF_CHECK(sncf::reason_class(sncf::ReasonCode::Accepted) == sncf::ReasonClass::Accepted);
  SNCF_CHECK(sncf::reason_class(sncf::ReasonCode::RefusedUnknownDevice) == sncf::ReasonClass::Refused);
  SNCF_CHECK(sncf::reason_class(sncf::ReasonCode::Unknown) == sncf::ReasonClass::Unknown);
}

SNCF_TEST(lifecycle_declared_transitions_hold) {
  using sncf::LifecycleState;
  SNCF_CHECK(sncf::is_legal_lifecycle_transition(LifecycleState::Unknown, LifecycleState::Desired));
  SNCF_CHECK(sncf::is_legal_lifecycle_transition(LifecycleState::Desired, LifecycleState::Eligible));
  SNCF_CHECK(sncf::is_legal_lifecycle_transition(LifecycleState::Eligible, LifecycleState::Authorized));
  SNCF_CHECK(sncf::is_legal_lifecycle_transition(LifecycleState::Authorized, LifecycleState::Acknowledged));
  SNCF_CHECK(sncf::is_legal_lifecycle_transition(LifecycleState::Acknowledged, LifecycleState::Applied));
  SNCF_CHECK(sncf::is_legal_lifecycle_transition(LifecycleState::Applied, LifecycleState::Verified));
  SNCF_CHECK(sncf::is_legal_lifecycle_transition(LifecycleState::Verified, LifecycleState::Quiesced));
  SNCF_CHECK(sncf::is_legal_lifecycle_transition(LifecycleState::Quiesced, LifecycleState::Withdrawn));
  SNCF_CHECK(sncf::is_legal_lifecycle_transition(LifecycleState::Stale, LifecycleState::Desired));
  SNCF_CHECK(sncf::is_legal_lifecycle_transition(LifecycleState::Withdrawn, LifecycleState::Desired));
  // Acked work cannot silently jump back to an earlier confirmation.
  SNCF_CHECK(!sncf::is_legal_lifecycle_transition(LifecycleState::Verified, LifecycleState::Acknowledged));
  SNCF_CHECK(!sncf::is_legal_lifecycle_transition(LifecycleState::Unknown, LifecycleState::Verified));
  SNCF_CHECK(!sncf::is_legal_lifecycle_transition(LifecycleState::Withdrawn, LifecycleState::Applied));
  SNCF_CHECK(!sncf::is_legal_lifecycle_transition(LifecycleState::Quiesced, LifecycleState::Applied));
  // Every state may be re-observed as itself.
  for (std::uint8_t raw = 0; raw <= 11U; ++raw) {
    SNCF_CHECK(sncf::is_legal_lifecycle_transition(static_cast<LifecycleState>(raw),
                                                   static_cast<LifecycleState>(raw)));
  }
}

SNCF_TEST(lifecycle_mutation_and_effect_claims_are_distinct) {
  using sncf::LifecycleState;
  SNCF_CHECK(sncf::permits_mutation(LifecycleState::Desired));
  SNCF_CHECK(sncf::permits_mutation(LifecycleState::Authorized));
  SNCF_CHECK(!sncf::permits_mutation(LifecycleState::Quiesced));
  SNCF_CHECK(!sncf::permits_mutation(LifecycleState::Withdrawn));
  SNCF_CHECK(!sncf::permits_mutation(LifecycleState::Stale));
  SNCF_CHECK(!sncf::permits_mutation(LifecycleState::Failed));
  SNCF_CHECK(!sncf::permits_mutation(LifecycleState::Unsupported));
  SNCF_CHECK(sncf::is_effect_claim(LifecycleState::Applied));
  SNCF_CHECK(sncf::is_effect_claim(LifecycleState::Verified));
  SNCF_CHECK(!sncf::is_effect_claim(LifecycleState::Authorized));
  SNCF_CHECK(!sncf::is_effect_claim(LifecycleState::Acknowledged));
}

SNCF_TEST(lifecycle_attempt_transitions_are_bounded) {
  using sncf::AttemptPhase;
  SNCF_CHECK(sncf::is_legal_attempt_transition(AttemptPhase::Dispatched, AttemptPhase::Acknowledged));
  SNCF_CHECK(sncf::is_legal_attempt_transition(AttemptPhase::Acknowledged, AttemptPhase::Applied));
  SNCF_CHECK(sncf::is_legal_attempt_transition(AttemptPhase::Applied, AttemptPhase::Verified));
  SNCF_CHECK(sncf::is_legal_attempt_transition(AttemptPhase::Dispatched, AttemptPhase::Ambiguous));
  SNCF_CHECK(sncf::is_legal_attempt_transition(AttemptPhase::Ambiguous, AttemptPhase::Applied));
  SNCF_CHECK(!sncf::is_legal_attempt_transition(AttemptPhase::Verified, AttemptPhase::Applied));
  SNCF_CHECK(!sncf::is_legal_attempt_transition(AttemptPhase::Failed, AttemptPhase::Applied));
  SNCF_CHECK(!sncf::is_legal_attempt_transition(AttemptPhase::Quiesced, AttemptPhase::Applied));
  SNCF_CHECK(sncf::is_terminal_attempt_phase(AttemptPhase::Verified));
  SNCF_CHECK(sncf::is_terminal_attempt_phase(AttemptPhase::Failed));
  SNCF_CHECK(!sncf::is_terminal_attempt_phase(AttemptPhase::Dispatched));
  SNCF_CHECK(!sncf::is_terminal_attempt_phase(AttemptPhase::Ambiguous));
}

SNCF_TEST(lifecycle_names_round_trip) {
  for (std::uint8_t raw = 0; raw <= 11U; ++raw) {
    const auto state = static_cast<sncf::LifecycleState>(raw);
    SNCF_CHECK(sncf::lifecycle_state_from_name(sncf::lifecycle_state_name(state)) == state);
  }
  for (std::uint8_t raw = 0; raw <= 4U; ++raw) {
    const auto desired = static_cast<sncf::DesiredState>(raw);
    SNCF_CHECK(sncf::desired_state_from_name(sncf::desired_state_name(desired)) == desired);
  }
  for (std::uint8_t raw = 0; raw <= 4U; ++raw) {
    const auto authority = static_cast<sncf::AuthorityState>(raw);
    SNCF_CHECK(sncf::authority_state_from_name(sncf::authority_state_name(authority)) == authority);
  }
  for (std::uint8_t raw = 0; raw <= 4U; ++raw) {
    const auto freshness = static_cast<sncf::Freshness>(raw);
    SNCF_CHECK(sncf::freshness_from_name(sncf::freshness_name(freshness)) == freshness);
  }
  for (std::uint8_t raw = 0; raw <= 11U; ++raw) {
    const auto phase = static_cast<sncf::AttemptPhase>(raw);
    SNCF_CHECK(sncf::attempt_phase_from_name(sncf::attempt_phase_name(phase)) == phase);
  }
  for (std::uint8_t raw = 0; raw <= 6U; ++raw) {
    const auto outcome = static_cast<sncf::EffectOutcome>(raw);
    SNCF_CHECK(sncf::effect_outcome_from_name(sncf::effect_outcome_name(outcome)) == outcome);
  }
  SNCF_CHECK(sncf::lifecycle_state_from_name("not-a-state") == sncf::LifecycleState::Unknown);
  SNCF_CHECK(sncf::reason_from_name("not-a-reason") == sncf::ReasonCode::Unknown);
}

SNCF_TEST(lifecycle_scope_keys_and_fencing_ledger) {
  sncf::ExclusiveScope scope{sncf::SmartNicId::from_value(1), sncf::DeviceId::from_value(2),
                             sncf::PortId::from_value(3)};
  SNCF_CHECK_EQ(scope.to_key(), std::string("sn=1;dev=2;port=3"));
  sncf::ExclusiveScope device_scope{sncf::SmartNicId::from_value(1), sncf::DeviceId::from_value(2),
                                    sncf::PortId{}};
  SNCF_CHECK_EQ(device_scope.to_key(), std::string("sn=1;dev=2;port=*"));
  SNCF_CHECK(scope.is_port_scope());
  SNCF_CHECK(!device_scope.is_port_scope());
  SNCF_CHECK(device_scope < scope);

  sncf::FencingLedger ledger;
  SNCF_CHECK_EQ(ledger.high_water(scope).value(), 0U);
  const auto first = ledger.allocate(scope);
  SNCF_REQUIRE(first.has_value());
  SNCF_CHECK_EQ(first->value(), 1U);
  const auto second = ledger.allocate(scope);
  SNCF_REQUIRE(second.has_value());
  SNCF_CHECK_EQ(second->value(), 2U);
  SNCF_CHECK_EQ(ledger.high_water(scope).value(), 2U);
  SNCF_CHECK_EQ(ledger.size(), 1U);
  ledger.observe(scope, sncf::FencingToken::from_value(1));
  SNCF_CHECK_EQ(ledger.high_water(scope).value(), 2U);
  ledger.observe(scope, sncf::FencingToken::from_value(9));
  SNCF_CHECK_EQ(ledger.high_water(scope).value(), 9U);
}

SNCF_TEST(lifecycle_semver_and_labels) {
  const auto parsed = sncf::parse_semver("2.5.11");
  SNCF_REQUIRE(parsed.has_value());
  SNCF_CHECK_EQ(parsed->major, 2);
  SNCF_CHECK_EQ(parsed->minor, 5);
  SNCF_CHECK_EQ(parsed->patch, 11);
  SNCF_CHECK_EQ(parsed->to_string(), std::string("2.5.11"));
  SNCF_CHECK(!sncf::parse_semver("2.5").has_value());
  SNCF_CHECK(!sncf::parse_semver("2.5.1.0").has_value());
  SNCF_CHECK(!sncf::parse_semver("02.5.1").has_value());
  SNCF_CHECK(!sncf::parse_semver("v2.5.1").has_value());
  SNCF_CHECK(sncf::parse_semver("1.0.0").value() < sncf::parse_semver("1.0.1").value());
  SNCF_CHECK(sncf::parse_semver("1.2.0").value() < sncf::parse_semver("2.0.0").value());

  SNCF_CHECK(sncf::is_valid_label("flow-classifier"));
  SNCF_CHECK(sncf::is_valid_label("a.b_c-1"));
  SNCF_CHECK(!sncf::is_valid_label("Upper"));
  SNCF_CHECK(!sncf::is_valid_label("-leading"));
  SNCF_CHECK(!sncf::is_valid_label(""));
  SNCF_CHECK(!sncf::is_valid_label(std::string(65, 'a')));
  auto validated = sncf::validate_label("ok");
  SNCF_CHECK(validated.ok());
  auto refused = sncf::validate_label("NOT OK");
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == sncf::ReasonCode::RefusedInvalidLabel);
}

SNCF_TEST(lifecycle_checked_arithmetic_refuses_overflow) {
  std::uint64_t sum = 0;
  SNCF_CHECK(sncf::add_checked<std::uint64_t>(1, 2, sum));
  SNCF_CHECK_EQ(sum, 3U);
  SNCF_CHECK(!sncf::add_checked<std::uint64_t>((std::numeric_limits<std::uint64_t>::max)(), 1, sum));
  std::uint64_t product = 0;
  SNCF_CHECK(!sncf::mul_checked<std::uint64_t>((std::numeric_limits<std::uint64_t>::max)(), 2, product));
  SNCF_CHECK(!sncf::narrow_checked<std::uint16_t>(70000U).has_value());
  SNCF_CHECK(sncf::narrow_checked<std::uint16_t>(65535U).has_value());
  SNCF_CHECK(!sncf::byte_size_checked((std::numeric_limits<std::uint64_t>::max)(), 8).has_value());
  const auto timestamp = sncf::timestamp_add(sncf::TimestampNs::from_value(10), sncf::DurationNs::from_value(5));
  SNCF_REQUIRE(timestamp.has_value());
  SNCF_CHECK_EQ(timestamp->value(), 15U);
  SNCF_CHECK(!sncf::timestamp_add(sncf::TimestampNs::from_value((std::numeric_limits<std::uint64_t>::max)()),
                                  sncf::DurationNs::from_value(1))
                  .has_value());
  SNCF_CHECK(!sncf::timestamp_difference(sncf::TimestampNs::from_value(1), sncf::TimestampNs::from_value(2))
                  .has_value());
}

}  // namespace
