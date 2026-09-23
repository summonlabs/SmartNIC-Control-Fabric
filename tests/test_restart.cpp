// Copyright 2026 Summon Software Labs.
#include <cstdlib>
#include <filesystem>
#include <string>

#include "sncf/crash.hpp"
#include "sncf/fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace sncf;

/// Opens the store and registers a complete synthetic topology. Returns the
/// identifiers the crash writer needs; refusing is fatal for the child.
struct WriterState {
  std::unique_ptr<Fabric> fabric;
  SmartNicId smartnic;
  DeviceId device;
  FunctionPackageId package;
  PortId port = PortId::from_value(1);
  DeviceIncarnation incarnation = DeviceIncarnation::from_value(1);
};

WriterState writer_setup(const std::string& store) {
  WriterState state;
  Fabric::Options options;
  options.store_directory = store;
  options.sync_on_commit = true;
  auto fabric = Fabric::open(std::move(options));
  if (!fabric.ok()) {
    std::_Exit(90);
  }
  state.fabric = std::move(fabric.value());
  std::uint64_t command = 1;
  const auto header = [&command] {
    CommandHeader value;
    value.command = CommandId::from_value(command++);
    value.principal = PrincipalId::from_value(1);
    return value;
  };
  RegisterSmartNicRequest smartnic;
  smartnic.header = header();
  smartnic.label = "crash-nic";
  smartnic.port_count = 2;
  auto smartnic_result = state.fabric->register_smartnic(smartnic);
  if (!smartnic_result.ok()) {
    std::_Exit(91);
  }
  state.smartnic = smartnic_result.value().id;

  RegisterDeviceRequest device;
  device.header = header();
  device.smartnic = state.smartnic;
  device.model = DeviceModelId::from_value(1);
  device.incarnation = state.incarnation;
  device.serial = "crash-serial";
  device.port = state.port;
  auto device_result = state.fabric->register_device(device);
  if (!device_result.ok()) {
    std::_Exit(92);
  }
  state.device = device_result.value().id;

  RegisterPackageRequest package;
  package.header = header();
  package.name = "crash-package";
  package.version = SemVer{1, 0, 0};
  package.digest = sncf_test::digest_of('a');
  package.supported_models = {DeviceModelId::from_value(1)};
  package.required_capabilities = {CapabilityCode::PacketParsing};
  package.min_capability_generation = CapabilityGeneration::from_value(1);
  package.compatibility.min = CompatibilityGeneration::from_value(1);
  package.compatibility.max = CompatibilityGeneration::from_value(8);
  package.min_firmware = SemVer{1, 0, 0};
  package.exclusive_scope = true;
  auto package_result = state.fabric->register_package(package);
  if (!package_result.ok()) {
    std::_Exit(93);
  }
  state.package = package_result.value().id;

  CapabilityEvidenceRequest evidence;
  evidence.header = header();
  evidence.source = EvidenceSource::SyntheticFixture;
  evidence.payload_digest = sncf_test::digest_of('b');
  evidence.observed_at = SystemClock{}.now();
  evidence.synthetic = true;
  evidence.smartnic = state.smartnic;
  evidence.device = state.device;
  evidence.incarnation = state.incarnation;
  evidence.model = DeviceModelId::from_value(1);
  evidence.capability_generation = CapabilityGeneration::from_value(3);
  evidence.compatibility_generation = CompatibilityGeneration::from_value(4);
  evidence.firmware = SemVer{1, 2, 0};
  evidence.capabilities = {CapabilityCode::PacketParsing};
  if (!state.fabric->submit_capability_evidence(evidence).ok()) {
    std::_Exit(94);
  }
  ObservationRequest observation;
  observation.header = header();
  observation.source = EvidenceSource::SyntheticFixture;
  observation.payload_digest = sncf_test::digest_of('c');
  observation.observed_at = SystemClock{}.now();
  observation.synthetic = true;
  observation.smartnic = state.smartnic;
  observation.device = state.device;
  observation.incarnation = state.incarnation;
  observation.present = true;
  observation.capability_generation = CapabilityGeneration::from_value(3);
  observation.compatibility_generation = CompatibilityGeneration::from_value(4);
  if (!state.fabric->submit_observation(observation).ok()) {
    std::_Exit(95);
  }
  return state;
}

ActivationGrant writer_activate(WriterState& state, std::uint64_t& command) {
  ActivateRequest request;
  request.header.command = CommandId::from_value(command++);
  request.header.principal = PrincipalId::from_value(1);
  request.smartnic = state.smartnic;
  request.device = state.device;
  request.incarnation = state.incarnation;
  request.package = state.package;
  request.port = state.port;
  request.queue = QueueId::from_value(1);
  auto result = state.fabric->activate(request);
  if (!result.ok()) {
    std::_Exit(96);
  }
  return result.value();
}

