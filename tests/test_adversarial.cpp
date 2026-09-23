// Copyright 2026 Summon Software Labs.
#include <limits>
#include <string>

#include "sncf/fabric.hpp"
#include "sncf/service.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace sncf;
using sncf_test::Harness;
using sncf_test::Topology;

constexpr std::uint64_t kMaxU64 = (std::numeric_limits<std::uint64_t>::max)();

SNCF_TEST(adversarial_integer_extremes_are_refused_not_wrapped) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());

  RegisterSmartNicRequest extreme;
  extreme.header = harness.next_header();
  extreme.label = "nic-a";
  extreme.port_count = 0xFFFFFFFFU;
  auto refused = fabric.value()->register_smartnic(extreme);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedInvalidRange);

  extreme.header = harness.next_header();
  extreme.port_count = 0;
  auto zero_ports = fabric.value()->register_smartnic(extreme);
  SNCF_CHECK(!zero_ports.ok());
  SNCF_CHECK(zero_ports.code() == ReasonCode::RefusedInvalidRange);

  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  ActivateRequest activation;
  activation.header = harness.next_header();
  activation.smartnic = SmartNicId::from_value(kMaxU64);
  activation.device = DeviceId::from_value(kMaxU64);
  activation.incarnation = DeviceIncarnation::from_value(0xFFFFFFFFU);
  activation.package = FunctionPackageId::from_value(kMaxU64);
  activation.port = PortId::from_value(0xFFFFU);
  auto unknown = fabric.value()->activate(activation);
  SNCF_CHECK(!unknown.ok());
  SNCF_CHECK(unknown.code() == ReasonCode::RefusedUnknownPackage);
  SNCF_CHECK_EQ(fabric.value()->counters().internal_apply_failures, 0U);
  (void)topology;

  AcquireAuthorityRequest acquire;
  acquire.header = harness.next_header();
  acquire.instance = FunctionInstanceId::from_value(kMaxU64);
  acquire.ttl = DurationNs::from_value(kMaxU64);
  auto missing = fabric.value()->acquire_authority(acquire);
  SNCF_CHECK(!missing.ok());
  SNCF_CHECK(missing.code() == ReasonCode::RefusedUnknownInstance);

  auto missing_instance = fabric.value()->inspect_instance(FunctionInstanceId::from_value(kMaxU64));
  SNCF_CHECK(!missing_instance.ok());
  SNCF_CHECK(missing_instance.code() == ReasonCode::RefusedUnknownInstance);
}

SNCF_TEST(adversarial_oversized_inputs_are_refused) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());

  RegisterSmartNicRequest label;
  label.header = harness.next_header();
  label.label = std::string(1000U, 'a');
  label.port_count = 1;
  auto long_label = fabric.value()->register_smartnic(label);
  SNCF_CHECK(!long_label.ok());
  SNCF_CHECK(long_label.code() == ReasonCode::RefusedOversizedInput);

  label.header = harness.next_header();
  label.label = "bad label with spaces";
  auto bad_label = fabric.value()->register_smartnic(label);
  SNCF_CHECK(!bad_label.ok());
  SNCF_CHECK(bad_label.code() == ReasonCode::RefusedInvalidLabel);

  RegisterPackageRequest package;
  package.header = harness.next_header();
  package.name = "package";
  package.version = SemVer{1, 0, 0};
  package.digest = sncf_test::digest_of('a');
  for (std::uint16_t model = 1; model <= 300U; ++model) {
    package.supported_models.push_back(DeviceModelId::from_value(model));
  }
  package.required_capabilities = {CapabilityCode::PacketParsing};
  package.compatibility.min = CompatibilityGeneration::from_value(1);
  package.compatibility.max = CompatibilityGeneration::from_value(2);
  auto too_many_models = fabric.value()->register_package(package);
  SNCF_CHECK(!too_many_models.ok());
  SNCF_CHECK(too_many_models.code() == ReasonCode::RefusedOversizedInput);

  package.supported_models = {DeviceModelId::from_value(1), DeviceModelId::from_value(1)};
  package.header = harness.next_header();
  auto duplicate_models = fabric.value()->register_package(package);
  SNCF_CHECK(!duplicate_models.ok());
  SNCF_CHECK(duplicate_models.code() == ReasonCode::RefusedDuplicateIdentity);

  package.supported_models = {DeviceModelId::from_value(0)};
  package.header = harness.next_header();
  auto nil_model = fabric.value()->register_package(package);
  SNCF_CHECK(!nil_model.ok());
  SNCF_CHECK(nil_model.code() == ReasonCode::RefusedNilIdentity);

  package.supported_models = {DeviceModelId::from_value(1)};
  package.compatibility.min = CompatibilityGeneration::from_value(9);
  package.compatibility.max = CompatibilityGeneration::from_value(3);
  package.header = harness.next_header();
  auto inverted = fabric.value()->register_package(package);
  SNCF_CHECK(!inverted.ok());
  SNCF_CHECK(inverted.code() == ReasonCode::RefusedInvalidRange);
}

