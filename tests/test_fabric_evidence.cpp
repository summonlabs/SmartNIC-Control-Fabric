// Copyright 2026 Summon Software Labs.
#include <functional>

#include "sncf/fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace sncf;
using sncf_test::Harness;
using sncf_test::Topology;

/// Registers everything except evidence, so a test can control it precisely.
struct Bare {
  SmartNicId smartnic;
  DeviceId device;
  FunctionPackageId package;
  DeviceModelId model = DeviceModelId::from_value(1);
  DeviceIncarnation incarnation = DeviceIncarnation::from_value(1);
  PortId port = PortId::from_value(1);
};

Bare register_bare(Fabric& fabric, Harness& harness,
                   const std::vector<CapabilityCode>& required = {CapabilityCode::PacketParsing}) {
  Bare bare;
  RegisterSmartNicRequest smartnic;
  smartnic.header = harness.next_header();
  smartnic.label = "nic-a";
  smartnic.port_count = 4;
  auto smartnic_result = fabric.register_smartnic(smartnic);
  SNCF_REQUIRE(smartnic_result.ok());
  bare.smartnic = smartnic_result.value().id;

  RegisterDeviceRequest device;
  device.header = harness.next_header();
  device.smartnic = bare.smartnic;
  device.model = bare.model;
  device.incarnation = bare.incarnation;
  device.serial = "serial-0001";
  device.port = bare.port;
  auto device_result = fabric.register_device(device);
  SNCF_REQUIRE(device_result.ok());
  bare.device = device_result.value().id;

  RegisterPackageRequest package;
  package.header = harness.next_header();
  package.name = "flow-classifier";
  package.version = SemVer{1, 0, 0};
  package.digest = sncf_test::digest_of('a');
  package.supported_models = {bare.model};
  package.required_capabilities = required;
  package.min_capability_generation = CapabilityGeneration::from_value(2);
  package.compatibility.min = CompatibilityGeneration::from_value(5);
  package.compatibility.max = CompatibilityGeneration::from_value(12);
  package.min_firmware = SemVer{2, 0, 0};
  package.exclusive_scope = true;
  auto package_result = fabric.register_package(package);
  SNCF_REQUIRE(package_result.ok());
  bare.package = package_result.value().id;
  return bare;
}

CapabilityEvidenceRequest make_evidence(Harness& harness, const Bare& bare) {
  CapabilityEvidenceRequest evidence;
  evidence.header = harness.next_header();
  evidence.source = EvidenceSource::SyntheticFixture;
  evidence.payload_digest = sncf_test::digest_of('b');
  evidence.observed_at = harness.clock().now();
  evidence.synthetic = true;
  evidence.smartnic = bare.smartnic;
  evidence.device = bare.device;
  evidence.incarnation = bare.incarnation;
  evidence.model = bare.model;
  evidence.capability_generation = CapabilityGeneration::from_value(4);
  evidence.compatibility_generation = CompatibilityGeneration::from_value(9);
  evidence.firmware = SemVer{2, 5, 0};
  evidence.capabilities = {CapabilityCode::PacketParsing, CapabilityCode::QueueSteering};
  return evidence;
}

ActivateRequest make_activation(Harness& harness, const Bare& bare) {
  ActivateRequest request;
  request.header = harness.next_header();
  request.smartnic = bare.smartnic;
  request.device = bare.device;
  request.incarnation = bare.incarnation;
  request.package = bare.package;
  request.port = bare.port;
  request.queue = QueueId::from_value(1);
  return request;
}

SNCF_TEST(fabric_evidence_absence_is_not_success) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  const Bare bare = register_bare(*fabric.value(), harness);
  auto refused = fabric.value()->activate(make_activation(harness, bare));
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedCapabilityEvidenceAbsent);
}

