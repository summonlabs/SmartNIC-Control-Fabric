// Shared test scaffolding: unique temporary stores and a topology builder.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_TESTS_TEST_SUPPORT_HPP
#define SNCF_TESTS_TEST_SUPPORT_HPP

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "sncf/fabric.hpp"
#include "sncf/time.hpp"
#include "test_framework.hpp"

namespace sncf_test {

/// A unique temporary store directory that removes itself on destruction.
class TempDir {
 public:
  TempDir() {
    static std::atomic<std::uint64_t> counter{0};
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t index = counter.fetch_add(1, std::memory_order_relaxed);
    path_ = std::filesystem::temp_directory_path() /
            ("sncf-test-" + std::to_string(stamp) + "-" + std::to_string(index));
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_, error);
  }

  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

inline std::string hex_digest(char fill) {
  return std::string(64, fill);
}

inline sncf::Digest256 digest_of(char fill) {
  std::string text(64, fill);
  const auto parsed = sncf::Digest256::from_hex(text);
  SNCF_REQUIRE(parsed.has_value());
  return *parsed;
}

/// Wraps a Fabric with a manual clock and a monotonically increasing command
/// identity so tests never reuse a command by accident.
class Harness {
 public:
  explicit Harness(sncf::FabricConfig config = {}) : config_(config), store_() {}

  [[nodiscard]] sncf::Result<std::unique_ptr<sncf::Fabric>> open() {
    clock_ = std::make_shared<sncf::ManualClock>();
    sncf::Fabric::Options options;
    options.config = config_;
    options.store_directory = store_.path();
    options.clock = clock_;
    return sncf::Fabric::open(std::move(options));
  }

  /// Opens the store with the wall clock. Used when a separate operating-system
  /// process supplies timestamps, so both sides agree on the time base.
  [[nodiscard]] sncf::Result<std::unique_ptr<sncf::Fabric>> open_with_system_clock() {
    sncf::Fabric::Options options;
    options.config = config_;
    options.store_directory = store_.path();
    return sncf::Fabric::open(std::move(options));
  }

  /// Reopens the same store in the same process with a fresh clock reading.
  [[nodiscard]] sncf::Result<std::unique_ptr<sncf::Fabric>> reopen() {
    clock_ = std::make_shared<sncf::ManualClock>();
    sncf::Fabric::Options options;
    options.config = config_;
    options.store_directory = store_.path();
    options.clock = clock_;
    return sncf::Fabric::open(std::move(options));
  }

  [[nodiscard]] const std::filesystem::path& store() const noexcept { return store_.path(); }
  [[nodiscard]] const sncf::ManualClock& clock() const noexcept { return *clock_; }
  [[nodiscard]] sncf::ManualClock& clock() noexcept { return *clock_; }
  [[nodiscard]] const sncf::FabricConfig& config() const noexcept { return config_; }

  [[nodiscard]] sncf::CommandHeader next_header() {
    sncf::CommandHeader header;
    header.command = sncf::CommandId::from_value(next_command_++);
    header.principal = sncf::PrincipalId::from_value(7);
    return header;
  }

  [[nodiscard]] sncf::CommandHeader header_with(sncf::CommandId command,
                                                sncf::PrincipalId principal = sncf::PrincipalId::from_value(7)) {
    sncf::CommandHeader header;
    header.command = command;
    header.principal = principal;
    return header;
  }

  void advance(sncf::DurationNs delta) { SNCF_REQUIRE(clock_->advance(delta)); }

