// Regression tests for the defects found by the adversarial audit and fixed
// during hardening. Each one fails against the pre-fix behaviour.
//
// Copyright 2026 Summon Software Labs.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "sncf/crash.hpp"
#include "sncf/fabric.hpp"
#include "sncf/persistence.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace sncf;
using sncf_test::Harness;
using sncf_test::Topology;

/// A well-formed journal record document, so the store is exercised exactly as
/// the runtime would write it.
Value journal_record_value(std::uint64_t sequence) {
  SmartNicRecord record;
  record.id = SmartNicId::from_value(sequence);
  record.label = "hardening-nic";
  record.port_count = 1;
  record.registered_at = TimestampNs::from_value(1000U + sequence);
  JournalRecord journal;
  journal.sequence = RecordSequence::from_value(sequence);
  journal.kind = RecordKind::SmartNicRegistered;
  journal.command = CommandId::from_value(sequence);
  journal.principal = PrincipalId::from_value(1);
  journal.epoch = CoordinatorEpoch::from_value(1);
  journal.at = TimestampNs::from_value(1000U + sequence);
  journal.outcome = ReasonCode::Accepted;
  journal.subject = "smartnic:" + std::to_string(sequence);
  journal.body = Value::object({{"smartnic", smartnic_to_value(record)}});
  return journal.to_value();
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

SNCF_TEST(hardening_failed_append_leaves_no_trace) {
  sncf_test::TempDir directory;
  {
    Journal::Options options;
    options.directory = directory.path();
    options.sync_on_commit = true;
    auto opened = Journal::open(options);
    SNCF_REQUIRE(opened.ok());
    Journal& journal = *opened.value();
    SNCF_REQUIRE(journal.append(journal_record_value(1)).ok());
    const auto size_before = std::filesystem::file_size(journal_path(directory.path()));

    // A partial write followed by a failure must roll the file back exactly.
    fail_next_append_once();
    auto failed = journal.append(journal_record_value(2));
    SNCF_CHECK(!failed.ok());
    SNCF_CHECK(failed.code() == ReasonCode::RefusedCommitFailed);
    SNCF_CHECK_EQ(std::filesystem::file_size(journal_path(directory.path())), size_before);

    // The sequence is not consumed, so the next append reuses it and the store
    // stays readable: no replayed sequence, no stranded torn frame.
    SNCF_REQUIRE(journal.append(journal_record_value(2)).ok());
  }
  auto reopened = [&] {
    Journal::Options options;
    options.directory = directory.path();
    options.sync_on_commit = true;
    return Journal::open(options);
  }();
  SNCF_REQUIRE(reopened.ok());
  if (reopened.value()->report().disposition != RecoveryDisposition::CleanReopen) {
    ::sncf_test::fail(__FILE__, __LINE__,
                      std::string("expected clean_reopen but got ") +
                          std::string(recovery_disposition_name(reopened.value()->report().disposition)) +
                          " (dropped " + std::to_string(reopened.value()->report().records_dropped) +
                          " records, " + std::to_string(reopened.value()->report().bytes_dropped) +
                          " bytes)");
  }
  SNCF_CHECK_EQ(reopened.value()->recovered().size(), 2U);
  SNCF_CHECK_EQ(reopened.value()->recovered()[0].sequence.value(), 1U);
  SNCF_CHECK_EQ(reopened.value()->recovered()[1].sequence.value(), 2U);
}

SNCF_TEST(hardening_journal_without_its_snapshot_prefix_is_refused) {
  sncf_test::TempDir directory;
  {
    Journal::Options options;
    options.directory = directory.path();
    options.sync_on_commit = false;
    auto opened = Journal::open(options);
    SNCF_REQUIRE(opened.ok());
    Journal& journal = *opened.value();
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
      SNCF_REQUIRE(journal.append(journal_record_value(sequence)).ok());
    }
    SNCF_REQUIRE(journal.write_snapshot(Value::object({{"marker", Value::uint_value(3)}}),
                                        RecordSequence::from_value(3))
                     .ok());
    SNCF_REQUIRE(journal.append(journal_record_value(4)).ok());
  }
  // Deleting the snapshot leaves a journal that no longer covers the whole
  // history. Replaying it would silently serve a partial state, so it is
  // refused instead.
  std::error_code error;
  std::filesystem::remove(snapshot_path(directory.path()), error);
  SNCF_REQUIRE(!error);

  Journal::Options options;
  options.directory = directory.path();
  options.sync_on_commit = false;
  auto refused = Journal::open(options);
  SNCF_REQUIRE(refused.ok());  // open returns a journal carrying the classification
  SNCF_CHECK(refused.value()->report().disposition == RecoveryDisposition::HistoryIncompleteRefused);
  SNCF_CHECK(refused.value()->report().reason == ReasonCode::RefusedSequenceGap);
  SNCF_CHECK(!recovery_is_usable(refused.value()->report().disposition));

  Fabric::Options fabric_options;
  fabric_options.store_directory = directory.path();
  auto fabric = Fabric::open(std::move(fabric_options));
  SNCF_CHECK(!fabric.ok());
}

