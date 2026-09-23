// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <string>
#include <vector>

#include "sncf/fabric.hpp"
#include "sncf/model.hpp"
#include "sncf/version.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace sncf;
using sncf_test::Harness;

/// Deterministic 64-bit generator so every run of a script is reproducible.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}

  std::uint64_t next() {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 7U;
    state_ ^= state_ << 17U;
    return state_;
  }

  std::size_t below(std::size_t bound) { return bound == 0U ? 0U : static_cast<std::size_t>(next() % bound); }

 private:
  std::uint64_t state_;
};

struct BoundDevice {
  SmartNicId smartnic;
  DeviceId device;
  DeviceIncarnation incarnation;
};

/// Runs a seeded operation stream. The stream is a pure function of the seed, so
/// two Fabrics fed the same seed must reach byte-identical durable state.
void run_script(Fabric& fabric, Harness& harness, std::uint64_t seed, std::size_t steps,
                std::size_t compact_after) {
  Rng rng(seed);
  const DeviceModelId model = DeviceModelId::from_value(1);
  std::vector<SmartNicId> smartnics;
  std::vector<BoundDevice> devices;
  std::vector<FunctionPackageId> packages;
  std::vector<FunctionInstanceId> instances;
  std::uint64_t command = 1;

  const auto next_header = [&command] {
    CommandHeader value;
    value.command = CommandId::from_value(command++);
    value.principal = PrincipalId::from_value(3);
    return value;
  };

  for (std::size_t step = 0; step < steps; ++step) {
    const std::size_t choice = rng.below(9U);
    if (choice == 0U) {
      RegisterSmartNicRequest request;
      request.header = next_header();
      request.label = "nic-" + std::to_string(step);
      request.port_count = 8;
      auto result = fabric.register_smartnic(request);
      if (result.ok()) {
        smartnics.push_back(result.value().id);
      }
    } else if (choice == 1U && !smartnics.empty()) {
      RegisterDeviceRequest request;
      request.header = next_header();
      request.smartnic = smartnics[rng.below(smartnics.size())];
      request.model = model;
      request.incarnation = DeviceIncarnation::from_value(1);
      request.serial = "serial-" + std::to_string(step);
      request.port = PortId::from_value(static_cast<std::uint16_t>(1U + rng.below(8U)));
      auto result = fabric.register_device(request);
      if (result.ok()) {
        devices.push_back(BoundDevice{request.smartnic, result.value().id, request.incarnation});
      }
    } else if (choice == 2U) {
      RegisterPackageRequest request;
      request.header = next_header();
      request.name = "package-" + std::to_string(step);
      request.version = SemVer{1, 0, static_cast<std::uint16_t>(rng.below(4U))};
      request.digest = sncf_test::digest_of(static_cast<char>('a' + (step % 6U)));
      request.supported_models = {model};
      request.required_capabilities = {CapabilityCode::PacketParsing};
      request.min_capability_generation = CapabilityGeneration::from_value(1);
      request.compatibility.min = CompatibilityGeneration::from_value(1);
      request.compatibility.max = CompatibilityGeneration::from_value(10);
      request.min_firmware = SemVer{1, 0, 0};
      request.exclusive_scope = true;
      auto result = fabric.register_package(request);
      if (result.ok()) {
        packages.push_back(result.value().id);
      }
    } else if (choice == 3U && !devices.empty()) {
      const BoundDevice& bound = devices[rng.below(devices.size())];
      CapabilityEvidenceRequest request;
      request.header = next_header();
      request.source = EvidenceSource::SyntheticFixture;
      request.payload_digest = sncf_test::digest_of('b');
      request.observed_at = harness.clock().now();
      request.synthetic = true;
      request.smartnic = bound.smartnic;
      request.device = bound.device;
      request.incarnation = bound.incarnation;
      request.model = model;
      request.capability_generation = CapabilityGeneration::from_value(2);
      request.compatibility_generation = CompatibilityGeneration::from_value(3);
      request.firmware = SemVer{1, 5, 0};
      request.capabilities = {CapabilityCode::PacketParsing};
      (void)fabric.submit_capability_evidence(request);
    } else if (choice == 4U && !devices.empty()) {
      const BoundDevice& bound = devices[rng.below(devices.size())];
      ObservationRequest request;
      request.header = next_header();
      request.source = EvidenceSource::SyntheticFixture;
      request.payload_digest = sncf_test::digest_of('c');
      request.observed_at = harness.clock().now();
      request.synthetic = true;
      request.smartnic = bound.smartnic;
      request.device = bound.device;
      request.incarnation = bound.incarnation;
      request.present = rng.below(4U) != 0U;
      request.capability_generation = CapabilityGeneration::from_value(2);
      request.compatibility_generation = CompatibilityGeneration::from_value(3);
      (void)fabric.submit_observation(request);
    } else if (choice == 5U && !devices.empty() && !packages.empty()) {
      const BoundDevice& bound = devices[rng.below(devices.size())];
      ActivateRequest request;
      request.header = next_header();
      request.smartnic = bound.smartnic;
      request.device = bound.device;
      request.incarnation = bound.incarnation;
      request.package = packages[rng.below(packages.size())];
      request.port = PortId::from_value(static_cast<std::uint16_t>(1U + rng.below(8U)));
      request.queue = QueueId::from_value(1);
      auto result = fabric.activate(request);
      if (result.ok()) {
        instances.push_back(result.value().instance);
      }
    } else if (choice == 6U && !instances.empty()) {
      const FunctionInstanceId instance = instances[rng.below(instances.size())];
      auto record = fabric.inspect_instance(instance);
      if (record.ok()) {
        EffectReportRequest report;
        report.header = next_header();
        report.instance = instance;
        report.attempt = record.value().current_attempt;
        report.generation = record.value().deployment_generation;
        report.token = record.value().token;
        report.outcome = rng.below(2U) == 0U ? EffectOutcome::Applied : EffectOutcome::Failed;
        report.source = EvidenceSource::SyntheticFixture;
        report.payload_digest = sncf_test::digest_of('d');
        report.observed_at = harness.clock().now();
        report.synthetic = true;
        (void)fabric.report_effect(report);
      }
    } else if (choice == 7U && !instances.empty()) {
      const FunctionInstanceId instance = instances[rng.below(instances.size())];
      auto record = fabric.inspect_instance(instance);
      if (record.ok()) {
        QuiesceRequest request;
        request.header = next_header();
        request.instance = instance;
        request.claim.scope =
            ExclusiveScope{record.value().smartnic, record.value().device, record.value().port};
        request.claim.instance = instance;
        request.claim.lease = record.value().lease;
        request.claim.token = record.value().token;
        request.claim.epoch = fabric.epoch();
        request.claim.principal = PrincipalId::from_value(3);
        (void)fabric.request_quiesce(request);
      }
    } else {
      (void)fabric.compact();
    }
    if (compact_after != 0U && step + 1U == compact_after) {
      auto compacted = fabric.compact();
      SNCF_REQUIRE(compacted.ok());
    }
  }
}