SNCF_TEST(fabric_evidence_must_be_present_before_activation) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  const Bare bare = register_bare(*fabric.value(), harness);
  SNCF_REQUIRE(fabric.value()->submit_capability_evidence(make_evidence(harness, bare)).ok());

  // The device has evidence but has never been observed present.
  auto refused = fabric.value()->activate(make_activation(harness, bare));
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedEvidenceSourceUnavailable);

  ObservationRequest observation;
  observation.header = harness.next_header();
  observation.source = EvidenceSource::SyntheticFixture;
  observation.payload_digest = sncf_test::digest_of('c');
  observation.observed_at = harness.clock().now();
  observation.synthetic = true;
  observation.smartnic = bare.smartnic;
  observation.device = bare.device;
  observation.incarnation = bare.incarnation;
  observation.present = true;
  observation.capability_generation = CapabilityGeneration::from_value(4);
  observation.compatibility_generation = CompatibilityGeneration::from_value(9);
  SNCF_REQUIRE(fabric.value()->submit_observation(observation).ok());
  auto accepted = fabric.value()->activate(make_activation(harness, bare));
  SNCF_REQUIRE(accepted.ok());
}

SNCF_TEST(fabric_evidence_provenance_is_enforced) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  const Bare bare = register_bare(*fabric.value(), harness);
  CapabilityEvidenceRequest evidence = make_evidence(harness, bare);
  evidence.source = EvidenceSource::ExecutionRuntime;
  auto refused = fabric.value()->submit_capability_evidence(evidence);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedEvidenceProvenanceUntrusted);
  SNCF_CHECK(fabric.value()->reasons().count(ReasonCode::RefusedEvidenceProvenanceUntrusted) == 1U);
}

SNCF_TEST(fabric_evidence_requires_a_binding_digest) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  const Bare bare = register_bare(*fabric.value(), harness);
  CapabilityEvidenceRequest evidence = make_evidence(harness, bare);
  evidence.payload_digest = Digest256{};
  auto refused = fabric.value()->submit_capability_evidence(evidence);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedMissingEvidence);
}

SNCF_TEST(fabric_evidence_expiry_and_future_dating) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  const Bare bare = register_bare(*fabric.value(), harness);

  CapabilityEvidenceRequest future = make_evidence(harness, bare);
  future.observed_at = TimestampNs::from_value(harness.clock().now().value() + 1'000'000'000ULL);
  auto future_refused = fabric.value()->submit_capability_evidence(future);
  SNCF_CHECK(!future_refused.ok());
  SNCF_CHECK(future_refused.code() == ReasonCode::RefusedInvalidRange);

  CapabilityEvidenceRequest stale = make_evidence(harness, bare);
  stale.observed_at = TimestampNs::from_value(0);
  auto stale_refused = fabric.value()->submit_capability_evidence(stale);
  SNCF_CHECK(!stale_refused.ok());
  SNCF_CHECK(stale_refused.code() == ReasonCode::RefusedExpiredEvidence);
}

SNCF_TEST(fabric_evidence_incarnation_replay_is_stale) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  const Bare bare = register_bare(*fabric.value(), harness);
  SNCF_REQUIRE(fabric.value()->submit_capability_evidence(make_evidence(harness, bare)).ok());

  CapabilityEvidenceRequest old_incarnation = make_evidence(harness, bare);
  old_incarnation.incarnation = DeviceIncarnation::from_value(0);
  auto refused = fabric.value()->submit_capability_evidence(old_incarnation);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedNilIdentity);

  RegisterDeviceRequest reincarnated;
  reincarnated.header = harness.next_header();
  reincarnated.smartnic = bare.smartnic;
  reincarnated.model = bare.model;
  reincarnated.incarnation = DeviceIncarnation::from_value(5);
  reincarnated.serial = "serial-0001";
  reincarnated.port = bare.port;
  SNCF_REQUIRE(fabric.value()->register_device(reincarnated).ok());
  auto stale = make_evidence(harness, bare);
  stale.header = harness.next_header();
  auto stale_refused = fabric.value()->submit_capability_evidence(stale);
  SNCF_CHECK(!stale_refused.ok());
  SNCF_CHECK(stale_refused.code() == ReasonCode::RefusedStaleDeviceIncarnation);
}