SNCF_TEST(hardening_lease_table_stays_inside_its_bound) {
  FabricConfig config;
  config.max_leases = 4;
  config.max_journal_records = 4096;
  Harness harness(config);
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);
  sncf_test::report_verified(*fabric.value(), harness, activation);

  // Acquire and release repeatedly: the durable lease table must not grow past
  // the bound its own snapshot decoder enforces.
  for (int cycle = 0; cycle < 12; ++cycle) {
    auto instance = fabric.value()->inspect_instance(activation.instance);
    SNCF_REQUIRE(instance.ok());
    AcquireAuthorityRequest acquire;
    acquire.header = harness.next_header();
    acquire.instance = activation.instance;
    acquire.scope = ExclusiveScope{instance.value().smartnic, instance.value().device,
                                   instance.value().port};
    auto granted = fabric.value()->acquire_authority(acquire);
    if (!granted.ok()) {
      ::sncf_test::fail(__FILE__, __LINE__, std::string("acquire refused: ") +
                                               std::string(reason_name(granted.code())) + " - " +
                                               granted.status().detail());
    }
    ReleaseAuthorityRequest release;
    release.header = harness.next_header();
    release.instance = activation.instance;
    release.lease = granted.value().lease;
    release.token = granted.value().token;
    SNCF_REQUIRE(fabric.value()->release_authority(release).ok());
  }

  const Value exported = fabric.value()->export_value();
  const Value* durable = exported.find("durable");
  SNCF_REQUIRE(durable != nullptr);
  const Value* leases = durable->find("leases");
  SNCF_REQUIRE(leases != nullptr);
  SNCF_CHECK(leases->as_array().size() <= config.max_leases);

  // The store the runtime wrote must be one the runtime can read back.
  auto compacted = fabric.value()->compact();
  SNCF_REQUIRE(compacted.ok());
  fabric.value()->shutdown();
  auto reopened = harness.reopen();
  if (!reopened.ok()) {
    ::sncf_test::fail(__FILE__, __LINE__, std::string("reopen refused: ") +
                                             std::string(reason_name(reopened.code())) + " - " +
                                             reopened.status().detail());
  }
}

SNCF_TEST(hardening_device_reincarnation_revokes_every_lease_of_the_device) {
  FabricConfig config;
  config.max_instances = 8;
  Harness harness(config);
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  sncf_test::activate_new_instance(*fabric.value(), harness, topology, PortId::from_value(1));
  sncf_test::activate_new_instance(*fabric.value(), harness, topology, PortId::from_value(2));

  const Value before = fabric.value()->export_value();
  const Value* before_durable = before.find("durable");
  SNCF_REQUIRE(before_durable != nullptr);
  const Value* before_leases = before_durable->find("leases");
  SNCF_REQUIRE(before_leases != nullptr);
  std::size_t live_before = 0;
  for (const auto& lease : before_leases->as_array()) {
    auto revoked = field_bool(lease, "revoked");
    SNCF_REQUIRE(revoked.ok());
    if (!revoked.value()) {
      ++live_before;
    }
  }
  SNCF_CHECK(live_before >= 2U);

  RegisterDeviceRequest reincarnated;
  reincarnated.header = harness.next_header();
  reincarnated.smartnic = topology.smartnic;
  reincarnated.model = topology.model;
  reincarnated.incarnation = DeviceIncarnation::from_value(topology.incarnation.value() + 1);
  reincarnated.serial = "serial-0001";
  reincarnated.port = topology.port;
  auto advanced = fabric.value()->register_device(reincarnated);
  SNCF_REQUIRE(advanced.ok());

  const Value after = fabric.value()->export_value();
  const Value* after_durable = after.find("durable");
  SNCF_REQUIRE(after_durable != nullptr);
  const Value* after_leases = after_durable->find("leases");
  SNCF_REQUIRE(after_leases != nullptr);
  for (const auto& lease : after_leases->as_array()) {
    auto device = field_uint(lease, "device");
    auto revoked = field_bool(lease, "revoked");
    SNCF_REQUIRE(device.ok());
    SNCF_REQUIRE(revoked.ok());
    if (device.value() == topology.device.value()) {
      SNCF_CHECK(revoked.value());
    }
  }
}

