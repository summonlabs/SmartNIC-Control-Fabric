// Copyright 2026 Summon Software Labs.
#include "sncf/fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace sncf;
using sncf_test::Harness;
using sncf_test::Topology;

SNCF_TEST(fabric_fresh_open_advances_epoch_and_reports_recovery) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  SNCF_CHECK_EQ(fabric.value()->epoch().value(), 1U);
  SNCF_CHECK_EQ(fabric.value()->boot().value(), 1U);
  SNCF_CHECK(fabric.value()->recovery().disposition == RecoveryDisposition::FreshStore);
  const std::string export_text = fabric.value()->export_canonical();
  SNCF_CHECK(!export_text.empty());
  SNCF_CHECK(export_text.find("\"durable\"") != std::string::npos);
}

SNCF_TEST(fabric_registration_and_duplicate_delivery) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);

  // Re-registering an identical device is a no-op with the same identity.
  RegisterDeviceRequest device;
  device.header = harness.next_header();
  device.smartnic = topology.smartnic;
  device.model = topology.model;
  device.incarnation = topology.incarnation;
  device.serial = "serial-0001";
  device.port = topology.port;
  auto same = fabric.value()->register_device(device);
  SNCF_REQUIRE(same.ok());
  SNCF_CHECK(same.value().outcome.reason == ReasonCode::AcceptedNoChange);
  SNCF_CHECK_EQ(same.value().id.value(), topology.device.value());

  // Redelivering the exact same command returns the recorded outcome.
  auto replay = fabric.value()->register_device(device);
  SNCF_REQUIRE(replay.ok());
  SNCF_CHECK(replay.value().outcome.duplicate);
  SNCF_CHECK(replay.value().outcome.reason == ReasonCode::AcceptedNoChange);
  SNCF_CHECK_EQ(fabric.value()->counters().commands_deduplicated, 1U);

  // A different device on the same port with an older incarnation is stale.
  RegisterDeviceRequest stale = device;
  stale.header = harness.next_header();
  stale.incarnation = DeviceIncarnation::from_value(0);
  auto stale_result = fabric.value()->register_device(stale);
  SNCF_CHECK(!stale_result.ok());
  SNCF_CHECK(stale_result.code() == ReasonCode::RefusedNilIdentity ||
             stale_result.code() == ReasonCode::RefusedStaleDeviceIncarnation);

  // A higher incarnation supersedes the device and fences bound functions.
  RegisterDeviceRequest reincarnated = device;
  reincarnated.header = harness.next_header();
  reincarnated.incarnation = DeviceIncarnation::from_value(2);
  auto advanced = fabric.value()->register_device(reincarnated);
  SNCF_REQUIRE(advanced.ok());
  SNCF_CHECK_EQ(advanced.value().id.value(), topology.device.value());
}