void writer_verify(WriterState& state, std::uint64_t& command, const ActivationGrant& grant) {
  EffectReportRequest report;
  report.header.command = CommandId::from_value(command++);
  report.header.principal = PrincipalId::from_value(1);
  report.instance = grant.instance;
  report.attempt = grant.attempt;
  report.generation = grant.generation;
  report.token = grant.token;
  report.outcome = EffectOutcome::Verified;
  report.source = EvidenceSource::SyntheticFixture;
  report.payload_digest = sncf_test::digest_of('d');
  report.observed_at = SystemClock{}.now();
  report.synthetic = true;
  if (!state.fabric->report_effect(report).ok()) {
    std::_Exit(97);
  }
}

}  // namespace

/// Child-process entry point: performs a fixed workload and is terminated hard at
/// the requested durable boundary. Never returns when the boundary fires.
int sncf_run_crash_writer(const std::string& store, const std::string& point) {
  const std::string mode = point;
  arm_crash_point(CrashPoint::None);
  WriterState state = writer_setup(store);
  std::uint64_t command = 100;

  if (mode == "journal_before_write" || mode == "journal_after_write_before_sync" ||
      mode == "journal_after_sync_before_ack") {
    if (mode == "journal_before_write") {
      arm_crash_point(CrashPoint::JournalBeforeWrite);
    } else if (mode == "journal_after_write_before_sync") {
      arm_crash_point(CrashPoint::JournalAfterWriteBeforeSync);
    } else {
      arm_crash_point(CrashPoint::JournalAfterSyncBeforeAck);
    }
    (void)writer_activate(state, command);
    return 0;  // unreachable when the boundary fires
  }
  if (mode == "snapshot_before_rename" || mode == "snapshot_after_rename_before_journal_rewrite") {
    const auto grant = writer_activate(state, command);
    writer_verify(state, command, grant);
    arm_crash_point(mode == "snapshot_before_rename" ? CrashPoint::SnapshotBeforeRename
                                                     : CrashPoint::SnapshotAfterRenameBeforeJournalRewrite);
    (void)state.fabric->compact();
    return 0;
  }
  if (mode == "shutdown_before_close") {
    const auto grant = writer_activate(state, command);
    writer_verify(state, command, grant);
    arm_crash_point(CrashPoint::ShutdownBeforeClose);
    state.fabric->shutdown();
    return 0;
  }
  if (mode == "hard_exit_after_commit") {
    (void)writer_activate(state, command);
    // Terminate without unwinding, flushing or closing anything.
    std::_Exit(0);
  }
  if (mode == "clean") {
    const auto grant = writer_activate(state, command);
    writer_verify(state, command, grant);
    state.fabric->shutdown();
    return 0;
  }
  std::_Exit(98);
}

