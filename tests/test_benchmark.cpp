// Benchmarks that measure completed work.
//
// Each benchmark drives a fixed number of operations to completion (each one
// durably committed before the next begins) and then asserts that exactly that
// many completed. Enqueue latency is deliberately not measured: a queue that
// accepts work and never finishes it would look fast and prove nothing.
//
// Copyright 2026 Summon Software Labs.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "sncf/fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace sncf;
using sncf_test::Harness;
using sncf_test::Topology;

double seconds_between(std::chrono::steady_clock::time_point start,
                       std::chrono::steady_clock::time_point finish) {
  return std::chrono::duration<double>(finish - start).count();
}

SNCF_TEST(benchmark_durable_registrations_measure_completed_work) {
  FabricConfig config;
  config.max_smartnics = 2048;
  config.max_journal_records = 8192;
  Harness harness(config);
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());

  constexpr int kOperations = 400;
  std::vector<SmartNicId> completed;
  completed.reserve(kOperations);
  const auto start = std::chrono::steady_clock::now();
  for (int index = 0; index < kOperations; ++index) {
    RegisterSmartNicRequest request;
    request.header = harness.next_header();
    request.label = "bench-" + std::to_string(index);
    request.port_count = 1;
    auto result = fabric.value()->register_smartnic(request);
    SNCF_REQUIRE(result.ok());
    // Completion means the decision is durable and the identity is published,
    // not that the request was accepted into a queue.
    SNCF_CHECK(result.value().outcome.sequence.valid());
    completed.push_back(result.value().id);
  }
  const auto finish = std::chrono::steady_clock::now();

  SNCF_CHECK_EQ(completed.size(), static_cast<std::size_t>(kOperations));
  SNCF_CHECK_EQ(fabric.value()->counters().commands_accepted, static_cast<std::uint64_t>(kOperations));
  SNCF_CHECK_EQ(fabric.value()->counters().journal_records_appended,
                static_cast<std::uint64_t>(kOperations) + 1U);  // plus the recovery epoch record

  const Value exported = fabric.value()->export_value();
  const Value* durable = exported.find("durable");
  SNCF_REQUIRE(durable != nullptr);
  const Value* smartnics = durable->find("smartnics");
  SNCF_REQUIRE(smartnics != nullptr);
  SNCF_CHECK_EQ(smartnics->as_array().size(), static_cast<std::size_t>(kOperations));

  const double elapsed = seconds_between(start, finish);
  std::printf("            %d durable registrations completed in %.3f s (%.0f completed ops/s)\n",
              kOperations, elapsed, elapsed > 0.0 ? static_cast<double>(kOperations) / elapsed : 0.0);
  SNCF_CHECK(elapsed > 0.0);

  // Every completed identity survives a cold reopen, which is what makes the
  // throughput number a throughput of finished work.
  fabric.value()->shutdown();
  auto reopened = harness.reopen();
  SNCF_REQUIRE(reopened.ok());
  const Value reopened_export = reopened.value()->export_value();
  const Value* reopened_durable = reopened_export.find("durable");
  SNCF_REQUIRE(reopened_durable != nullptr);
  const Value* reopened_smartnics = reopened_durable->find("smartnics");
  SNCF_REQUIRE(reopened_smartnics != nullptr);
  SNCF_CHECK_EQ(reopened_smartnics->as_array().size(), static_cast<std::size_t>(kOperations));
}