SNCF_TEST(property_canonical_value_round_trip_is_total) {
  Rng rng(0x5EED1234ULL);
  for (int iteration = 0; iteration < 400; ++iteration) {
    Value::object_type fields;
    const std::size_t count = 1U + rng.below(6U);
    for (std::size_t index = 0; index < count; ++index) {
      const std::string key = "k" + std::to_string(index);
      switch (rng.below(5U)) {
        case 0U:
          fields.emplace_back(key, Value::uint_value(rng.next()));
          break;
        case 1U:
          fields.emplace_back(key, Value::int_value(-static_cast<std::int64_t>(rng.below(1000000U))));
          break;
        case 2U:
          fields.emplace_back(key, Value::boolean(rng.below(2U) == 0U));
          break;
        case 3U:
          fields.emplace_back(key, Value::string("value-" + std::to_string(rng.next())));
          break;
        default: {
          Value::array_type items;
          const std::size_t inner = rng.below(4U);
          for (std::size_t k = 0; k < inner; ++k) {
            items.push_back(Value::uint_value(rng.next()));
          }
          fields.emplace_back(key, Value::array(std::move(items)));
          break;
        }
      }
    }
    const Value original = Value::object(std::move(fields));
    const std::string text = original.to_canonical();
    auto parsed = parse_canonical(text);
    SNCF_REQUIRE(parsed.ok());
    SNCF_CHECK(parsed.value() == original);
    SNCF_CHECK_EQ(parsed.value().to_canonical(), text);
    SNCF_CHECK_EQ(parsed.value().digest().to_hex(), original.digest().to_hex());
  }
}

