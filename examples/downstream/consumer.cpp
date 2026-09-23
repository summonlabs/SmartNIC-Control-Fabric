// Independent downstream consumer of the installed SmartNIC Control Fabric
// package. It uses only installed headers and the exported sncf::sncf target, so
// a successful run proves the package is complete and self-sufficient.
//
// All devices and evidence here are SYNTHETIC fixtures: no real SmartNIC is
// involved and none is claimed.
//
// Copyright 2026 Summon Software Labs.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include <sncf/fabric.hpp>
#include <sncf/version.hpp>

namespace {

using namespace sncf;

std::uint64_t g_command = 1;

CommandHeader next_header() {
  CommandHeader header;
  header.command = CommandId::from_value(g_command++);
  header.principal = PrincipalId::from_value(1);
  return header;
}

Digest256 digest_of(char fill) {
  const auto parsed = Digest256::from_hex(std::string(64, fill));
  return parsed.has_value() ? *parsed : Digest256{};
}

int fail(const char* step, const Status& status) {
  std::printf("downstream consumer failed at %s: %s (%s)\n", step, std::string(reason_name(status.code())).c_str(),
              status.detail().c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path store =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() / "sncf-downstream-consumer";

  std::error_code error;
  std::filesystem::remove_all(store, error);

  Fabric::Options options;
  options.store_directory = store;
  auto opened = Fabric::open(std::move(options));
  if (!opened.ok()) {
    return fail("open", opened.status());
  }
  Fabric& fabric = *opened.value();
  std::printf("sncf %s semantics %s epoch %llu\n", std::string(version_string()).c_str(),
              std::string(semantics_id()).c_str(),
              static_cast<unsigned long long>(fabric.epoch().value()));

  RegisterSmartNicRequest smartnic;
  smartnic.header = next_header();
  smartnic.label = "downstream-nic";
  smartnic.port_count = 2;
  auto smartnic_result = fabric.register_smartnic(smartnic);
  if (!smartnic_result.ok()) {
    return fail("register_smartnic", smartnic_result.status());
  }

  RegisterDeviceRequest device;
  device.header = next_header();
  device.smartnic = smartnic_result.value().id;
  device.model = DeviceModelId::from_value(1);
  device.incarnation = DeviceIncarnation::from_value(1);
  device.serial = "downstream-serial";
  device.port = PortId::from_value(1);
  auto device_result = fabric.register_device(device);
  if (!device_result.ok()) {
    return fail("register_device", device_result.status());
  }

  RegisterPackageRequest package;
  package.header = next_header();
  package.name = "downstream-function";
  package.version = SemVer{1, 0, 0};
  package.digest = digest_of('a');
  package.supported_models = {DeviceModelId::from_value(1)};
  package.required_capabilities = {CapabilityCode::PacketParsing};
  package.min_capability_generation = CapabilityGeneration::from_value(1);
  package.compatibility.min = CompatibilityGeneration::from_value(1);
  package.compatibility.max = CompatibilityGeneration::from_value(4);
  package.min_firmware = SemVer{1, 0, 0};
  package.exclusive_scope = true;
  auto package_result = fabric.register_package(package);
  if (!package_result.ok()) {
    return fail("register_package", package_result.status());
  }

  CapabilityEvidenceRequest evidence;
  evidence.header = next_header();
  evidence.source = EvidenceSource::SyntheticFixture;
  evidence.payload_digest = digest_of('b');
  evidence.observed_at = SystemClock{}.now();
  evidence.synthetic = true;
  evidence.smartnic = smartnic_result.value().id;
  evidence.device = device_result.value().id;
  evidence.incarnation = device.incarnation;
  evidence.model = device.model;
  evidence.capability_generation = CapabilityGeneration::from_value(2);
  evidence.compatibility_generation = CompatibilityGeneration::from_value(2);
  evidence.firmware = SemVer{1, 4, 0};
  evidence.capabilities = {CapabilityCode::PacketParsing};
  auto evidence_result = fabric.submit_capability_evidence(evidence);
  if (!evidence_result.ok()) {
    return fail("submit_capability_evidence", evidence_result.status());
  }

  ObservationRequest observation;
  observation.header = next_header();
  observation.source = EvidenceSource::SyntheticFixture;
  observation.payload_digest = digest_of('c');
  observation.observed_at = SystemClock{}.now();
  observation.synthetic = true;
  observation.smartnic = smartnic_result.value().id;
  observation.device = device_result.value().id;
  observation.incarnation = device.incarnation;
  observation.present = true;
  observation.capability_generation = evidence.capability_generation;
  observation.compatibility_generation = evidence.compatibility_generation;
  auto observation_result = fabric.submit_observation(observation);
  if (!observation_result.ok()) {
    return fail("submit_observation", observation_result.status());
  }

  ActivateRequest activation;
  activation.header = next_header();
  activation.smartnic = smartnic_result.value().id;
  activation.device = device_result.value().id;
  activation.incarnation = device.incarnation;
  activation.package = package_result.value().id;
  activation.port = device.port;
  activation.queue = QueueId::from_value(1);
  auto activation_result = fabric.activate(activation);
  if (!activation_result.ok()) {
    return fail("activate", activation_result.status());
  }

  EffectReportRequest effect;
  effect.header = next_header();
  effect.instance = activation_result.value().instance;
  effect.attempt = activation_result.value().attempt;
  effect.generation = activation_result.value().generation;
  effect.token = activation_result.value().token;
  effect.outcome = EffectOutcome::Verified;
  effect.source = EvidenceSource::SyntheticFixture;
  effect.payload_digest = digest_of('d');
  effect.observed_at = SystemClock{}.now();
  effect.synthetic = true;
  effect.detail = "downstream verification";
  auto effect_result = fabric.report_effect(effect);
  if (!effect_result.ok()) {
    return fail("report_effect", effect_result.status());
  }

  auto instance = fabric.inspect_instance(activation_result.value().instance);
  if (!instance.ok()) {
    return fail("inspect_instance", instance.status());
  }
  if (instance.value().lifecycle != LifecycleState::Verified) {
    std::printf("downstream consumer failed: lifecycle is %s\n",
                std::string(lifecycle_state_name(instance.value().lifecycle)).c_str());
    return 1;
  }

  const std::string exported = fabric.export_canonical();
  const auto parsed = parse_canonical(exported);
  if (!parsed.ok()) {
    return fail("parse_canonical", parsed.status());
  }
  const auto counters = fabric.counters();
  std::printf("downstream consumer ok: lifecycle=verified accepted=%llu refused=%llu durable_digest=%s\n",
              static_cast<unsigned long long>(counters.commands_accepted),
              static_cast<unsigned long long>(counters.commands_refused),
              fabric.durable_digest().to_hex().c_str());
  fabric.shutdown();
  return 0;
}