SNCF_TEST(fabric_evidence_gating_is_exact) {
  struct Case {
    const char* name;
    ReasonCode expected;
    std::function<void(CapabilityEvidenceRequest&)> mutate;
  };
  const std::vector<Case> cases{
      {"capability generation", ReasonCode::RefusedIncompatibleCapabilityGeneration,
       [](CapabilityEvidenceRequest& evidence) {
         evidence.capability_generation = CapabilityGeneration::from_value(1);
       }},
      {"compatibility generation", ReasonCode::RefusedIncompatibleCompatibilityGeneration,
       [](CapabilityEvidenceRequest& evidence) {
         evidence.compatibility_generation = CompatibilityGeneration::from_value(99);
       }},
      {"firmware", ReasonCode::RefusedIncompatibleFirmware,
       [](CapabilityEvidenceRequest& evidence) { evidence.firmware = SemVer{1, 0, 0}; }},
      {"device model", ReasonCode::RefusedUnsupportedDeviceModel,
       [](CapabilityEvidenceRequest& evidence) { evidence.model = DeviceModelId::from_value(9); }},
      {"required capability", ReasonCode::RefusedUnsupportedRequiredCapability,
       [](CapabilityEvidenceRequest& evidence) {
         evidence.capabilities = {CapabilityCode::RateLimiting};
       }},
  };
  for (const auto& test_case : cases) {
    Harness harness;
    auto fabric = harness.open();
    SNCF_REQUIRE(fabric.ok());
    const Bare bare = register_bare(*fabric.value(), harness, {CapabilityCode::PacketParsing});
    CapabilityEvidenceRequest evidence = make_evidence(harness, bare);
    test_case.mutate(evidence);
    SNCF_REQUIRE(fabric.value()->submit_capability_evidence(evidence).ok());
    ObservationRequest observation;
    observation.header = harness.next_header();
    observation.source = EvidenceSource::SyntheticFixture;
    observation.payload_digest = sncf_test::digest_of('c');
    observation.observed_at = harness.clock().now();
    observation.synthetic = true;
    observation.smartnic = bare.smartnic;
    observation.device = bare.device;
    observation.incarnation = bare.incarnation;
    observation.present = true;
    observation.capability_generation = evidence.capability_generation;
    observation.compatibility_generation = evidence.compatibility_generation;
    SNCF_REQUIRE(fabric.value()->submit_observation(observation).ok());

    // A model mismatch is evaluated against the device record, so feed the
    // evidence model through the device registration for that case.
    Bare target = bare;
    if (evidence.model != bare.model) {
      RegisterDeviceRequest reincarnated;
      reincarnated.header = harness.next_header();
      reincarnated.smartnic = bare.smartnic;
      reincarnated.model = evidence.model;
      reincarnated.incarnation = DeviceIncarnation::from_value(bare.incarnation.value() + 1);
      reincarnated.serial = "serial-0001";
      reincarnated.port = bare.port;
      SNCF_REQUIRE(fabric.value()->register_device(reincarnated).ok());
      target.incarnation = reincarnated.incarnation;
      CapabilityEvidenceRequest reincarnated_evidence = evidence;
      reincarnated_evidence.header = harness.next_header();
      reincarnated_evidence.incarnation = reincarnated.incarnation;
      SNCF_REQUIRE(fabric.value()->submit_capability_evidence(reincarnated_evidence).ok());
      ObservationRequest observed_again = observation;
      observed_again.header = harness.next_header();
      observed_again.incarnation = reincarnated.incarnation;
      SNCF_REQUIRE(fabric.value()->submit_observation(observed_again).ok());
    }
    ActivateRequest activation = make_activation(harness, target);
    auto refused = fabric.value()->activate(activation);
    SNCF_CHECK(!refused.ok());
    if (refused.code() != test_case.expected) {
      ::sncf_test::fail(__FILE__, __LINE__,
                        std::string("case ") + test_case.name + " expected " +
                            std::string(reason_name(test_case.expected)) + " but got " +
                            std::string(reason_name(refused.code())));
    }
  }
}