SNCF_TEST(property_scripted_state_is_deterministic) {
  Harness first;
  Harness second;
  auto first_fabric = first.open();
  auto second_fabric = second.open();
  SNCF_REQUIRE(first_fabric.ok());
  SNCF_REQUIRE(second_fabric.ok());
  run_script(*first_fabric.value(), first, 0xABCDEF01ULL, 60U, 0U);
  run_script(*second_fabric.value(), second, 0xABCDEF01ULL, 60U, 0U);
  SNCF_CHECK_EQ(first_fabric.value()->durable_digest().to_hex(),
                second_fabric.value()->durable_digest().to_hex());
  SNCF_CHECK_EQ(first_fabric.value()->export_canonical(), second_fabric.value()->export_canonical());
}

SNCF_TEST(property_snapshot_and_replay_preserve_every_field) {
  Harness plain;
  Harness compacted;
  auto plain_fabric = plain.open();
  auto compacted_fabric = compacted.open();
  SNCF_REQUIRE(plain_fabric.ok());
  SNCF_REQUIRE(compacted_fabric.ok());
  run_script(*plain_fabric.value(), plain, 0xC0FFEEULL, 48U, 0U);
  run_script(*compacted_fabric.value(), compacted, 0xC0FFEEULL, 48U, 16U);
  SNCF_CHECK(compacted_fabric.value()->counters().journal_compactions > 0U);
  SNCF_CHECK_EQ(plain_fabric.value()->durable_digest().to_hex(),
                compacted_fabric.value()->durable_digest().to_hex());
}

SNCF_TEST(property_reopening_a_quiet_store_is_stable) {
  Harness harness;
  {
    auto fabric = harness.open();
    SNCF_REQUIRE(fabric.ok());
    fabric.value()->shutdown();
  }
  std::string previous;
  for (int cycle = 0; cycle < 3; ++cycle) {
    auto fabric = harness.reopen();
    SNCF_REQUIRE(fabric.ok());
    const std::string digest = fabric.value()->durable_digest().to_hex();
    SNCF_CHECK(previous.empty() || previous != digest);
    previous = digest;
    fabric.value()->shutdown();
  }
  auto fabric = harness.reopen();
  SNCF_REQUIRE(fabric.ok());
  SNCF_CHECK(fabric.value()->counters().journal_records_replayed > 0U);
  SNCF_CHECK(recovery_is_usable(fabric.value()->recovery().disposition));
}

SNCF_TEST(property_export_parses_and_matches_schema) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  const std::string text = fabric.value()->export_canonical();
  auto parsed = parse_canonical(text);
  SNCF_REQUIRE(parsed.ok());
  auto schema = field_uint(parsed.value(), "schema");
  SNCF_REQUIRE(schema.ok());
  SNCF_CHECK_EQ(schema.value(), static_cast<std::uint64_t>(kExportSchemaVersion));
  auto digest = field_string(parsed.value(), "durable_digest");
  SNCF_REQUIRE(digest.ok());
  SNCF_CHECK_EQ(digest.value(), fabric.value()->durable_digest().to_hex());
  const Value* durable = parsed.value().find("durable");
  SNCF_REQUIRE(durable != nullptr);
  SNCF_CHECK_EQ(durable->digest().to_hex(), digest.value());
}

}  // namespace