namespace {

using sncf_test::Harness;

struct StoreFacts {
  bool instance_exists = false;
  bool verified = false;
  bool ambiguous = false;
  bool authority_granted = false;
  std::uint64_t attempt_phase_ambiguous = 0;
  std::uint64_t epoch = 0;
  std::size_t packages = 0;
};

StoreFacts inspect_store(const std::filesystem::path& store) {
  StoreFacts facts;
  Fabric::Options options;
  options.store_directory = store;
  auto fabric = Fabric::open(std::move(options));
  SNCF_REQUIRE(fabric.ok());
  facts.epoch = fabric.value()->epoch().value();
  const Value exported = fabric.value()->export_value();
  const Value* durable = exported.find("durable");
  SNCF_REQUIRE(durable != nullptr);
  const Value* packages = durable->find("packages");
  if (packages != nullptr) {
    facts.packages = packages->as_array().size();
  }
  const Value* instances = durable->find("instances");
  SNCF_REQUIRE(instances != nullptr);
  if (instances->as_array().empty()) {
    return facts;
  }
  auto instance = instance_from_value(instances->as_array()[0]);
  SNCF_REQUIRE(instance.ok());
  facts.instance_exists = true;
  facts.verified = instance.value().lifecycle == LifecycleState::Verified;
  facts.ambiguous = instance.value().pending_intent_ambiguous;
  facts.authority_granted = instance.value().authority == AuthorityState::Granted;
  for (const auto& attempt : instance.value().attempts) {
    if (attempt.phase == AttemptPhase::Ambiguous) {
      ++facts.attempt_phase_ambiguous;
    }
  }
  fabric.value()->shutdown();
  return facts;
}

SNCF_TEST(restart_child_process_is_killed_at_every_durable_boundary) {
  const struct {
    const char* point;
    bool expect_instance;
    bool expect_ambiguous;
    bool expect_verified;
  } cases[] = {
      {"journal_before_write", false, false, false},
      {"journal_after_write_before_sync", true, true, false},
      {"journal_after_sync_before_ack", true, true, false},
      {"hard_exit_after_commit", true, true, false},
      {"snapshot_before_rename", true, false, true},
      {"snapshot_after_rename_before_journal_rewrite", true, false, true},
      {"shutdown_before_close", true, false, true},
  };
  for (const auto& test_case : cases) {
    sncf_test::TempDir directory;
    const int status = sncf_test::run_self_child(
        {"--crash-writer", directory.path().string(), test_case.point});
    // The writer returns 0 only when it fell through without hitting the
    // boundary; a hard crash reports the crash code or, for _Exit(0), zero.
    if (status != 0 && (status < 70 || status > 76)) {
      ::sncf_test::fail(__FILE__, __LINE__,
                        std::string("child failed for ") + test_case.point + " with status " +
                            std::to_string(status));
    }
    const StoreFacts facts = inspect_store(directory.path());
    if (facts.instance_exists != test_case.expect_instance) {
      ::sncf_test::fail(__FILE__, __LINE__,
                        std::string("instance existence mismatch for ") + test_case.point);
    }
    if (facts.instance_exists) {
      if (facts.verified != test_case.expect_verified) {
        ::sncf_test::fail(__FILE__, __LINE__,
                          std::string("verified mismatch for ") + test_case.point);
      }
      SNCF_CHECK(!facts.authority_granted);
      if (facts.ambiguous != test_case.expect_ambiguous) {
        ::sncf_test::fail(__FILE__, __LINE__,
                          std::string("ambiguity mismatch for ") + test_case.point);
      }
    }
    SNCF_CHECK_EQ(facts.packages, 1U);
    SNCF_CHECK(facts.epoch > 0U);
  }
}

SNCF_TEST(restart_ambiguity_is_resolved_only_by_enforcement_evidence) {
  sncf_test::TempDir directory;
  SNCF_CHECK_EQ(sncf_test::run_self_child({"--crash-writer", directory.path().string(),
                                           "journal_after_sync_before_ack"}),
                70 + static_cast<int>(CrashPoint::JournalAfterSyncBeforeAck));
  const StoreFacts facts = inspect_store(directory.path());
  SNCF_CHECK(facts.instance_exists);
  SNCF_CHECK(!facts.verified);
  SNCF_CHECK(facts.ambiguous);
  SNCF_CHECK_EQ(facts.attempt_phase_ambiguous, 1U);

  Fabric::Options options;
  options.store_directory = directory.path();
  auto fabric = Fabric::open(std::move(options));
  SNCF_REQUIRE(fabric.ok());
  const Value exported = fabric.value()->export_value();
  const Value* durable = exported.find("durable");
  SNCF_REQUIRE(durable != nullptr);
  const Value* instances = durable->find("instances");
  SNCF_REQUIRE(instances != nullptr);
  auto instance = instance_from_value(instances->as_array()[0]);
  SNCF_REQUIRE(instance.ok());

  // A report with a stale token cannot resolve anything.
  EffectReportRequest stale;
  stale.header.command = CommandId::from_value(1000);
  stale.header.principal = PrincipalId::from_value(1);
  stale.instance = instance.value().id;
  stale.attempt = instance.value().current_attempt;
  stale.generation = instance.value().deployment_generation;
  stale.token = FencingToken::from_value(instance.value().token.value() + 1U);
  stale.outcome = EffectOutcome::Applied;
  stale.source = EvidenceSource::SyntheticFixture;
  stale.payload_digest = sncf_test::digest_of('e');
  stale.observed_at = SystemClock{}.now();
  stale.synthetic = true;
  auto refused = fabric.value()->report_effect(stale);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedStaleFencingToken);

  // The matching enforcement-side report resolves the ambiguity. The outcome is
  // whatever the executor says it was; the runtime never assumes one.
  auto record = fabric.value()->inspect_instance(instance.value().id);
  SNCF_REQUIRE(record.ok());
  stale.header.command = CommandId::from_value(1001);
  stale.token = record.value().token;
  stale.outcome = EffectOutcome::Failed;
  auto accepted = fabric.value()->report_effect(stale);
  SNCF_REQUIRE(accepted.ok());
  auto resolved = fabric.value()->inspect_instance(instance.value().id);
  SNCF_REQUIRE(resolved.ok());
  SNCF_CHECK(!resolved.value().pending_intent_ambiguous);
  SNCF_CHECK(resolved.value().lifecycle == LifecycleState::Failed);
  SNCF_CHECK(resolved.value().authority != AuthorityState::Granted);
  fabric.value()->shutdown();
}

SNCF_TEST(restart_clean_child_reopens_with_matching_structure) {
  sncf_test::TempDir directory;
  SNCF_CHECK_EQ(sncf_test::run_self_child({"--crash-writer", directory.path().string(), "clean"}), 0);
  const StoreFacts facts = inspect_store(directory.path());
  SNCF_CHECK(facts.instance_exists);
  SNCF_CHECK(facts.verified);
  SNCF_CHECK(!facts.ambiguous);
  SNCF_CHECK(!facts.authority_granted);
}

}  // namespace