SNCF_TEST(fabric_evidence_survives_restart_only_as_pending) {
  Harness harness;
  Bare bare;
  {
    auto fabric = harness.open();
    SNCF_REQUIRE(fabric.ok());
    bare = register_bare(*fabric.value(), harness);
    SNCF_REQUIRE(fabric.value()->submit_capability_evidence(make_evidence(harness, bare)).ok());
    ObservationRequest observation;
    observation.header = harness.next_header();
    observation.source = EvidenceSource::SyntheticFixture;
    observation.payload_digest = sncf_test::digest_of('c');
    observation.observed_at = harness.clock().now();
    observation.synthetic = true;
    observation.smartnic = bare.smartnic;
    observation.device = bare.device;
    observation.incarnation = bare.incarnation;
    observation.present = true;
    observation.capability_generation = CapabilityGeneration::from_value(4);
    observation.compatibility_generation = CompatibilityGeneration::from_value(9);
    SNCF_REQUIRE(fabric.value()->submit_observation(observation).ok());
    fabric.value()->shutdown();
  }
  auto fabric = harness.reopen();
  SNCF_REQUIRE(fabric.ok());
  SNCF_CHECK(fabric.value()->counters().evidence_left_pending_reverification >= 1U);

  auto refused = fabric.value()->activate(make_activation(harness, bare));
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedEvidenceNotReverified);

  ReverifyEvidenceRequest reverify;
  reverify.header = harness.next_header();
  reverify.source = EvidenceSource::SyntheticFixture;
  reverify.payload_digest = sncf_test::digest_of('7');
  reverify.observed_at = harness.clock().now();
  reverify.device = bare.device;
  reverify.incarnation = bare.incarnation;
  auto reverified = fabric.value()->reverify_evidence(reverify);
  SNCF_REQUIRE(reverified.ok());
  SNCF_CHECK(reverified.value().freshness == Freshness::Fresh);

  // Presence is liveness, and liveness does not survive a restart: the old
  // observation is invalidated, so capability revalidation alone is not enough.
  auto still_absent = fabric.value()->activate(make_activation(harness, bare));
  SNCF_CHECK(!still_absent.ok());
  SNCF_CHECK(still_absent.code() == ReasonCode::RefusedEvidenceSourceUnavailable);

  ObservationRequest fresh;
  fresh.header = harness.next_header();
  fresh.source = EvidenceSource::SyntheticFixture;
  fresh.payload_digest = sncf_test::digest_of('8');
  fresh.observed_at = harness.clock().now();
  fresh.synthetic = true;
  fresh.smartnic = bare.smartnic;
  fresh.device = bare.device;
  fresh.incarnation = bare.incarnation;
  fresh.present = true;
  fresh.capability_generation = CapabilityGeneration::from_value(4);
  fresh.compatibility_generation = CompatibilityGeneration::from_value(9);
  SNCF_REQUIRE(fabric.value()->submit_observation(fresh).ok());

  auto accepted = fabric.value()->activate(make_activation(harness, bare));
  SNCF_REQUIRE(accepted.ok());
  SNCF_CHECK_EQ(fabric.value()->counters().evidence_reverified, 1U);
}