SNCF_TEST(hardening_read_only_open_changes_nothing) {
  Harness harness;
  {
    auto fabric = harness.open();
    SNCF_REQUIRE(fabric.ok());
    sncf_test::build_topology(*fabric.value(), harness);
    fabric.value()->shutdown();
  }
  const auto before_journal = read_text_file(journal_path(harness.store()));
  const auto before_size = std::filesystem::file_size(journal_path(harness.store()));

  Fabric::Options options;
  options.store_directory = harness.store();
  options.read_only = true;
  auto fabric = Fabric::open(std::move(options));
  SNCF_REQUIRE(fabric.ok());
  SNCF_CHECK_EQ(fabric.value()->epoch().value(), 1U);  // no recovery advance

  RegisterSmartNicRequest request;
  request.header.command = CommandId::from_value(1);
  request.header.principal = PrincipalId::from_value(1);
  request.label = "should-not-exist";
  request.port_count = 1;
  auto refused = fabric.value()->register_smartnic(request);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedReadOnlyStore);
  auto compacted = fabric.value()->compact();
  SNCF_CHECK(!compacted.ok());
  SNCF_CHECK(compacted.code() == ReasonCode::RefusedReadOnlyStore);
  fabric.value()->shutdown();

  SNCF_CHECK_EQ(std::filesystem::file_size(journal_path(harness.store())), before_size);
  SNCF_CHECK_EQ(read_text_file(journal_path(harness.store())), before_journal);
}

SNCF_TEST(hardening_replay_outside_the_window_is_fenced_not_re_executed) {
  FabricConfig config;
  config.max_dedupe_entries = 2;
  config.max_smartnics = 32;
  Harness harness(config);
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());

  const auto register_named = [&](std::uint64_t command, const std::string& label) {
    RegisterSmartNicRequest request;
    request.header.command = CommandId::from_value(command);
    request.header.principal = PrincipalId::from_value(4);
    request.label = label;
    request.port_count = 1;
    return fabric.value()->register_smartnic(request);
  };
  SNCF_REQUIRE(register_named(1, "first").ok());
  SNCF_REQUIRE(register_named(2, "second").ok());
  SNCF_REQUIRE(register_named(3, "third").ok());
  SNCF_REQUIRE(register_named(4, "fourth").ok());

  // Command 1 has left the two-entry window. It must be refused rather than
  // silently executed a second time.
  auto fenced = register_named(1, "first");
  SNCF_CHECK(!fenced.ok());
  SNCF_CHECK(fenced.code() == ReasonCode::RefusedReplayWindowExceeded);
  SNCF_CHECK(fabric.value()->counters().replayed_commands_fenced == 1U);

  // A command still inside the window is still idempotent.
  auto replay = register_named(4, "fourth");
  SNCF_REQUIRE(replay.ok());
  SNCF_CHECK(replay.value().outcome.duplicate);

  const Value exported = fabric.value()->export_value();
  const Value* durable = exported.find("durable");
  SNCF_REQUIRE(durable != nullptr);
  const Value* smartnics = durable->find("smartnics");
  SNCF_REQUIRE(smartnics != nullptr);
  SNCF_CHECK_EQ(smartnics->as_array().size(), 4U);

  // The floor survives a restart, so the fence is durable.
  fabric.value()->shutdown();
  auto reopened = harness.reopen();
  SNCF_REQUIRE(reopened.ok());
  auto fenced_again = reopened.value()->register_smartnic([&] {
    RegisterSmartNicRequest request;
    request.header.command = CommandId::from_value(1);
    request.header.principal = PrincipalId::from_value(4);
    request.label = "first";
    request.port_count = 1;
    return request;
  }());
  SNCF_CHECK(!fenced_again.ok());
  SNCF_CHECK(fenced_again.code() == ReasonCode::RefusedReplayWindowExceeded);
}

SNCF_TEST(hardening_liveness_is_re_observed_after_restart) {
  Harness harness;
  Topology topology;
  {
    auto fabric = harness.open();
    SNCF_REQUIRE(fabric.ok());
    topology = sncf_test::build_topology(*fabric.value(), harness);
    fabric.value()->shutdown();
  }
  auto fabric = harness.reopen();
  SNCF_REQUIRE(fabric.ok());
  const Value exported = fabric.value()->export_value();
  const Value* durable = exported.find("durable");
  SNCF_REQUIRE(durable != nullptr);
  const Value* devices = durable->find("devices");
  SNCF_REQUIRE(devices != nullptr);
  SNCF_REQUIRE(!devices->as_array().empty());
  auto present = field_bool(devices->as_array()[0], "present");
  SNCF_REQUIRE(present.ok());
  SNCF_CHECK(!present.value());
}

SNCF_TEST(hardening_observation_freshness_is_evaluated_at_decision_time) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  const Topology topology = sncf_test::build_topology(*fabric.value(), harness);

  // The observation ages out even though nothing else changed: a stale liveness
  // report cannot keep authorising deployments.
  harness.advance(sncf::seconds(120));
  ActivateRequest request;
  request.header = harness.next_header();
  request.smartnic = topology.smartnic;
  request.device = topology.device;
  request.incarnation = topology.incarnation;
  request.package = topology.package;
  request.port = topology.port;
  request.queue = QueueId::from_value(1);
  auto refused = fabric.value()->activate(request);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedExpiredEvidence);
}

}  // namespace
