// Copyright 2026 Summon Software Labs.
#include <string>

#include "sncf/fabric.hpp"
#include "sncf/version.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace sncf;
using sncf_test::Harness;
using sncf_test::Topology;

SNCF_TEST(explain_instance_is_deterministic_and_complete) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);

  auto explanation = fabric.value()->explain_instance(activation.instance);
  SNCF_REQUIRE(explanation.ok());
  SNCF_CHECK_EQ(explanation.value().subject, std::string("instance:") +
                                                std::to_string(activation.instance.value()));
  SNCF_CHECK(!explanation.value().factors.empty());
  SNCF_CHECK(explanation.value().accepted);
  const std::string text = explanation.value().to_canonical();
  auto parsed = parse_canonical(text);
  SNCF_REQUIRE(parsed.ok());
  const Value* factors = parsed.value().find("factors");
  SNCF_REQUIRE(factors != nullptr);
  SNCF_CHECK(factors->is_array());
  SNCF_CHECK(!factors->as_array().empty());

  auto again = fabric.value()->explain_instance(activation.instance);
  SNCF_REQUIRE(again.ok());
  SNCF_CHECK_EQ(again.value().to_canonical(), text);

  auto missing = fabric.value()->explain_instance(FunctionInstanceId::from_value(4242));
  SNCF_CHECK(!missing.ok());
  SNCF_CHECK(missing.code() == ReasonCode::RefusedUnknownInstance);
}

SNCF_TEST(explain_last_decision_names_the_refusal_and_its_evidence) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);

  // A stale token is refused and the explanation must say which rule applied.
  auto instance = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(instance.ok());
  ActivateRequest request;
  request.header = harness.next_header();
  request.smartnic = topology.smartnic;
  request.device = topology.device;
  request.incarnation = topology.incarnation;
  request.package = topology.package;
  request.port = topology.port;
  request.queue = QueueId::from_value(1);
  request.instance = activation.instance;
  request.claim = sncf_test::claim_for(instance.value(), *fabric.value());
  request.claim.token = FencingToken::from_value(activation.token.value() - 1U);
  auto refused = fabric.value()->activate(request);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedStaleFencingToken);

  auto explanation = fabric.value()->explain_last_decision();
  SNCF_REQUIRE(explanation.ok());
  SNCF_CHECK(!explanation.value().accepted);
  SNCF_CHECK(explanation.value().primary_reason == ReasonCode::RefusedStaleFencingToken);
  SNCF_CHECK_EQ(explanation.value().operation, std::string("activate"));
  bool found = false;
  for (const auto& factor : explanation.value().factors) {
    if (factor.reason == ReasonCode::RefusedStaleFencingToken) {
      found = true;
    }
  }
  SNCF_CHECK(found);
  SNCF_CHECK(!explanation.value().factors.empty());
}

SNCF_TEST(explain_evidence_factors_carry_digests_and_generations) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);
  (void)activation;
  auto explanation = fabric.value()->explain_last_decision();
  SNCF_REQUIRE(explanation.ok());
  SNCF_CHECK(explanation.value().accepted);
  bool has_evidence = false;
  bool has_generation = false;
  for (const auto& factor : explanation.value().factors) {
    if (factor.has_evidence) {
      has_evidence = true;
      SNCF_CHECK(!factor.evidence_digest.is_zero());
      SNCF_CHECK_EQ(factor.evidence_digest.to_hex().size(), 64U);
    }
    if (factor.has_generation) {
      has_generation = true;
    }
  }
  SNCF_CHECK(has_evidence);
  SNCF_CHECK(has_generation);
}

SNCF_TEST(export_separates_durable_state_from_volatile_runtime_state) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  sncf_test::activate_new_instance(*fabric.value(), harness, topology);

  const Value exported = fabric.value()->export_value();
  const Value* durable = exported.find("durable");
  const Value* runtime = exported.find("runtime");
  SNCF_REQUIRE(durable != nullptr);
  SNCF_REQUIRE(runtime != nullptr);
  SNCF_CHECK(durable->find("instances") != nullptr);
  SNCF_CHECK(durable->find("events") == nullptr);
  SNCF_CHECK(runtime->find("events") != nullptr);
  SNCF_CHECK(runtime->find("counters") != nullptr);
  SNCF_CHECK(runtime->find("reasons") != nullptr);
  SNCF_CHECK(runtime->find("recovery") != nullptr);
  auto volatile_flag = field_bool(*runtime, "volatile");
  SNCF_REQUIRE(volatile_flag.ok());
  SNCF_CHECK(volatile_flag.value());

  auto schema = field_uint(exported, "schema");
  auto version = field_string(exported, "version");
  auto semantics = field_string(exported, "semantics");
  SNCF_REQUIRE(schema.ok());
  SNCF_REQUIRE(version.ok());
  SNCF_REQUIRE(semantics.ok());
  SNCF_CHECK_EQ(version.value(), std::string(version_string()));
  SNCF_CHECK_EQ(semantics.value(), std::string(semantics_id()));
}

SNCF_TEST(export_is_byte_stable_for_identical_operation_streams) {
  Harness first;
  Harness second;
  auto first_fabric = first.open();
  auto second_fabric = second.open();
  SNCF_REQUIRE(first_fabric.ok());
  SNCF_REQUIRE(second_fabric.ok());
  sncf_test::build_topology(*first_fabric.value(), first);
  sncf_test::build_topology(*second_fabric.value(), second);
  SNCF_CHECK_EQ(first_fabric.value()->export_canonical(), second_fabric.value()->export_canonical());
  SNCF_CHECK_EQ(first_fabric.value()->durable_digest().to_hex(),
                second_fabric.value()->durable_digest().to_hex());
}

SNCF_TEST(export_reflects_refusals_in_the_reason_counters) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  ActivateRequest request;
  request.header = harness.next_header();
  request.smartnic = SmartNicId::from_value(1);
  request.device = DeviceId::from_value(1);
  request.package = FunctionPackageId::from_value(1);
  request.incarnation = DeviceIncarnation::from_value(1);
  auto refused = fabric.value()->activate(request);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(fabric.value()->reasons().count(ReasonCode::RefusedUnknownPackage) == 1U);
  const Value exported = fabric.value()->export_value();
  const Value* runtime = exported.find("runtime");
  SNCF_REQUIRE(runtime != nullptr);
  const Value* reasons = runtime->find("reasons");
  SNCF_REQUIRE(reasons != nullptr);
  const Value* entry = reasons->find("refused_unknown_package");
  SNCF_REQUIRE(entry != nullptr);
  auto count = field_uint(*reasons, "refused_unknown_package");
  SNCF_REQUIRE(count.ok());
  SNCF_CHECK_EQ(count.value(), 1U);
  const Value* events = runtime->find("events");
  SNCF_REQUIRE(events != nullptr);
  SNCF_CHECK(!events->as_array().empty());
}

}  // namespace