SNCF_TEST(fabric_plan_is_a_dry_run) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const Digest256 before = fabric.value()->durable_digest();
  const std::uint64_t accepted_before = fabric.value()->counters().commands_accepted;

  PlanRequest request;
  request.smartnic = topology.smartnic;
  request.device = topology.device;
  request.incarnation = topology.incarnation;
  request.package = topology.package;
  request.port = topology.port;
  request.queue = QueueId::from_value(1);
  auto plan = fabric.value()->plan_deployment(request);
  SNCF_REQUIRE(plan.ok());
  SNCF_CHECK(plan.value().verdict == PlanVerdict::CreatesInstance);
  SNCF_CHECK(plan.value().creates_instance);
  SNCF_CHECK(plan.value().primary_reason == ReasonCode::Accepted);
  SNCF_CHECK(!plan.value().steps.empty());
  SNCF_CHECK_EQ(fabric.value()->durable_digest().to_hex(), before.to_hex());
  // Planning is not a command: it neither mutates durable state nor counts.
  SNCF_CHECK_EQ(fabric.value()->counters().commands_accepted, accepted_before);

  // The plan is deterministic for identical input.
  auto again = fabric.value()->plan_deployment(request);
  SNCF_REQUIRE(again.ok());
  SNCF_CHECK_EQ(again.value().to_value().to_canonical(), plan.value().to_value().to_canonical());

  // A plan against a missing device reports the refusal and mutates nothing.
  PlanRequest missing = request;
  missing.device = DeviceId::from_value(999);
  auto refused = fabric.value()->plan_deployment(missing);
  SNCF_REQUIRE(refused.ok());
  SNCF_CHECK(refused.value().verdict == PlanVerdict::Refused);
  SNCF_CHECK(refused.value().primary_reason == ReasonCode::RefusedUnknownDevice);
  SNCF_CHECK_EQ(fabric.value()->durable_digest().to_hex(), before.to_hex());
}

SNCF_TEST(fabric_effect_reports_require_matching_evidence) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);

  EffectReportRequest report;
  report.header = harness.next_header();
  report.instance = activation.instance;
  report.attempt = activation.attempt;
  report.generation = activation.generation;
  report.token = FencingToken::from_value(activation.token.value() + 5);
  report.outcome = EffectOutcome::Applied;
  report.source = EvidenceSource::SyntheticFixture;
  report.payload_digest = sncf_test::digest_of('d');
  report.observed_at = harness.clock().now();
  auto stale_token = fabric.value()->report_effect(report);
  SNCF_CHECK(!stale_token.ok());
  SNCF_CHECK(stale_token.code() == ReasonCode::RefusedStaleFencingToken);

  report.header = harness.next_header();
  report.token = activation.token;
  report.generation = DeploymentGeneration::from_value(activation.generation.value() + 3);
  auto stale_generation = fabric.value()->report_effect(report);
  SNCF_CHECK(!stale_generation.ok());
  SNCF_CHECK(stale_generation.code() == ReasonCode::RefusedSupersededGeneration);

  report.header = harness.next_header();
  report.generation = activation.generation;
  report.attempt = AttemptId::from_value(activation.attempt.value() + 9);
  auto stale_attempt = fabric.value()->report_effect(report);
  SNCF_CHECK(!stale_attempt.ok());
  SNCF_CHECK(stale_attempt.code() == ReasonCode::RefusedAttemptNotCurrent);

  report.header = harness.next_header();
  report.attempt = activation.attempt;
  report.payload_digest = Digest256{};
  auto unbound = fabric.value()->report_effect(report);
  SNCF_CHECK(!unbound.ok());
  SNCF_CHECK(unbound.code() == ReasonCode::RefusedMissingEvidence);

  report.header = harness.next_header();
  report.payload_digest = sncf_test::digest_of('d');
  report.source = EvidenceSource::Operator;
  auto untrusted = fabric.value()->report_effect(report);
  SNCF_CHECK(!untrusted.ok());
  SNCF_CHECK(untrusted.code() == ReasonCode::RefusedEvidenceProvenanceUntrusted);

  report.header = harness.next_header();
  report.source = EvidenceSource::SyntheticFixture;
  auto accepted = fabric.value()->report_effect(report);
  SNCF_REQUIRE(accepted.ok());
  auto instance = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(instance.ok());
  SNCF_CHECK(instance.value().lifecycle == LifecycleState::Applied);
}

}  // namespace