SNCF_TEST(adversarial_package_identity_collisions_are_refused) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  RegisterPackageRequest package;
  package.header = harness.next_header();
  package.name = "collide";
  package.version = SemVer{2, 3, 4};
  package.digest = sncf_test::digest_of('a');
  package.supported_models = {DeviceModelId::from_value(1)};
  package.required_capabilities = {CapabilityCode::PacketParsing};
  package.compatibility.min = CompatibilityGeneration::from_value(1);
  package.compatibility.max = CompatibilityGeneration::from_value(5);
  auto first = fabric.value()->register_package(package);
  SNCF_REQUIRE(first.ok());

  package.header = harness.next_header();
  auto same = fabric.value()->register_package(package);
  SNCF_REQUIRE(same.ok());
  SNCF_CHECK(same.value().outcome.reason == ReasonCode::AcceptedNoChange);
  SNCF_CHECK_EQ(same.value().id.value(), first.value().id.value());

  package.header = harness.next_header();
  package.digest = sncf_test::digest_of('b');
  auto collision = fabric.value()->register_package(package);
  SNCF_CHECK(!collision.ok());
  SNCF_CHECK(collision.code() == ReasonCode::RefusedDuplicateIdentity);

  package.header = harness.next_header();
  package.digest = Digest256{};
  auto no_digest = fabric.value()->register_package(package);
  SNCF_CHECK(!no_digest.ok());
  SNCF_CHECK(no_digest.code() == ReasonCode::RefusedInvalidDigest);
}

SNCF_TEST(adversarial_replayed_command_with_a_different_payload_is_stable) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  RegisterSmartNicRequest first;
  first.header = harness.next_header();
  first.label = "nic-a";
  first.port_count = 2;
  auto recorded = fabric.value()->register_smartnic(first);
  SNCF_REQUIRE(recorded.ok());
  const std::uint64_t id = recorded.value().id.value();

  // The same command identity with a different label returns the recorded
  // outcome and must not create a second SmartNIC.
  RegisterSmartNicRequest conflicting = first;
  conflicting.label = "nic-b";
  auto replay = fabric.value()->register_smartnic(conflicting);
  SNCF_REQUIRE(replay.ok());
  SNCF_CHECK(replay.value().outcome.duplicate);
  SNCF_CHECK_EQ(replay.value().id.value(), id);
  SNCF_CHECK_EQ(fabric.value()->counters().instances_created, 0U);

  RegisterSmartNicRequest third;
  third.header = harness.next_header();
  third.label = "nic-c";
  third.port_count = 1;
  auto created = fabric.value()->register_smartnic(third);
  SNCF_REQUIRE(created.ok());
  SNCF_CHECK_EQ(created.value().id.value(), id + 1U);
}

SNCF_TEST(adversarial_timestamp_and_duration_extremes) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);

  const auto activation = sncf_test::activate_new_instance(*fabric.value(), harness, topology);
  auto instance = fabric.value()->inspect_instance(activation.instance);
  SNCF_REQUIRE(instance.ok());

  // Release the activation lease so the far-future acquisition is really tried.
  ReleaseAuthorityRequest release;
  release.header = harness.next_header();
  release.instance = activation.instance;
  release.lease = activation.lease;
  release.token = activation.token;
  SNCF_REQUIRE(fabric.value()->release_authority(release).ok());

  AcquireAuthorityRequest acquire;
  acquire.header = harness.next_header();
  acquire.instance = instance.value().id;
  acquire.scope = ExclusiveScope{instance.value().smartnic, instance.value().device, instance.value().port};
  acquire.ttl = DurationNs::from_value(kMaxU64);
  auto overflow = fabric.value()->acquire_authority(acquire);
  SNCF_CHECK(!overflow.ok());
  SNCF_CHECK(overflow.code() == ReasonCode::RefusedArithmeticOverflow);

  EffectReportRequest report;
  report.header = harness.next_header();
  report.instance = instance.value().id;
  report.attempt = instance.value().current_attempt;
  report.generation = instance.value().deployment_generation;
  report.token = instance.value().token;
  report.outcome = EffectOutcome::Applied;
  report.source = EvidenceSource::SyntheticFixture;
  report.payload_digest = sncf_test::digest_of('d');
  report.observed_at = TimestampNs::from_value(kMaxU64);
  auto future = fabric.value()->report_effect(report);
  SNCF_CHECK(!future.ok());
  SNCF_CHECK(future.code() == ReasonCode::RefusedInvalidRange);

  report.header = harness.next_header();
  report.observed_at = TimestampNs::from_value(0);
  auto ancient = fabric.value()->report_effect(report);
  SNCF_CHECK(!ancient.ok());
  SNCF_CHECK(ancient.code() == ReasonCode::RefusedExpiredEvidence);
}