SNCF_TEST(benchmark_exclusive_scope_decisions_measure_completed_work) {
  FabricConfig config;
  config.max_smartnics = 256;
  config.max_devices = 512;
  config.max_journal_records = 8192;
  Harness harness(config);
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());

  RegisterSmartNicRequest smartnic;
  smartnic.header = harness.next_header();
  smartnic.label = "bench-nic";
  smartnic.port_count = 64;
  auto smartnic_result = fabric.value()->register_smartnic(smartnic);
  SNCF_REQUIRE(smartnic_result.ok());

  constexpr int kDevices = 48;
  std::uint64_t accepted = 0;
  std::uint64_t refused = 0;
  const auto start = std::chrono::steady_clock::now();
  for (int index = 0; index < kDevices; ++index) {
    RegisterDeviceRequest device;
    device.header = harness.next_header();
    device.smartnic = smartnic_result.value().id;
    device.model = DeviceModelId::from_value(1);
    device.incarnation = DeviceIncarnation::from_value(1);
    device.serial = "bench-serial-" + std::to_string(index);
    device.port = PortId::from_value(static_cast<std::uint16_t>(index + 1));
    auto registered = fabric.value()->register_device(device);
    SNCF_REQUIRE(registered.ok());

    // Contend for the same exclusive scope twice: the first attempt completes,
    // the second is refused with a stable code.
    RegisterPackageRequest package;
    package.header = harness.next_header();
    package.name = "bench-package-" + std::to_string(index);
    package.version = SemVer{1, 0, 0};
    package.digest = sncf_test::digest_of('a');
    package.supported_models = {DeviceModelId::from_value(1)};
    package.required_capabilities = {CapabilityCode::PacketParsing};
    package.min_capability_generation = CapabilityGeneration::from_value(1);
    package.compatibility.min = CompatibilityGeneration::from_value(1);
    package.compatibility.max = CompatibilityGeneration::from_value(8);
    package.min_firmware = SemVer{1, 0, 0};
    package.exclusive_scope = true;
    auto package_result = fabric.value()->register_package(package);
    SNCF_REQUIRE(package_result.ok());

    CapabilityEvidenceRequest evidence;
    evidence.header = harness.next_header();
    evidence.source = EvidenceSource::SyntheticFixture;
    evidence.payload_digest = sncf_test::digest_of('b');
    evidence.observed_at = harness.clock().now();
    evidence.synthetic = true;
    evidence.smartnic = smartnic_result.value().id;
    evidence.device = registered.value().id;
    evidence.incarnation = device.incarnation;
    evidence.model = device.model;
    evidence.capability_generation = CapabilityGeneration::from_value(2);
    evidence.compatibility_generation = CompatibilityGeneration::from_value(2);
    evidence.firmware = SemVer{1, 4, 0};
    evidence.capabilities = {CapabilityCode::PacketParsing};
    SNCF_REQUIRE(fabric.value()->submit_capability_evidence(evidence).ok());

    ObservationRequest observation;
    observation.header = harness.next_header();
    observation.source = EvidenceSource::SyntheticFixture;
    observation.payload_digest = sncf_test::digest_of('c');
    observation.observed_at = harness.clock().now();
    observation.synthetic = true;
    observation.smartnic = smartnic_result.value().id;
    observation.device = registered.value().id;
    observation.incarnation = device.incarnation;
    observation.present = true;
    observation.capability_generation = evidence.capability_generation;
    observation.compatibility_generation = evidence.compatibility_generation;
    SNCF_REQUIRE(fabric.value()->submit_observation(observation).ok());

    ActivateRequest activation;
    activation.header = harness.next_header();
    activation.smartnic = smartnic_result.value().id;
    activation.device = registered.value().id;
    activation.incarnation = device.incarnation;
    activation.package = package_result.value().id;
    activation.port = device.port;
    activation.queue = QueueId::from_value(1);
    auto granted = fabric.value()->activate(activation);
    SNCF_REQUIRE(granted.ok());
    ++accepted;

    activation.header = harness.next_header();
    activation.queue = QueueId::from_value(2);
    auto conflict = fabric.value()->activate(activation);
    SNCF_CHECK(!conflict.ok());
    SNCF_CHECK(conflict.code() == ReasonCode::RefusedScopeConflict);
    ++refused;
  }
  const auto finish = std::chrono::steady_clock::now();

  SNCF_CHECK_EQ(accepted, static_cast<std::uint64_t>(kDevices));
  SNCF_CHECK_EQ(refused, static_cast<std::uint64_t>(kDevices));
  SNCF_CHECK_EQ(fabric.value()->counters().instances_created, static_cast<std::uint64_t>(kDevices));
  const double elapsed = seconds_between(start, finish);
  std::printf("            %llu activations and %llu refusals completed in %.3f s (%.0f completed ops/s)\n",
              static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(refused), elapsed,
              elapsed > 0.0 ? static_cast<double>(accepted + refused) / elapsed : 0.0);
  SNCF_CHECK(elapsed > 0.0);
}

SNCF_TEST(regression_evidence_freshness_stays_pending_across_restarts) {
  Harness harness;
  {
    auto fabric = harness.open();
    SNCF_REQUIRE(fabric.ok());
    sncf_test::build_topology(*fabric.value(), harness);
    fabric.value()->shutdown();
  }
  for (int cycle = 0; cycle < 3; ++cycle) {
    auto fabric = harness.reopen();
    SNCF_REQUIRE(fabric.ok());
    const Value exported = fabric.value()->export_value();
    const Value* durable = exported.find("durable");
    SNCF_REQUIRE(durable != nullptr);
    const Value* evidence = durable->find("capability_evidence");
    SNCF_REQUIRE(evidence != nullptr);
    SNCF_REQUIRE(!evidence->as_array().empty());
    const Value* freshness = evidence->as_array()[0].find("envelope");
    SNCF_REQUIRE(freshness != nullptr);
    auto text = field_string(*freshness, "freshness");
    SNCF_REQUIRE(text.ok());
    // It must never decay to "unknown": the reason it cannot be used has to
    // survive every restart.
    SNCF_CHECK_EQ(text.value(), std::string("pending_reverification"));
    fabric.value()->shutdown();
  }
}

SNCF_TEST(regression_effect_claims_carry_the_enforcement_digest) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);
  sncf_test::report_verified(*fabric.value(), harness, activation);

  auto instance = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(instance.ok());
  SNCF_CHECK(!instance.value().last_effect_digest.is_zero());
  SNCF_CHECK_EQ(instance.value().last_effect_digest.to_hex(), sncf_test::digest_of('d').to_hex());
  SNCF_REQUIRE(!instance.value().attempts.empty());
  SNCF_CHECK_EQ(instance.value().attempts.back().effect_digest.to_hex(), sncf_test::digest_of('d').to_hex());

  // The digest survives persistence and replay unchanged.
  fabric.value()->shutdown();
  auto reopened = harness.reopen();
  SNCF_REQUIRE(reopened.ok());
  auto replayed = reopened.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(replayed.ok());
  SNCF_CHECK_EQ(replayed.value().last_effect_digest.to_hex(), sncf_test::digest_of('d').to_hex());

  auto explanation = reopened.value()->explain_instance(activation.instance);
  SNCF_REQUIRE(explanation.ok());
  bool carries_digest = false;
  for (const auto& factor : explanation.value().factors) {
    if (factor.has_evidence && factor.evidence_digest.to_hex() == sncf_test::digest_of('d').to_hex()) {
      carries_digest = true;
    }
  }
  SNCF_CHECK(carries_digest);
}

}  // namespace