SNCF_TEST(fabric_full_lifecycle_to_verified) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const sncf_test::Activation activation =
      sncf_test::activate_new_instance(*fabric.value(), harness, topology);

  auto instance = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(instance.ok());
  SNCF_CHECK(instance.value().lifecycle == LifecycleState::Authorized);
  SNCF_CHECK(instance.value().authority == AuthorityState::Granted);
  SNCF_CHECK(instance.value().desired == DesiredState::Active);
  SNCF_CHECK_EQ(instance.value().attempts.size(), 1U);
  SNCF_CHECK(instance.value().attempts[0].phase == AttemptPhase::Dispatched);

  AcknowledgeRequest ack;
  ack.header = harness.next_header();
  ack.instance = activation.instance;
  ack.attempt = activation.attempt;
  ack.generation = activation.generation;
  ack.token = activation.token;
  ack.source = EvidenceSource::SyntheticFixture;
  ack.observed_at = harness.clock().now();
  auto acknowledged = fabric.value()->acknowledge(ack);
  SNCF_REQUIRE(acknowledged.ok());
  auto after_ack = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(after_ack.ok());
  SNCF_CHECK(after_ack.value().lifecycle == LifecycleState::Acknowledged);
  SNCF_CHECK(after_ack.value().attempts[0].phase == AttemptPhase::Acknowledged);

  EffectReportRequest applied;
  applied.header = harness.next_header();
  applied.instance = activation.instance;
  applied.attempt = activation.attempt;
  applied.generation = activation.generation;
  applied.token = activation.token;
  applied.outcome = EffectOutcome::Applied;
  applied.source = EvidenceSource::SyntheticFixture;
  applied.payload_digest = sncf_test::digest_of('e');
  applied.observed_at = harness.clock().now();
  applied.synthetic = true;
  auto applied_result = fabric.value()->report_effect(applied);
  SNCF_REQUIRE(applied_result.ok());
  auto after_applied = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(after_applied.ok());
  SNCF_CHECK(after_applied.value().lifecycle == LifecycleState::Applied);
  SNCF_CHECK(after_applied.value().attempts[0].phase == AttemptPhase::Applied);

  EffectReportRequest verified = applied;
  verified.header = harness.next_header();
  verified.outcome = EffectOutcome::Verified;
  verified.payload_digest = sncf_test::digest_of('f');
  auto verified_result = fabric.value()->report_effect(verified);
  SNCF_REQUIRE(verified_result.ok());
  auto after_verified = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(after_verified.ok());
  SNCF_CHECK(after_verified.value().lifecycle == LifecycleState::Verified);
  SNCF_CHECK(after_verified.value().attempts[0].phase == AttemptPhase::Verified);
  SNCF_CHECK(after_verified.value().last_effect_outcome == EffectOutcome::Verified);

  // Redelivering the acknowledgement is idempotent and cannot downgrade the
  // verified claim back to "acknowledged".
  auto late_ack = fabric.value()->acknowledge(ack);
  SNCF_REQUIRE(late_ack.ok());
  SNCF_CHECK(late_ack.value().duplicate);
  auto still_verified = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(still_verified.ok());
  SNCF_CHECK(still_verified.value().lifecycle == LifecycleState::Verified);
  SNCF_CHECK(still_verified.value().attempts[0].phase == AttemptPhase::Verified);
}

SNCF_TEST(fabric_acknowledgement_is_not_application) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);

  AcknowledgeRequest ack;
  ack.header = harness.next_header();
  ack.instance = activation.instance;
  ack.attempt = activation.attempt;
  ack.generation = activation.generation;
  ack.token = activation.token;
  ack.source = EvidenceSource::SyntheticFixture;
  ack.observed_at = harness.clock().now();
  SNCF_REQUIRE(fabric.value()->acknowledge(ack).ok());

  auto instance = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(instance.ok());
  SNCF_CHECK(instance.value().lifecycle == LifecycleState::Acknowledged);
  SNCF_CHECK(!is_effect_claim(instance.value().lifecycle));
  SNCF_CHECK(instance.value().last_effect_digest.is_zero());
  SNCF_CHECK(instance.value().last_effect_outcome == EffectOutcome::Unknown);
}