SNCF_TEST(adversarial_service_rejects_unknown_and_malformed_operations) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  FabricService service(*fabric.value());

  auto unknown = service.dispatch("not-an-operation", Value::object({}));
  SNCF_CHECK(!unknown.ok());
  SNCF_CHECK(unknown.code() == ReasonCode::RefusedOutOfBoundary);

  auto not_object = service.dispatch("ping", Value::array({}));
  SNCF_CHECK(!not_object.ok());
  SNCF_CHECK(not_object.code() == ReasonCode::RefusedMalformedInput);

  auto missing_field = service.dispatch("register_smartnic", Value::object({{"command", Value::uint_value(1)}}));
  SNCF_CHECK(!missing_field.ok());
  SNCF_CHECK(missing_field.code() == ReasonCode::RefusedMissingField);

  auto nil_identity = service.dispatch(
      "register_smartnic",
      Value::object({{"command", Value::uint_value(0)},
                     {"label", Value::string("nic-a")},
                     {"port_count", Value::uint_value(1)},
                     {"principal", Value::uint_value(1)}}));
  SNCF_CHECK(!nil_identity.ok());
  SNCF_CHECK(nil_identity.code() == ReasonCode::RefusedNilIdentity);

  auto bad_capability = service.dispatch(
      "register_package",
      Value::object({{"capabilities", Value::array({Value::string("no-such-capability")})},
                     {"command", Value::uint_value(1)},
                     {"compatibility", Value::object({{"max", Value::uint_value(2)}, {"min", Value::uint_value(1)}})},
                     {"demand", Value::object({{"memory_bytes", Value::uint_value(0)},
                                               {"ports", Value::uint_value(1)},
                                               {"queues", Value::uint_value(1)}})},
                     {"digest", Value::string(std::string(64, 'a'))},
                     {"exclusive_scope", Value::boolean(true)},
                     {"layout_revision", Value::uint_value(1)},
                     {"min_capability_generation", Value::uint_value(1)},
                     {"min_firmware", Value::object({{"major", Value::uint_value(1)},
                                                     {"minor", Value::uint_value(0)},
                                                     {"patch", Value::uint_value(0)}})},
                     {"name", Value::string("package")},
                     {"principal", Value::uint_value(1)},
                     {"supported_models", Value::array({Value::uint_value(1)})},
                     {"version", Value::object({{"major", Value::uint_value(1)},
                                                {"minor", Value::uint_value(0)},
                                                {"patch", Value::uint_value(0)}})}}));
  SNCF_CHECK(!bad_capability.ok());
  SNCF_CHECK(bad_capability.code() == ReasonCode::RefusedInvalidEnumValue);

  auto pong = service.dispatch("ping", Value::object({}));
  SNCF_REQUIRE(pong.ok());
  auto ok_field = field_bool(pong.value(), "ok");
  SNCF_REQUIRE(ok_field.ok());
  SNCF_CHECK(ok_field.value());
}

SNCF_TEST(adversarial_corrupt_state_documents_are_refused) {
  Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Topology topology = sncf_test::build_topology(*fabric.value(), harness);
  (void)topology;
  Value document = fabric.value()->export_value();
  const Value* durable = document.find("durable");
  SNCF_REQUIRE(durable != nullptr);

  FabricState state;
  FabricConfig config;
  auto good = state_from_value(*durable, state, config);
  SNCF_REQUIRE(good.ok());
  SNCF_CHECK_EQ(state.instances.size(), 0U);

  // Remove a required key.
  {
    Value::object_type fields = durable->as_object();
    fields.erase(std::remove_if(fields.begin(), fields.end(),
                                [](const Value::field_type& field) { return field.first == "epoch"; }),
                 fields.end());
    FabricState target;
    auto refused = state_from_value(Value::object(std::move(fields)), target, config);
    SNCF_CHECK(!refused.ok());
    SNCF_CHECK(refused.code() == ReasonCode::RefusedMissingField);
  }
  // Replace the version marker with something incompatible.
  {
    Value::object_type fields = durable->as_object();
    for (auto& field : fields) {
      if (field.first == "schema") {
        field.second = Value::uint_value(999);
      }
    }
    FabricState target;
    auto refused = state_from_value(Value::object(std::move(fields)), target, config);
    SNCF_CHECK(!refused.ok());
    SNCF_CHECK(refused.code() == ReasonCode::RefusedIncompatibleStoreVersion);
  }
  // Swap the semantics identifier.
  {
    Value::object_type fields = durable->as_object();
    for (auto& field : fields) {
      if (field.first == "semantics") {
        field.second = Value::string("other-semantics");
      }
    }
    FabricState target;
    auto refused = state_from_value(Value::object(std::move(fields)), target, config);
    SNCF_CHECK(!refused.ok());
    SNCF_CHECK(refused.code() == ReasonCode::RefusedSemanticsMismatch);
  }
  // Invert the order of a collection.
  {
    Value::object_type fields = durable->as_object();
    for (auto& field : fields) {
      if (field.first == "leases") {
        field.second = Value::array({Value::object({{"device", Value::uint_value(1)}})});
      }
    }
    FabricState target;
    auto refused = state_from_value(Value::object(std::move(fields)), target, config);
    SNCF_CHECK(!refused.ok());
  }
}

}  // namespace
