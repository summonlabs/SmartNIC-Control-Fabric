// Copyright 2026 Summon Software Labs.
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "sncf/fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace sncf;
using sncf_test::Harness;
using sncf_test::Topology;

SNCF_TEST(concurrency_parallel_registration_is_consistent) {
  FabricConfig config;
  config.max_smartnics = 512;
  config.max_journal_records = 8192;
  Harness harness(config);
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());

  constexpr int kThreads = 8;
  constexpr int kPerThread = 24;
  std::atomic<int> refusals{0};
  std::vector<std::thread> threads;
  std::atomic<std::uint64_t> next_command{1};
  for (int worker = 0; worker < kThreads; ++worker) {
    threads.emplace_back([&fabric, &refusals, &next_command, worker] {
      for (int index = 0; index < kPerThread; ++index) {
        RegisterSmartNicRequest request;
        request.header.command =
            CommandId::from_value(next_command.fetch_add(1, std::memory_order_relaxed));
        request.header.principal = PrincipalId::from_value(static_cast<std::uint64_t>(worker + 1));
        request.label = "nic-" + std::to_string(worker) + "-" + std::to_string(index);
        request.port_count = 1;
        auto result = fabric.value()->register_smartnic(request);
        if (!result.ok()) {
          refusals.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  SNCF_CHECK_EQ(refusals.load(), 0);
  auto export_value = fabric.value()->export_value();
  const Value* durable = export_value.find("durable");
  SNCF_REQUIRE(durable != nullptr);
  const Value* smartnics = durable->find("smartnics");
  SNCF_REQUIRE(smartnics != nullptr);
  SNCF_CHECK_EQ(smartnics->as_array().size(), static_cast<std::size_t>(kThreads * kPerThread));
  // Identities are dense and unique.
  std::uint64_t expected = 1;
  for (const auto& entry : smartnics->as_array()) {
    auto id = field_uint(entry, "id");
    SNCF_REQUIRE(id.ok());
    SNCF_CHECK_EQ(id.value(), expected);
    ++expected;
  }
}

SNCF_TEST(concurrency_duplicate_delivery_applies_once) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());

  constexpr int kThreads = 6;
  std::atomic<int> applied{0};
  std::atomic<int> duplicates{0};
  std::atomic<int> other{0};
  std::vector<std::thread> threads;
  RegisterSmartNicRequest shared;
  shared.header.command = CommandId::from_value(9001);
  shared.header.principal = PrincipalId::from_value(11);
  shared.label = "nic-shared";
  shared.port_count = 4;
  for (int worker = 0; worker < kThreads; ++worker) {
    threads.emplace_back([&] {
      auto result = fabric.value()->register_smartnic(shared);
      if (!result.ok()) {
        other.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (result.value().outcome.duplicate) {
        duplicates.fetch_add(1, std::memory_order_relaxed);
      } else {
        applied.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  SNCF_CHECK_EQ(applied.load(), 1);
  SNCF_CHECK_EQ(duplicates.load(), kThreads - 1);
  SNCF_CHECK_EQ(other.load(), 0);
  auto export_value = fabric.value()->export_value();
  const Value* durable = export_value.find("durable");
  SNCF_REQUIRE(durable != nullptr);
  const Value* smartnics = durable->find("smartnics");
  SNCF_REQUIRE(smartnics != nullptr);
  SNCF_CHECK_EQ(smartnics->as_array().size(), 1U);
}

SNCF_TEST(concurrency_mixed_ingest_and_query_stays_consistent) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);

  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 3; ++worker) {
    threads.emplace_back([&fabric, &errors, worker] {
      std::uint64_t command = 100000U + static_cast<std::uint64_t>(worker) * 1000U;
      for (int index = 0; index < 40; ++index) {
        ObservationRequest observation;
        observation.header.command = CommandId::from_value(command++);
        observation.header.principal = PrincipalId::from_value(5);
        observation.source = EvidenceSource::SyntheticFixture;
        observation.payload_digest = sncf_test::digest_of('c');
        // A clock that never advances keeps the report fresh and deterministic.
        observation.observed_at = TimestampNs::from_value(1'700'000'000'000'000'000ULL);
        observation.synthetic = true;
        observation.smartnic = SmartNicId::from_value(1);
        observation.device = DeviceId::from_value(1);
        observation.incarnation = DeviceIncarnation::from_value(1);
        observation.present = true;
        observation.capability_generation = CapabilityGeneration::from_value(4);
        observation.compatibility_generation = CompatibilityGeneration::from_value(9);
        if (!fabric.value()->submit_observation(observation).ok()) {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (int worker = 0; worker < 3; ++worker) {
    threads.emplace_back([&fabric, &errors, &activation] {
      for (int index = 0; index < 60; ++index) {
        auto instance = fabric.value()->inspect_instance(activation.instance);
        if (!instance.ok()) {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
        auto explanation = fabric.value()->explain_instance(activation.instance);
        if (!explanation.ok()) {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
        if (fabric.value()->durable_digest().is_zero()) {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  SNCF_CHECK_EQ(errors.load(), 0);
}

SNCF_TEST(concurrency_bounded_structures_evict_and_account) {
  FabricConfig config;
  config.max_events = 8;
  config.max_history = 4;
  config.max_dedupe_entries = 4;
  config.max_attempts_per_instance = 2;
  Harness harness(config);
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);

  for (int index = 0; index < 12; ++index) {
    ObservationRequest observation;
    observation.header = harness.next_header();
    observation.source = EvidenceSource::SyntheticFixture;
    observation.payload_digest = sncf_test::digest_of('c');
    observation.observed_at = harness.clock().now();
    observation.synthetic = true;
    observation.smartnic = topology.smartnic;
    observation.device = topology.device;
    observation.incarnation = topology.incarnation;
    observation.present = true;
    observation.capability_generation = topology.capability_generation;
    observation.compatibility_generation = topology.compatibility_generation;
    auto observed = fabric.value()->submit_observation(observation);
    if (!observed.ok()) {
      ::sncf_test::fail(__FILE__, __LINE__, std::string("observation refused: ") +
                                               std::string(reason_name(observed.code())) + " - " +
                                               observed.status().detail());
    }
  }

  auto instance = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(instance.ok());
  auto record = instance.value();
  // Settle the activation attempt so the first loop iteration starts clean.
  {
    EffectReportRequest settle;
    settle.header = harness.next_header();
    settle.instance = record.id;
    settle.attempt = record.current_attempt;
    settle.generation = record.deployment_generation;
    settle.token = record.token;
    settle.outcome = EffectOutcome::Applied;
    settle.source = EvidenceSource::SyntheticFixture;
    settle.payload_digest = sncf_test::digest_of('e');
    settle.observed_at = harness.clock().now();
    settle.synthetic = true;
    SNCF_REQUIRE(fabric.value()->report_effect(settle).ok());
    auto settled = fabric.value()->inspect_instance(activation.instance);
    SNCF_REQUIRE(settled.ok());
    record = settled.value();
  }
  for (int index = 0; index < 6; ++index) {
    ActivateRequest request;
    request.header = harness.next_header();
    request.smartnic = topology.smartnic;
    request.device = topology.device;
    request.incarnation = topology.incarnation;
    request.package = topology.package;
    request.port = topology.port;
    request.queue = QueueId::from_value(1);
    request.instance = activation.instance;
    request.claim = sncf_test::claim_for(record, *fabric.value());
    auto result = fabric.value()->activate(request);
    if (!result.ok()) {
      ::sncf_test::fail(__FILE__, __LINE__, std::string("activation refused: ") +
                                               std::string(reason_name(result.code())) + " - " +
                                               result.status().detail());
    }
    auto refreshed = fabric.value()->inspect_instance(activation.instance);
    SNCF_REQUIRE(refreshed.ok());
    record = refreshed.value();

    // Settle the attempt so the next activation is a fresh one rather than a
    // second in-flight attempt, which the configured bound refuses.
    EffectReportRequest settle;
    settle.header = harness.next_header();
    settle.instance = record.id;
    settle.attempt = record.current_attempt;
    settle.generation = record.deployment_generation;
    settle.token = record.token;
    settle.outcome = EffectOutcome::Applied;
    settle.source = EvidenceSource::SyntheticFixture;
    settle.payload_digest = sncf_test::digest_of('f');
    settle.observed_at = harness.clock().now();
    settle.synthetic = true;
    auto settled = fabric.value()->report_effect(settle);
    if (!settled.ok()) {
      ::sncf_test::fail(__FILE__, __LINE__, std::string("settle refused: ") +
                                               std::string(reason_name(settled.code())) + " - " +
                                               settled.status().detail());
    }
    auto after_settle = fabric.value()->inspect_instance(activation.instance);
    SNCF_REQUIRE(after_settle.ok());
    record = after_settle.value();
  }
  SNCF_CHECK(record.attempts.size() <= config.max_attempts_per_instance);
  const FabricCounters counters = fabric.value()->counters();
  SNCF_CHECK(counters.attempt_history_evictions > 0U);
  SNCF_CHECK(counters.events_evicted > 0U);
  SNCF_CHECK(counters.history_evictions > 0U);
  SNCF_CHECK(counters.dedupe_evictions > 0U);
  SNCF_CHECK(fabric.value()->events().size() <= config.max_events);
  SNCF_CHECK(fabric.value()->history().size() <= config.max_history);
}

SNCF_TEST(concurrency_sequential_open_close_cycles_leave_no_residue) {
  Harness harness;
  std::string digest;
  for (int cycle = 0; cycle < 5; ++cycle) {
    auto fabric = harness.open();
    SNCF_REQUIRE(fabric.ok());
    if (cycle == 0) {
      sncf_test::build_topology(*fabric.value(), harness);
    }
    const auto counters = fabric.value()->counters();
    SNCF_CHECK_EQ(counters.internal_apply_failures, 0U);
    digest = fabric.value()->durable_digest().to_hex();
    fabric.value()->shutdown();
    SNCF_CHECK(!fabric.value()->is_running());
  }
  auto fabric = harness.reopen();
  SNCF_REQUIRE(fabric.ok());
  SNCF_CHECK(!digest.empty());
  SNCF_CHECK(!fabric.value()->durable_digest().is_zero());
  SNCF_CHECK_EQ(fabric.value()->counters().journal_records_dropped_on_recovery, 0U);
}

}  // namespace