SNCF_TEST(fabric_quiesce_and_withdraw_revoke_authority_immediately) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);
  sncf_test::report_verified(*fabric.value(), harness, activation);

  auto instance = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(instance.ok());

  QuiesceRequest quiesce;
  quiesce.header = harness.next_header();
  quiesce.instance = activation.instance;
  quiesce.claim = sncf_test::claim_for(instance.value(), *fabric.value());
  auto quiesced = fabric.value()->request_quiesce(quiesce);
  SNCF_REQUIRE(quiesced.ok());
  auto after_intent = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(after_intent.ok());
  SNCF_CHECK(after_intent.value().desired == DesiredState::Quiesced);
  SNCF_CHECK(after_intent.value().authority == AuthorityState::Revoked);
  SNCF_CHECK(after_intent.value().lease.is_nil());
  // The confirmed lifecycle has not changed: intent is not effect.
  SNCF_CHECK(after_intent.value().lifecycle == LifecycleState::Verified);

  // Authority is gone, so a second mutation is refused. A fresh command identity
  // is used so this is a real attempt and not a replay of the first command.
  WithdrawRequest withdraw;
  withdraw.header = harness.next_header();
  withdraw.instance = activation.instance;
  withdraw.claim = quiesce.claim;
  auto second = fabric.value()->request_withdraw(withdraw);
  SNCF_CHECK(!second.ok());
  SNCF_CHECK(second.code() == ReasonCode::RefusedAuthorityRevoked ||
             second.code() == ReasonCode::RefusedLeaseExpired ||
             second.code() == ReasonCode::RefusedNoAuthority);

  // The execution side confirms the drain of the attempt the intent dispatched.
  EffectReportRequest drained;
  drained.header = harness.next_header();
  drained.instance = activation.instance;
  drained.attempt = after_intent.value().current_attempt;
  drained.generation = activation.generation;
  drained.token = activation.token;
  drained.outcome = EffectOutcome::Quiesced;
  drained.source = EvidenceSource::SyntheticFixture;
  drained.payload_digest = sncf_test::digest_of('1');
  drained.observed_at = harness.clock().now();
  drained.synthetic = true;
  auto confirmed = fabric.value()->report_effect(drained);
  if (!confirmed.ok()) {
    ::sncf_test::fail(__FILE__, __LINE__, std::string("drain confirmation refused: ") +
                                             std::string(reason_name(confirmed.code())) + " - " +
                                             confirmed.status().detail());
  }
  auto final_state = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(final_state.ok());
  SNCF_CHECK(final_state.value().lifecycle == LifecycleState::Quiesced);
  SNCF_CHECK(final_state.value().authority == AuthorityState::Revoked);

  // A quiesced instance refuses further authority.
  AcquireAuthorityRequest acquire;
  acquire.header = harness.next_header();
  acquire.instance = activation.instance;
  acquire.scope = ExclusiveScope{final_state.value().smartnic, final_state.value().device,
                                 final_state.value().port};
  auto refused = fabric.value()->acquire_authority(acquire);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedInstanceQuiesced);
}

SNCF_TEST(fabric_replacement_and_rollback) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto first = sncf_test::activate_new_instance(*fabric.value(), harness, topology);
  sncf_test::report_verified(*fabric.value(), harness, first);

  RegisterPackageRequest replacement;
  replacement.header = harness.next_header();
  replacement.name = "flow-classifier";
  replacement.version = SemVer{2, 0, 0};
  replacement.digest = sncf_test::digest_of('9');
  replacement.supported_models = {topology.model};
  replacement.required_capabilities = {CapabilityCode::PacketParsing};
  replacement.min_capability_generation = CapabilityGeneration::from_value(1);
  replacement.compatibility.min = CompatibilityGeneration::from_value(1);
  replacement.compatibility.max = CompatibilityGeneration::from_value(20);
  replacement.min_firmware = SemVer{1, 0, 0};
  replacement.exclusive_scope = true;
  auto package = fabric.value()->register_package(replacement);
  SNCF_REQUIRE(package.ok());

  auto instance = fabric.value()->inspect_instance(first.instance);
  SNCF_REQUIRE(instance.ok());
  ActivateRequest replace;
  replace.header = harness.next_header();
  replace.smartnic = topology.smartnic;
  replace.device = topology.device;
  replace.incarnation = topology.incarnation;
  replace.package = package.value().id;
  replace.port = topology.port;
  replace.queue = QueueId::from_value(1);
  replace.instance = first.instance;
  replace.claim = sncf_test::claim_for(instance.value(), *fabric.value());
  auto replaced = fabric.value()->activate(replace);
  SNCF_REQUIRE(replaced.ok());
  SNCF_CHECK_EQ(replaced.value().generation.value(), 2U);
  auto after_replace = fabric.value()->inspect_instance(first.instance);
  SNCF_REQUIRE(after_replace.ok());
  SNCF_CHECK_EQ(after_replace.value().package.value(), package.value().id.value());
  SNCF_CHECK_EQ(after_replace.value().previous_package.value(), topology.package.value());
  SNCF_CHECK_EQ(fabric.value()->counters().instances_replaced, 1U);

  sncf_test::Activation second;
  second.instance = first.instance;
  second.attempt = replaced.value().attempt;
  second.lease = replaced.value().lease;
  second.token = replaced.value().token;
  second.generation = replaced.value().generation;
  sncf_test::report_verified(*fabric.value(), harness, second);

  auto settled = fabric.value()->inspect_instance(first.instance);
  SNCF_REQUIRE(settled.ok());
  RollbackRequest rollback;
  rollback.header = harness.next_header();
  rollback.instance = first.instance;
  rollback.claim = sncf_test::claim_for(settled.value(), *fabric.value());
  auto rolled = fabric.value()->request_rollback(rollback);
  SNCF_REQUIRE(rolled.ok());
  auto after_rollback = fabric.value()->inspect_instance(first.instance);
  SNCF_REQUIRE(after_rollback.ok());
  SNCF_CHECK_EQ(after_rollback.value().package.value(), topology.package.value());
  SNCF_CHECK_EQ(after_rollback.value().previous_package.value(), package.value().id.value());
  SNCF_CHECK_EQ(after_rollback.value().deployment_generation.value(), 3U);
}

