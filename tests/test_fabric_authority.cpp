// Copyright 2026 Summon Software Labs.
#include <thread>

#include "sncf/fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace sncf;
using sncf_test::Harness;
using sncf_test::Topology;

SNCF_TEST(fabric_authority_scope_is_exclusive) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto first = sncf_test::activate_new_instance(*fabric.value(), harness, topology, PortId::from_value(1));

  ActivateRequest second;
  second.header = harness.next_header();
  second.smartnic = topology.smartnic;
  second.device = topology.device;
  second.incarnation = topology.incarnation;
  second.package = topology.package;
  second.port = PortId::from_value(1);  // the same exclusive port
  second.queue = QueueId::from_value(2);
  auto refused = fabric.value()->activate(second);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedScopeConflict);

  // A different port is a different scope and is accepted.
  second.header = harness.next_header();
  second.port = PortId::from_value(2);
  auto accepted = fabric.value()->activate(second);
  SNCF_REQUIRE(accepted.ok());
  SNCF_CHECK(accepted.value().instance != first.instance);
}

SNCF_TEST(fabric_authority_lease_holder_is_exclusive) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);

  auto instance = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(instance.ok());

  // The holder releasing its own lease succeeds and clears authority.
  ReleaseAuthorityRequest release;
  release.header = harness.next_header();
  release.instance = activation.instance;
  release.lease = activation.lease;
  release.token = FencingToken::from_value(0);  // stale on purpose
  auto stale = fabric.value()->release_authority(release);
  SNCF_CHECK(!stale.ok());
  SNCF_CHECK(stale.code() == ReasonCode::RefusedStaleFencingToken);

  release.header = harness.next_header();
  release.token = activation.token;
  auto released = fabric.value()->release_authority(release);
  SNCF_REQUIRE(released.ok());
  auto after = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(after.ok());
  SNCF_CHECK(after.value().authority == AuthorityState::Revoked);
  SNCF_CHECK(after.value().lease.is_nil());

  // Releasing again with the same command is a replay, not a new revocation.
  auto replay = fabric.value()->release_authority(release);
  SNCF_REQUIRE(replay.ok());
  SNCF_CHECK(replay.value().duplicate);
}

SNCF_TEST(fabric_authority_lease_expiry_is_explicit) {
  FabricConfig config;
  config.lease_ttl = sncf_test::Harness{}.config().lease_ttl;
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);
  auto instance = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(instance.ok());

  // The lease that was live at activation has now expired.
  harness.advance(sncf::seconds(31));
  AcquireAuthorityRequest acquire;
  acquire.header = harness.next_header();
  acquire.instance = activation.instance;
  acquire.scope = ExclusiveScope{instance.value().smartnic, instance.value().device, instance.value().port};
  auto renewed = fabric.value()->acquire_authority(acquire);
  SNCF_REQUIRE(renewed.ok());
  // Renewal never reuses the expired token: the fencing mark strictly increases.
  SNCF_CHECK(renewed.value().token.value() > activation.token.value());
  SNCF_CHECK(renewed.value().lease != activation.lease);

  // The expired token can no longer authorise anything.
  auto refreshed = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(refreshed.ok());
  ActivateRequest activate;
  activate.header = harness.next_header();
  activate.smartnic = topology.smartnic;
  activate.device = topology.device;
  activate.incarnation = topology.incarnation;
  activate.package = topology.package;
  activate.port = topology.port;
  activate.queue = QueueId::from_value(1);
  activate.instance = activation.instance;
  activate.claim = sncf_test::claim_for(refreshed.value(), *fabric.value());
  activate.claim.token = activation.token;
  activate.claim.lease = activation.lease;
  auto fenced = fabric.value()->activate(activate);
  SNCF_CHECK(!fenced.ok());
  SNCF_CHECK(fenced.code() == ReasonCode::RefusedStaleFencingToken ||
             fenced.code() == ReasonCode::RefusedUnknownLease ||
             fenced.code() == ReasonCode::RefusedLeaseExpired);
}

SNCF_TEST(fabric_authority_stale_token_is_fenced) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);
  sncf_test::report_verified(*fabric.value(), harness, activation);

  auto instance = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(instance.ok());

  // Release and re-acquire: the new lease carries a strictly higher token.
  ReleaseAuthorityRequest release;
  release.header = harness.next_header();
  release.instance = activation.instance;
  release.lease = instance.value().lease;
  release.token = instance.value().token;
  SNCF_REQUIRE(fabric.value()->release_authority(release).ok());

  AcquireAuthorityRequest acquire;
  acquire.header = harness.next_header();
  acquire.instance = activation.instance;
  acquire.scope = ExclusiveScope{instance.value().smartnic, instance.value().device, instance.value().port};
  auto granted = fabric.value()->acquire_authority(acquire);
  SNCF_REQUIRE(granted.ok());
  SNCF_CHECK(granted.value().token.value() > activation.token.value());

  auto reacquired = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(reacquired.ok());
  ActivateRequest activate;
  activate.header = harness.next_header();
  activate.smartnic = topology.smartnic;
  activate.device = topology.device;
  activate.incarnation = topology.incarnation;
  activate.package = topology.package;
  activate.port = topology.port;
  activate.queue = QueueId::from_value(1);
  activate.instance = activation.instance;
  activate.claim = sncf_test::claim_for(reacquired.value(), *fabric.value());
  activate.claim.token = activation.token;  // the fenced token
  auto refused = fabric.value()->activate(activate);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedStaleFencingToken);

  activate.header = harness.next_header();
  activate.claim.token = granted.value().token;
  activate.claim.lease = granted.value().lease;
  auto accepted = fabric.value()->activate(activate);
  SNCF_REQUIRE(accepted.ok());
  SNCF_CHECK(accepted.value().token.value() >= granted.value().token.value());
}

