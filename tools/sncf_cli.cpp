// sncf: inspection, export, verification and service tooling for the SmartNIC
// Control Fabric runtime.
// Copyright 2026 Summon Software Labs.
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "sncf/fabric.hpp"
#include "sncf/service.hpp"
#include "sncf/transport.hpp"
#include "sncf/version.hpp"

namespace {

using namespace sncf;

void print_usage() {
  std::cout << "sncf " << version_string() << " - SmartNIC Control Fabric\n\n"
            << "usage: sncf <command> [options]\n\n"
            << "commands:\n"
            << "  inspect   --store DIR [--json]      summarise the durable state\n"
            << "  export    --store DIR               print the canonical export\n"
            << "  verify    --store DIR               open the store and report recovery\n"
            << "  explain   --store DIR --instance N  explain an instance\n"
            << "  serve     --store DIR [--port N] [--connections N] [--workers N]\n"
            << "                                      serve the control-fabric protocol\n"
            << "  selftest  --store DIR               run a scripted lifecycle check\n";
}

struct Arguments {
  std::string command;
  std::string store;
  std::uint64_t instance = 0;
  std::uint16_t port = 0;
  std::size_t connections = 16;
  std::size_t workers = 4;
  bool json = false;
  bool valid = true;

  [[nodiscard]] std::size_t number(const std::string& text) const {
    try {
      return static_cast<std::size_t>(std::stoull(text));
    } catch (...) {
      return 0;
    }
  }
};

Arguments parse(int argc, char** argv) {
  Arguments arguments;
  if (argc < 2) {
    arguments.valid = false;
    return arguments;
  }
  arguments.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string token = argv[i];
    const bool has_value = (i + 1) < argc;
    if (token == "--store" && has_value) {
      arguments.store = argv[++i];
    } else if (token == "--instance" && has_value) {
      arguments.instance = std::strtoull(argv[++i], nullptr, 10);
    } else if (token == "--port" && has_value) {
      arguments.port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (token == "--connections" && has_value) {
      arguments.connections = arguments.number(argv[++i]);
    } else if (token == "--workers" && has_value) {
      arguments.workers = arguments.number(argv[++i]);
    } else if (token == "--json") {
      arguments.json = true;
    } else if (token == "--help" || token == "-h") {
      arguments.valid = false;
      return arguments;
    } else {
      std::cerr << "unrecognised option: " << token << "\n";
      arguments.valid = false;
      return arguments;
    }
  }
  return arguments;
}

/// Opens the store. Inspection commands open it read-only so a diagnostic can
/// never revoke the authority of a coordinator that owns the same store.
Result<std::unique_ptr<Fabric>> open_store(const Arguments& arguments, bool read_only) {
  if (arguments.store.empty()) {
    return Status(ReasonCode::RefusedMissingField, "--store DIR is required");
  }
  Fabric::Options options;
  options.store_directory = arguments.store;
  options.read_only = read_only;
  return Fabric::open(std::move(options));
}

int report_failure(const Status& status) {
  std::cerr << "refused: " << reason_name(status.code());
  if (!status.detail().empty()) {
    std::cerr << ": " << status.detail();
  }
  std::cerr << "\n";
  return 2;
}

int command_verify(const Arguments& arguments) {
  auto fabric = open_store(arguments, true);
  if (!fabric.ok()) {
    return report_failure(fabric.status());
  }
  const RecoveryReport report = fabric.value()->recovery();
  std::cout << "disposition=" << recovery_disposition_name(report.disposition) << "\n"
            << "usable=" << (recovery_is_usable(report.disposition) ? "true" : "false") << "\n"
            << "records_recovered=" << report.records_recovered << "\n"
            << "records_dropped=" << report.records_dropped << "\n"
            << "bytes_dropped=" << report.bytes_dropped << "\n"
            << "snapshot_sequence=" << report.snapshot_sequence << "\n"
            << "epoch=" << fabric.value()->epoch().value() << "\n"
            << "boot=" << fabric.value()->boot().value() << "\n";
  return recovery_is_usable(report.disposition) ? 0 : 3;
}

int command_inspect(const Arguments& arguments) {
  auto fabric = open_store(arguments, true);
  if (!fabric.ok()) {
    return report_failure(fabric.status());
  }
  if (arguments.json) {
    std::cout << fabric.value()->export_canonical() << "\n";
    return 0;
  }
  const RecoveryReport report = fabric.value()->recovery();
  std::cout << "epoch=" << fabric.value()->epoch().value() << "\n"
            << "boot=" << fabric.value()->boot().value() << "\n"
            << "recovery=" << recovery_disposition_name(report.disposition) << "\n"
            << "durable_digest=" << fabric.value()->durable_digest().to_hex() << "\n";
  const FabricCounters counters = fabric.value()->counters();
  std::cout << "commands_accepted=" << counters.commands_accepted << "\n"
            << "commands_refused=" << counters.commands_refused << "\n"
            << "journal_records_appended=" << counters.journal_records_appended << "\n";
  std::cout << "refusals_by_reason:\n";
  for (std::uint16_t raw = 1; raw <= kMaxReasonCodeValue; ++raw) {
    const auto code = static_cast<ReasonCode>(raw);
    const std::uint64_t count = fabric.value()->reasons().count(code);
    if (count > 0U) {
      std::cout << "  " << reason_name(code) << "=" << count << "\n";
    }
  }
  return 0;
}

int command_export(const Arguments& arguments) {
  auto fabric = open_store(arguments, true);
  if (!fabric.ok()) {
    return report_failure(fabric.status());
  }
  std::cout << fabric.value()->export_canonical() << "\n";
  return 0;
}

int command_explain(const Arguments& arguments) {
  auto fabric = open_store(arguments, true);
  if (!fabric.ok()) {
    return report_failure(fabric.status());
  }
  auto explanation = fabric.value()->explain_instance(FunctionInstanceId::from_value(arguments.instance));
  if (!explanation.ok()) {
    return report_failure(explanation.status());
  }
  std::cout << explanation.value().to_canonical() << "\n";
  return 0;
}

int command_serve(const Arguments& arguments) {
  auto fabric = open_store(arguments, false);
  if (!fabric.ok()) {
    return report_failure(fabric.status());
  }
  Server::Options server_options;
  server_options.port = arguments.port;
  server_options.max_connections = arguments.connections;
  server_options.worker_threads = arguments.workers;
  auto server = Server::start(*fabric.value(), server_options);
  if (!server.ok()) {
    return report_failure(server.status());
  }
  std::cout << "listening on 127.0.0.1:" << server.value()->port() << "\n";
  std::cout.flush();
  server.value()->wait_for_shutdown();
  server.value()->stop();
  fabric.value()->shutdown();
  std::cout << "stopped\n";
  return 0;
}

/// A scripted, self-contained lifecycle used to demonstrate that an installed
/// artifact really runs. Lives only in the CLI: it is not part of the library.
int command_selftest(const Arguments& arguments) {
  if (arguments.store.empty()) {
    std::cerr << "--store DIR is required\n";
    return 2;
  }
  auto opened = open_store(arguments, false);
  if (!opened.ok()) {
    return report_failure(opened.status());
  }
  Fabric& fabric = *opened.value();
  std::uint64_t command = 1;
  const auto header = [&command] {
    CommandHeader value;
    value.command = CommandId::from_value(command++);
    value.principal = PrincipalId::from_value(1);
    return value;
  };
  const auto digest = [](char fill) {
    return Digest256::from_hex(std::string(64, fill)).value();
  };

  RegisterSmartNicRequest smartnic;
  smartnic.header = header();
  smartnic.label = "selftest-nic";
  smartnic.port_count = 2;
  auto smartnic_result = fabric.register_smartnic(smartnic);
  if (!smartnic_result.ok()) {
    return report_failure(smartnic_result.status());
  }
  RegisterDeviceRequest device;
  device.header = header();
  device.smartnic = smartnic_result.value().id;
  device.model = DeviceModelId::from_value(1);
  device.incarnation = DeviceIncarnation::from_value(1);
  device.serial = "selftest-serial";
  device.port = PortId::from_value(1);
  auto device_result = fabric.register_device(device);
  if (!device_result.ok()) {
    return report_failure(device_result.status());
  }
  RegisterPackageRequest package;
  package.header = header();
  package.name = "selftest-function";
  package.version = SemVer{1, 0, 0};
  package.digest = digest('a');
  package.supported_models = {DeviceModelId::from_value(1)};
  package.required_capabilities = {CapabilityCode::PacketParsing};
  package.min_capability_generation = CapabilityGeneration::from_value(1);
  package.compatibility.min = CompatibilityGeneration::from_value(1);
  package.compatibility.max = CompatibilityGeneration::from_value(8);
  package.min_firmware = SemVer{1, 0, 0};
  package.exclusive_scope = true;
  auto package_result = fabric.register_package(package);
  if (!package_result.ok()) {
    return report_failure(package_result.status());
  }
  CapabilityEvidenceRequest evidence;
  evidence.header = header();
  evidence.source = EvidenceSource::SyntheticFixture;
  evidence.payload_digest = digest('b');
  evidence.observed_at = SystemClock{}.now();
  evidence.synthetic = true;
  evidence.smartnic = smartnic_result.value().id;
  evidence.device = device_result.value().id;
  evidence.incarnation = device.incarnation;
  evidence.model = device.model;
  evidence.capability_generation = CapabilityGeneration::from_value(3);
  evidence.compatibility_generation = CompatibilityGeneration::from_value(4);
  evidence.firmware = SemVer{1, 2, 0};
  evidence.capabilities = {CapabilityCode::PacketParsing};
  if (auto result = fabric.submit_capability_evidence(evidence); !result.ok()) {
    return report_failure(result.status());
  }
  ObservationRequest observation;
  observation.header = header();
  observation.source = EvidenceSource::SyntheticFixture;
  observation.payload_digest = digest('c');
  observation.observed_at = SystemClock{}.now();
  observation.synthetic = true;
  observation.smartnic = smartnic_result.value().id;
  observation.device = device_result.value().id;
  observation.incarnation = device.incarnation;
  observation.present = true;
  observation.capability_generation = evidence.capability_generation;
  observation.compatibility_generation = evidence.compatibility_generation;
  if (auto result = fabric.submit_observation(observation); !result.ok()) {
    return report_failure(result.status());
  }
  ActivateRequest activation;
  activation.header = header();
  activation.smartnic = smartnic_result.value().id;
  activation.device = device_result.value().id;
  activation.incarnation = device.incarnation;
  activation.package = package_result.value().id;
  activation.port = device.port;
  activation.queue = QueueId::from_value(1);
  auto activation_result = fabric.activate(activation);
  if (!activation_result.ok()) {
    return report_failure(activation_result.status());
  }
  EffectReportRequest effect;
  effect.header = header();
  effect.instance = activation_result.value().instance;
  effect.attempt = activation_result.value().attempt;
  effect.generation = activation_result.value().generation;
  effect.token = activation_result.value().token;
  effect.outcome = EffectOutcome::Verified;
  effect.source = EvidenceSource::SyntheticFixture;
  effect.payload_digest = digest('d');
  effect.observed_at = SystemClock{}.now();
  effect.synthetic = true;
  effect.detail = "selftest verification";
  if (auto result = fabric.report_effect(effect); !result.ok()) {
    return report_failure(result.status());
  }
  auto instance = fabric.inspect_instance(activation_result.value().instance);
  if (!instance.ok()) {
    return report_failure(instance.status());
  }
  if (instance.value().lifecycle != LifecycleState::Verified) {
    std::cerr << "selftest failed: lifecycle is "
              << lifecycle_state_name(instance.value().lifecycle) << "\n";
    return 4;
  }
  std::cout << "selftest ok instance=" << activation_result.value().instance.value()
            << " lifecycle=" << lifecycle_state_name(instance.value().lifecycle)
            << " durable_digest=" << fabric.durable_digest().to_hex() << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Arguments arguments = parse(argc, argv);
  if (!arguments.valid) {
    print_usage();
    return argc < 2 ? 1 : 0;
  }
  if (arguments.command == "verify") {
    return command_verify(arguments);
  }
  if (arguments.command == "inspect") {
    return command_inspect(arguments);
  }
  if (arguments.command == "export") {
    return command_export(arguments);
  }
  if (arguments.command == "explain") {
    return command_explain(arguments);
  }
  if (arguments.command == "serve") {
    return command_serve(arguments);
  }
  if (arguments.command == "selftest") {
    return command_selftest(arguments);
  }
  print_usage();
  return 1;
}