SNCF_TEST(fabric_rollback_requires_a_previous_deployment) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);
  sncf_test::report_verified(*fabric.value(), harness, activation);
  auto instance = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(instance.ok());
  RollbackRequest rollback;
  rollback.header = harness.next_header();
  rollback.instance = activation.instance;
  rollback.claim = sncf_test::claim_for(instance.value(), *fabric.value());
  auto refused = fabric.value()->request_rollback(rollback);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedIllegalTransition);
}

SNCF_TEST(fabric_capacity_bounds_are_enforced) {
  FabricConfig config;
  config.max_instances = 1;
  config.max_smartnics = 1;
  config.max_packages = 1;
  Harness harness(config);
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  sncf_test::activate_new_instance(*fabric.value(), harness, topology,
                                   PortId::from_value(1));

  RegisterSmartNicRequest extra;
  extra.header = harness.next_header();
  extra.label = "nic-b";
  extra.port_count = 1;
  auto refused_smartnic = fabric.value()->register_smartnic(extra);
  SNCF_CHECK(!refused_smartnic.ok());
  SNCF_CHECK(refused_smartnic.code() == ReasonCode::RefusedCapacityExceeded);

  ActivateRequest second;
  second.header = harness.next_header();
  second.smartnic = topology.smartnic;
  second.device = topology.device;
  second.incarnation = topology.incarnation;
  second.package = topology.package;
  second.port = PortId::from_value(2);
  second.queue = QueueId::from_value(1);
  auto refused = fabric.value()->activate(second);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedCapacityExceeded);
  SNCF_CHECK_EQ(fabric.value()->counters().commands_refused, 2U);
  SNCF_CHECK(fabric.value()->reasons().count(ReasonCode::RefusedCapacityExceeded) == 2U);
}

SNCF_TEST(fabric_shutdown_refuses_further_mutation) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  fabric.value()->shutdown();
  SNCF_CHECK(!fabric.value()->is_running());
  RegisterSmartNicRequest request;
  request.header = harness.next_header();
  request.label = "nic-z";
  request.port_count = 1;
  auto refused = fabric.value()->register_smartnic(request);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedShuttingDown);
  fabric.value()->shutdown();  // idempotent
}

SNCF_TEST(fabric_unknown_entities_are_refused_distinctly) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  ActivateRequest request;
  request.header = harness.next_header();
  request.smartnic = SmartNicId::from_value(99);
  request.device = DeviceId::from_value(99);
  request.incarnation = DeviceIncarnation::from_value(1);
  request.package = FunctionPackageId::from_value(99);
  request.port = PortId::from_value(1);
  auto refused = fabric.value()->activate(request);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedUnknownPackage);

  auto missing_instance = fabric.value()->inspect_instance(FunctionInstanceId::from_value(5));
  SNCF_CHECK(!missing_instance.ok());
  SNCF_CHECK(missing_instance.code() == ReasonCode::RefusedUnknownInstance);
}

}  // namespace