SNCF_TEST(fabric_restart_fences_authority_and_advances_epoch) {
  Harness harness;
  std::uint64_t first_epoch = 0;
  std::uint64_t first_token = 0;
  FunctionInstanceId instance_id;
  {
    auto fabric = harness.open();
    SNCF_REQUIRE(fabric.ok());
    Topology topology = sncf_test::build_topology(*fabric.value(), harness);
    const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);
    instance_id = activation.instance;
    first_epoch = fabric.value()->epoch().value();
    first_token = activation.token.value();
    fabric.value()->shutdown();
  }
  auto reopened = harness.reopen();
  SNCF_REQUIRE(reopened.ok());
  SNCF_CHECK(reopened.value()->epoch().value() > first_epoch);
  const RecoveryReport report = reopened.value()->recovery();
  SNCF_CHECK(report.disposition == RecoveryDisposition::CleanReopen);
  SNCF_CHECK(report.records_recovered > 0U);

  auto instance = reopened.value()->inspect_instance(instance_id);
  SNCF_REQUIRE(instance.ok());
  SNCF_CHECK(instance.value().authority == AuthorityState::Superseded);
  SNCF_CHECK(instance.value().lease.is_nil());
  SNCF_CHECK(reopened.value()->counters().tokens_advanced_at_recovery >= 1U);

  // The pre-restart token can no longer authorise anything.
  ActivateRequest activate;
  activate.header = harness.next_header();
  activate.smartnic = instance.value().smartnic;
  activate.device = instance.value().device;
  activate.incarnation = instance.value().incarnation;
  activate.package = instance.value().package;
  activate.port = instance.value().port;
  activate.queue = instance.value().queue;
  activate.instance = instance_id;
  activate.claim.scope = ExclusiveScope{instance.value().smartnic, instance.value().device, instance.value().port};
  activate.claim.instance = instance_id;
  activate.claim.lease = LeaseId::from_value(1);
  activate.claim.token = FencingToken::from_value(first_token);
  activate.claim.epoch = CoordinatorEpoch::from_value(first_epoch);
  activate.claim.principal = PrincipalId::from_value(7);
  auto refused = reopened.value()->activate(activate);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedUnknownLease ||
             refused.code() == ReasonCode::RefusedStaleEpoch ||
             refused.code() == ReasonCode::RefusedStaleFencingToken ||
             refused.code() == ReasonCode::RefusedAuthorityRevoked ||
             refused.code() == ReasonCode::RefusedNoAuthority);
}

SNCF_TEST(fabric_repeated_restart_is_monotonic_and_never_resurrects_authority) {
  Harness harness;
  FunctionInstanceId instance_id;
  {
    auto fabric = harness.open();
    SNCF_REQUIRE(fabric.ok());
    Topology topology = sncf_test::build_topology(*fabric.value(), harness);
    const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);
    instance_id = activation.instance;
    fabric.value()->shutdown();
  }
  std::uint64_t last_epoch = 1;
  for (int cycle = 0; cycle < 4; ++cycle) {
    auto fabric = harness.reopen();
    SNCF_REQUIRE(fabric.ok());
    SNCF_CHECK(fabric.value()->epoch().value() > last_epoch);
    last_epoch = fabric.value()->epoch().value();
    auto instance = fabric.value()->inspect_instance(instance_id);
    SNCF_REQUIRE(instance.ok());
    SNCF_CHECK(instance.value().authority != AuthorityState::Granted);
    fabric.value()->shutdown();
  }
}

SNCF_TEST(fabric_concurrent_queries_observe_consistent_state) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);

  std::atomic<int> mismatches{0};
  std::vector<std::thread> readers;
  for (int worker = 0; worker < 4; ++worker) {
    readers.emplace_back([&] {
      for (int iteration = 0; iteration < 32; ++iteration) {
        auto instance = fabric.value()->inspect_instance(activation.instance);
        if (!instance.ok() || instance.value().id != activation.instance ||
            instance.value().device != topology.device) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
        const std::string text = fabric.value()->export_canonical();
        auto parsed = parse_canonical(text);
        if (!parsed.ok()) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& reader : readers) {
    reader.join();
  }
  SNCF_CHECK_EQ(mismatches.load(), 0);
}

}  // namespace