 private:
  sncf::FabricConfig config_;
  TempDir store_;
  std::shared_ptr<sncf::ManualClock> clock_;
  std::uint64_t next_command_ = 1;
};

/// A registered topology with accepted, fresh evidence. All fixtures are
/// SYNTHETIC: no real device or vendor runtime is involved.
struct Topology {
  sncf::SmartNicId smartnic;
  sncf::DeviceId device;
  sncf::FunctionPackageId package;
  sncf::DeviceModelId model = sncf::DeviceModelId::from_value(1);
  sncf::DeviceIncarnation incarnation = sncf::DeviceIncarnation::from_value(1);
  sncf::CapabilityGeneration capability_generation = sncf::CapabilityGeneration::from_value(4);
  sncf::CompatibilityGeneration compatibility_generation = sncf::CompatibilityGeneration::from_value(9);
  sncf::SemVer firmware{2, 5, 0};
  sncf::PortId port = sncf::PortId::from_value(1);
};

inline Topology build_topology(sncf::Fabric& fabric, Harness& harness,
                               const std::vector<sncf::CapabilityCode>& capabilities = {
                                   sncf::CapabilityCode::PacketParsing,
                                   sncf::CapabilityCode::QueueSteering}) {
  Topology topology;
  sncf::RegisterSmartNicRequest smartnic;
  smartnic.header = harness.next_header();
  smartnic.label = "nic-a";
  smartnic.port_count = 4;
  auto smartnic_result = fabric.register_smartnic(smartnic);
  SNCF_REQUIRE(smartnic_result.ok());
  topology.smartnic = smartnic_result.value().id;

  sncf::RegisterDeviceRequest device;
  device.header = harness.next_header();
  device.smartnic = topology.smartnic;
  device.model = topology.model;
  device.incarnation = topology.incarnation;
  device.serial = "serial-0001";
  device.port = topology.port;
  auto device_result = fabric.register_device(device);
  SNCF_REQUIRE(device_result.ok());
  topology.device = device_result.value().id;

  sncf::RegisterPackageRequest package;
  package.header = harness.next_header();
  package.name = "flow-classifier";
  package.version = sncf::SemVer{1, 4, 2};
  package.digest = digest_of('a');
  package.supported_models = {topology.model};
  package.required_capabilities = capabilities;
  package.min_capability_generation = sncf::CapabilityGeneration::from_value(2);
  package.compatibility.min = sncf::CompatibilityGeneration::from_value(5);
  package.compatibility.max = sncf::CompatibilityGeneration::from_value(12);
  package.min_firmware = sncf::SemVer{2, 0, 0};
  package.demand.ports = 1;
  package.demand.queues = 8;
  package.demand.memory_bytes = 1U << 20U;
  package.exclusive_scope = true;
  package.layout_revision = sncf::LayoutRevision::from_value(3);
  auto package_result = fabric.register_package(package);
  SNCF_REQUIRE(package_result.ok());
  topology.package = package_result.value().id;

  sncf::CapabilityEvidenceRequest evidence;
  evidence.header = harness.next_header();
  evidence.source = sncf::EvidenceSource::SyntheticFixture;
  evidence.payload_digest = digest_of('b');
  evidence.observed_at = harness.clock().now();
  evidence.synthetic = true;
  evidence.smartnic = topology.smartnic;
  evidence.device = topology.device;
  evidence.incarnation = topology.incarnation;
  evidence.model = topology.model;
  evidence.capability_generation = topology.capability_generation;
  evidence.compatibility_generation = topology.compatibility_generation;
  evidence.firmware = topology.firmware;
  evidence.capabilities = capabilities;
  auto evidence_result = fabric.submit_capability_evidence(evidence);
  SNCF_REQUIRE(evidence_result.ok());

  sncf::ObservationRequest observation;
  observation.header = harness.next_header();
  observation.source = sncf::EvidenceSource::SyntheticFixture;
  observation.payload_digest = digest_of('c');
  observation.observed_at = harness.clock().now();
  observation.synthetic = true;
  observation.smartnic = topology.smartnic;
  observation.device = topology.device;
  observation.incarnation = topology.incarnation;
  observation.present = true;
  observation.capability_generation = topology.capability_generation;
  observation.compatibility_generation = topology.compatibility_generation;
  auto observation_result = fabric.submit_observation(observation);
  SNCF_REQUIRE(observation_result.ok());
  return topology;
}

/// Activates a new instance under a freshly granted lease.
struct Activation {
  sncf::FunctionInstanceId instance;
  sncf::AttemptId attempt;
  sncf::LeaseId lease;
  sncf::FencingToken token;
  sncf::DeploymentGeneration generation;
};

inline Activation activate_new_instance(sncf::Fabric& fabric, Harness& harness, const Topology& topology,
                                        sncf::PortId port = sncf::PortId::from_value(1),
                                        std::uint64_t command_seed = 0) {
  (void)command_seed;
  sncf::ActivateRequest request;
  request.header = harness.next_header();
  request.smartnic = topology.smartnic;
  request.device = topology.device;
  request.incarnation = topology.incarnation;
  request.package = topology.package;
  request.port = port;
  request.queue = sncf::QueueId::from_value(1);
  auto result = fabric.activate(request);
  SNCF_REQUIRE(result.ok());
  Activation activation;
  activation.instance = result.value().instance;
  activation.attempt = result.value().attempt;
  activation.lease = result.value().lease;
  activation.token = result.value().token;
  activation.generation = result.value().generation;
  return activation;
}

inline sncf::AuthorityClaim claim_for(const sncf::InstanceRecord& instance, const sncf::Fabric& fabric,
                                      sncf::PrincipalId principal = sncf::PrincipalId::from_value(7)) {
  sncf::AuthorityClaim claim;
  claim.scope = sncf::ExclusiveScope{instance.smartnic, instance.device, instance.port};
  claim.instance = instance.id;
  claim.lease = instance.lease;
  claim.token = instance.token;
  claim.epoch = fabric.epoch();
  claim.principal = principal;
  return claim;
}

inline sncf::CommandOutcome report_verified(sncf::Fabric& fabric, Harness& harness,
                                            const Activation& activation) {
  sncf::EffectReportRequest report;
  report.header = harness.next_header();
  report.instance = activation.instance;
  report.attempt = activation.attempt;
  report.generation = activation.generation;
  report.token = activation.token;
  report.outcome = sncf::EffectOutcome::Verified;
  report.source = sncf::EvidenceSource::SyntheticFixture;
  report.payload_digest = digest_of('d');
  report.observed_at = harness.clock().now();
  report.synthetic = true;
  report.detail = "synthetic verification";
  auto result = fabric.report_effect(report);
  if (!result.ok()) {
    ::sncf_test::fail(__FILE__, __LINE__, std::string("effect report refused: ") +
                                             std::string(sncf::reason_name(result.code())) + " - " +
                                             result.status().detail());
  }
  return result.value();
}

}  // namespace sncf_test

#endif  // SNCF_TESTS_TEST_SUPPORT_HPP
