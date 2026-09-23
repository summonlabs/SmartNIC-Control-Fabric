// Copyright 2026 Summon Software Labs.
#include "sncf/service.hpp"

#include <algorithm>
#include <limits>

#include "sncf/version.hpp"

namespace sncf {
namespace {

Result<CommandHeader> decode_header(const Value& args) noexcept {
  auto command = field_uint(args, "command");
  SNCF_TRY(command);
  auto principal = field_uint(args, "principal");
  SNCF_TRY(principal);
  CommandHeader header;
  header.command = CommandId::from_value(command.value());
  header.principal = PrincipalId::from_value(principal.value());
  if (!header.command.valid() || !header.principal.valid()) {
    return Status(ReasonCode::RefusedNilIdentity, "command and principal ids must be non-zero");
  }
  return header;
}

Result<SemVer> decode_semver_field(const Value& args, std::string_view key) noexcept {
  auto value = field_value(args, key);
  SNCF_TRY(value);
  return semver_from_value(*value.value());
}

Result<std::uint64_t> u64_field(const Value& args, std::string_view key) noexcept {
  auto value = field_uint(args, key);
  SNCF_TRY(value);
  return value.value();
}

Result<CoordinatorEpoch> decode_epoch(const Value& args, std::string_view key) noexcept {
  auto value = field_uint(args, key);
  SNCF_TRY(value);
  return CoordinatorEpoch::from_value(value.value());
}

Result<ExclusiveScope> decode_scope(const Value& object) noexcept {
  auto smartnic = field_uint(object, "smartnic");
  SNCF_TRY(smartnic);
  auto device = field_uint(object, "device");
  SNCF_TRY(device);
  auto port = field_uint(object, "port");
  SNCF_TRY(port);
  const auto port16 = narrow_checked<std::uint16_t>(port.value());
  if (!port16.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "scope port exceeds 16 bits");
  }
  ExclusiveScope scope;
  scope.smartnic = SmartNicId::from_value(smartnic.value());
  scope.device = DeviceId::from_value(device.value());
  scope.port = PortId::from_value(*port16);
  if (!scope.valid()) {
    return Status(ReasonCode::RefusedNilIdentity, "scope must name a SmartNIC and a device");
  }
  return scope;
}

Result<AuthorityClaim> decode_claim(const Value& args) noexcept {
  auto claim_value = field_value(args, "claim");
  SNCF_TRY(claim_value);
  if (!claim_value.value()->is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "claim must be an object");
  }
  const Value& claim_object = *claim_value.value();
  auto scope_object = field_value(claim_object, "scope");
  SNCF_TRY(scope_object);
  auto scope = decode_scope(*scope_object.value());
  SNCF_TRY(scope);
  AuthorityClaim claim;
  claim.scope = scope.value();
  auto instance = field_uint(claim_object, "instance");
  SNCF_TRY(instance);
  claim.instance = FunctionInstanceId::from_value(instance.value());
  auto lease = field_uint(claim_object, "lease");
  SNCF_TRY(lease);
  claim.lease = LeaseId::from_value(lease.value());
  auto token = field_uint(claim_object, "token");
  SNCF_TRY(token);
  claim.token = FencingToken::from_value(token.value());
  auto epoch = decode_epoch(claim_object, "epoch");
  SNCF_TRY(epoch);
  claim.epoch = epoch.value();
  auto principal = field_uint(claim_object, "principal");
  SNCF_TRY(principal);
  claim.principal = PrincipalId::from_value(principal.value());
  return claim;
}

Result<std::vector<DeviceModelId>> decode_models(const Value& args) noexcept {
  auto models = field_array(args, "supported_models");
  SNCF_TRY(models);
  if (models.value()->size() > 256U) {
    return Status(ReasonCode::RefusedOversizedInput, "at most 256 supported models are accepted");
  }
  std::vector<DeviceModelId> out;
  out.reserve(models.value()->size());
  for (const auto& entry : *models.value()) {
    if (!entry.is_uint()) {
      return Status(ReasonCode::RefusedInvalidEnumValue, "supported model must be an integer");
    }
    const auto narrowed = narrow_checked<std::uint16_t>(entry.as_uint());
    if (!narrowed.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "supported model exceeds 16 bits");
    }
    out.push_back(DeviceModelId::from_value(*narrowed));
  }
  return out;
}

Result<std::vector<CapabilityCode>> decode_capabilities(const Value& args) noexcept {
  auto capabilities = field_array(args, "capabilities");
  SNCF_TRY(capabilities);
  if (capabilities.value()->size() > 64U) {
    return Status(ReasonCode::RefusedOversizedInput, "at most 64 capabilities are accepted");
  }
  std::vector<CapabilityCode> out;
  out.reserve(capabilities.value()->size());
  for (const auto& entry : *capabilities.value()) {
    if (!entry.is_string()) {
      return Status(ReasonCode::RefusedInvalidEnumValue, "capability must be named as a string");
    }
    const CapabilityCode code = capability_from_name(entry.as_string());
    if (code == CapabilityCode::Unknown) {
      return Status(ReasonCode::RefusedInvalidEnumValue, "capability name is not known");
    }
    out.push_back(code);
  }
  return out;
}

Result<Digest256> decode_digest(const Value& args, std::string_view key) noexcept {
  auto text = field_string(args, key);
  SNCF_TRY(text);
  const auto parsed = Digest256::from_hex(text.value());
  if (!parsed.has_value()) {
    return Status(ReasonCode::RefusedInvalidDigest, std::string(key) + " must be 64 lowercase hex characters");
  }
  return *parsed;
}

Result<EvidenceSource> decode_source(const Value& args) noexcept {
  auto text = field_string(args, "source");
  SNCF_TRY(text);
  const EvidenceSource source = evidence_source_from_name(text.value());
  if (source == EvidenceSource::Unknown) {
    return Status(ReasonCode::RefusedInvalidEnumValue, "evidence source is not known");
  }
  return source;
}

Value outcome_value(ReasonCode reason, bool duplicate, RecordSequence sequence) {
  return Value::object({
      {"duplicate", Value::boolean(duplicate)},
      {"ok", Value::boolean(true)},
      {"reason", Value::string(std::string(reason_name(reason)))},
      {"sequence", Value::uint_value(sequence.value())},
  });
}

Value merge(Value::object_type fields) { return Value::object(std::move(fields)); }

[[maybe_unused]] Result<Value> ok_with(Value::object_type fields) {
  return Value::object(std::move(fields));
}


}  // namespace

std::string_view FabricService::operations_help() noexcept {
  return "ping register_smartnic register_device register_package submit_capability_evidence "
         "submit_observation reverify_evidence acquire_authority release_authority plan_deployment "
         "activate acknowledge report_effect request_quiesce request_withdraw request_rollback "
         "inspect_instance explain_instance explain_last_decision export counters recovery events "
         "history shutdown";
}

Result<Value> FabricService::dispatch(std::string_view operation, const Value& args) noexcept {
  if (!args.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "arguments must be an object");
  }
  if (operation == "ping") {
    return merge({
        {"boot", Value::uint_value(fabric_.boot().value())},
        {"epoch", Value::uint_value(fabric_.epoch().value())},
        {"ok", Value::boolean(true)},
        {"semantics", Value::string(std::string(semantics_id()))},
        {"version", Value::string(std::string(version_string()))},
    });
  }
  if (operation == "register_smartnic") {
    RegisterSmartNicRequest request;
    auto header = decode_header(args);
    SNCF_TRY(header);
    request.header = header.value();
    auto label = field_string(args, "label");
    SNCF_TRY(label);
    request.label = label.value();
    auto ports = u64_field(args, "port_count");
    SNCF_TRY(ports);
    const auto port_count = narrow_checked<std::uint32_t>(ports.value());
    if (!port_count.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "port_count exceeds 32 bits");
    }
    request.port_count = *port_count;
    auto result = fabric_.register_smartnic(std::move(request));
    SNCF_TRY(result);
    return merge({
        {"id", Value::uint_value(result.value().id.value())},
        {"duplicate", Value::boolean(result.value().outcome.duplicate)},
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(result.value().outcome.reason)))},
        {"sequence", Value::uint_value(result.value().outcome.sequence.value())},
    });
  }
  if (operation == "register_device") {
    RegisterDeviceRequest request;
    auto header = decode_header(args);
    SNCF_TRY(header);
    request.header = header.value();
    auto smartnic = u64_field(args, "smartnic");
    SNCF_TRY(smartnic);
    request.smartnic = SmartNicId::from_value(smartnic.value());
    auto model = u64_field(args, "model");
    SNCF_TRY(model);
    const auto model16 = narrow_checked<std::uint16_t>(model.value());
    if (!model16.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "model exceeds 16 bits");
    }
    request.model = DeviceModelId::from_value(*model16);
    auto incarnation = u64_field(args, "incarnation");
    SNCF_TRY(incarnation);
    const auto incarnation32 = narrow_checked<std::uint32_t>(incarnation.value());
    if (!incarnation32.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "incarnation exceeds 32 bits");
    }
    request.incarnation = DeviceIncarnation::from_value(*incarnation32);
    auto serial = field_string(args, "serial");
    SNCF_TRY(serial);
    request.serial = serial.value();
    auto port = u64_field(args, "port");
    SNCF_TRY(port);
    const auto port16 = narrow_checked<std::uint16_t>(port.value());
    if (!port16.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "port exceeds 16 bits");
    }
    request.port = PortId::from_value(*port16);
    auto result = fabric_.register_device(std::move(request));
    SNCF_TRY(result);
    return merge({
        {"id", Value::uint_value(result.value().id.value())},
        {"duplicate", Value::boolean(result.value().outcome.duplicate)},
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(result.value().outcome.reason)))},
        {"sequence", Value::uint_value(result.value().outcome.sequence.value())},
    });
  }
  if (operation == "register_package") {
    RegisterPackageRequest request;
    auto header = decode_header(args);
    SNCF_TRY(header);
    request.header = header.value();
    auto name = field_string(args, "name");
    SNCF_TRY(name);
    request.name = name.value();
    auto version = decode_semver_field(args, "version");
    SNCF_TRY(version);
    request.version = version.value();
    auto digest = decode_digest(args, "digest");
    SNCF_TRY(digest);
    request.digest = digest.value();
    auto models = decode_models(args);
    SNCF_TRY(models);
    request.supported_models = models.value();
    auto capabilities = decode_capabilities(args);
    SNCF_TRY(capabilities);
    request.required_capabilities = capabilities.value();
    auto min_capability = u64_field(args, "min_capability_generation");
    SNCF_TRY(min_capability);
    request.min_capability_generation = CapabilityGeneration::from_value(min_capability.value());
    auto compatibility = field_value(args, "compatibility");
    SNCF_TRY(compatibility);
    auto compat_min = field_uint(*compatibility.value(), "min");
    SNCF_TRY(compat_min);
    auto compat_max = field_uint(*compatibility.value(), "max");
    SNCF_TRY(compat_max);
    request.compatibility.min = CompatibilityGeneration::from_value(compat_min.value());
    request.compatibility.max = CompatibilityGeneration::from_value(compat_max.value());
    auto min_firmware = decode_semver_field(args, "min_firmware");
    SNCF_TRY(min_firmware);
    request.min_firmware = min_firmware.value();
    auto demand = field_value(args, "demand");
    SNCF_TRY(demand);
    auto demand_ports = field_uint(*demand.value(), "ports");
    SNCF_TRY(demand_ports);
    auto demand_queues = field_uint(*demand.value(), "queues");
    SNCF_TRY(demand_queues);
    auto demand_memory = field_uint(*demand.value(), "memory_bytes");
    SNCF_TRY(demand_memory);
    const auto ports32 = narrow_checked<std::uint32_t>(demand_ports.value());
    const auto queues32 = narrow_checked<std::uint32_t>(demand_queues.value());
    if (!ports32.has_value() || !queues32.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "resource demand exceeds 32 bits");
    }
    request.demand.ports = *ports32;
    request.demand.queues = *queues32;
    request.demand.memory_bytes = demand_memory.value();
    auto exclusive = field_bool(args, "exclusive_scope");
    SNCF_TRY(exclusive);
    request.exclusive_scope = exclusive.value();
    auto layout = u64_field(args, "layout_revision");
    SNCF_TRY(layout);
    request.layout_revision = LayoutRevision::from_value(layout.value());
    auto result = fabric_.register_package(std::move(request));
    SNCF_TRY(result);
    return merge({
        {"id", Value::uint_value(result.value().id.value())},
        {"duplicate", Value::boolean(result.value().outcome.duplicate)},
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(result.value().outcome.reason)))},
        {"sequence", Value::uint_value(result.value().outcome.sequence.value())},
    });
  }
  if (operation == "submit_capability_evidence") {
    CapabilityEvidenceRequest request;
    auto header = decode_header(args);
    SNCF_TRY(header);
    request.header = header.value();
    auto source = decode_source(args);
    SNCF_TRY(source);
    request.source = source.value();
    auto digest = decode_digest(args, "payload_digest");
    SNCF_TRY(digest);
    request.payload_digest = digest.value();
    auto observed = u64_field(args, "observed_at");
    SNCF_TRY(observed);
    request.observed_at = TimestampNs::from_value(observed.value());
    auto synthetic = field_bool(args, "synthetic");
    SNCF_TRY(synthetic);
    request.synthetic = synthetic.value();
    auto smartnic = u64_field(args, "smartnic");
    SNCF_TRY(smartnic);
    request.smartnic = SmartNicId::from_value(smartnic.value());
    auto device = u64_field(args, "device");
    SNCF_TRY(device);
    request.device = DeviceId::from_value(device.value());
    auto incarnation = u64_field(args, "incarnation");
    SNCF_TRY(incarnation);
    const auto incarnation32 = narrow_checked<std::uint32_t>(incarnation.value());
    if (!incarnation32.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "incarnation exceeds 32 bits");
    }
    request.incarnation = DeviceIncarnation::from_value(*incarnation32);
    auto model = u64_field(args, "model");
    SNCF_TRY(model);
    const auto model16 = narrow_checked<std::uint16_t>(model.value());
    if (!model16.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "model exceeds 16 bits");
    }
    request.model = DeviceModelId::from_value(*model16);
    auto capability_generation = u64_field(args, "capability_generation");
    SNCF_TRY(capability_generation);
    request.capability_generation = CapabilityGeneration::from_value(capability_generation.value());
    auto compatibility_generation = u64_field(args, "compatibility_generation");
    SNCF_TRY(compatibility_generation);
    request.compatibility_generation = CompatibilityGeneration::from_value(compatibility_generation.value());
    auto firmware = decode_semver_field(args, "firmware");
    SNCF_TRY(firmware);
    request.firmware = firmware.value();
    auto capabilities = decode_capabilities(args);
    SNCF_TRY(capabilities);
    request.capabilities = capabilities.value();
    auto result = fabric_.submit_capability_evidence(std::move(request));
    SNCF_TRY(result);
    return merge({
        {"duplicate", Value::boolean(result.value().outcome.duplicate)},
        {"freshness", Value::string(std::string(freshness_name(result.value().freshness)))},
        {"id", Value::uint_value(result.value().id.value())},
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(result.value().outcome.reason)))},
        {"sequence", Value::uint_value(result.value().outcome.sequence.value())},
    });
  }
  if (operation == "submit_observation") {
    ObservationRequest request;
    auto header = decode_header(args);
    SNCF_TRY(header);
    request.header = header.value();
    auto source = decode_source(args);
    SNCF_TRY(source);
    request.source = source.value();
    auto digest = decode_digest(args, "payload_digest");
    SNCF_TRY(digest);
    request.payload_digest = digest.value();
    auto observed = u64_field(args, "observed_at");
    SNCF_TRY(observed);
    request.observed_at = TimestampNs::from_value(observed.value());
    auto synthetic = field_bool(args, "synthetic");
    SNCF_TRY(synthetic);
    request.synthetic = synthetic.value();
    auto smartnic = u64_field(args, "smartnic");
    SNCF_TRY(smartnic);
    request.smartnic = SmartNicId::from_value(smartnic.value());
    auto device = u64_field(args, "device");
    SNCF_TRY(device);
    request.device = DeviceId::from_value(device.value());
    auto incarnation = u64_field(args, "incarnation");
    SNCF_TRY(incarnation);
    const auto incarnation32 = narrow_checked<std::uint32_t>(incarnation.value());
    if (!incarnation32.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "incarnation exceeds 32 bits");
    }
    request.incarnation = DeviceIncarnation::from_value(*incarnation32);
    auto present = field_bool(args, "present");
    SNCF_TRY(present);
    request.present = present.value();
    auto capability_generation = u64_field(args, "capability_generation");
    SNCF_TRY(capability_generation);
    request.capability_generation = CapabilityGeneration::from_value(capability_generation.value());
    auto compatibility_generation = u64_field(args, "compatibility_generation");
    SNCF_TRY(compatibility_generation);
    request.compatibility_generation = CompatibilityGeneration::from_value(compatibility_generation.value());
    auto result = fabric_.submit_observation(std::move(request));
    SNCF_TRY(result);
    return merge({
        {"duplicate", Value::boolean(result.value().outcome.duplicate)},
        {"freshness", Value::string(std::string(freshness_name(result.value().freshness)))},
        {"id", Value::uint_value(result.value().id.value())},
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(result.value().outcome.reason)))},
        {"sequence", Value::uint_value(result.value().outcome.sequence.value())},
    });
  }
  if (operation == "reverify_evidence") {
    ReverifyEvidenceRequest request;
    auto header = decode_header(args);
    SNCF_TRY(header);
    request.header = header.value();
    auto source = decode_source(args);
    SNCF_TRY(source);
    request.source = source.value();
    auto digest = decode_digest(args, "payload_digest");
    SNCF_TRY(digest);
    request.payload_digest = digest.value();
    auto observed = u64_field(args, "observed_at");
    SNCF_TRY(observed);
    request.observed_at = TimestampNs::from_value(observed.value());
    auto device = u64_field(args, "device");
    SNCF_TRY(device);
    request.device = DeviceId::from_value(device.value());
    auto incarnation = u64_field(args, "incarnation");
    SNCF_TRY(incarnation);
    const auto incarnation32 = narrow_checked<std::uint32_t>(incarnation.value());
    if (!incarnation32.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "incarnation exceeds 32 bits");
    }
    request.incarnation = DeviceIncarnation::from_value(*incarnation32);
    auto result = fabric_.reverify_evidence(std::move(request));
    SNCF_TRY(result);
    return merge({
        {"duplicate", Value::boolean(result.value().outcome.duplicate)},
        {"freshness", Value::string(std::string(freshness_name(result.value().freshness)))},
        {"id", Value::uint_value(result.value().id.value())},
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(result.value().outcome.reason)))},
        {"sequence", Value::uint_value(result.value().outcome.sequence.value())},
    });
  }
  if (operation == "acquire_authority") {
    AcquireAuthorityRequest request;
    auto header = decode_header(args);
    SNCF_TRY(header);
    request.header = header.value();
    auto instance = u64_field(args, "instance");
    SNCF_TRY(instance);
    request.instance = FunctionInstanceId::from_value(instance.value());
    auto scope_value = field_value(args, "scope");
    SNCF_TRY(scope_value);
    auto scope = decode_scope(*scope_value.value());
    SNCF_TRY(scope);
    request.scope = scope.value();
    auto ttl = u64_field(args, "ttl_ns");
    SNCF_TRY(ttl);
    request.ttl = DurationNs::from_value(ttl.value());
    auto result = fabric_.acquire_authority(std::move(request));
    SNCF_TRY(result);
    return merge({
        {"duplicate", Value::boolean(result.value().outcome.duplicate)},
        {"expires_at", Value::uint_value(result.value().expires_at.value())},
        {"lease", Value::uint_value(result.value().lease.value())},
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(result.value().outcome.reason)))},
        {"sequence", Value::uint_value(result.value().outcome.sequence.value())},
        {"token", Value::uint_value(result.value().token.value())},
    });
  }
  if (operation == "release_authority") {
    ReleaseAuthorityRequest request;
    auto header = decode_header(args);
    SNCF_TRY(header);
    request.header = header.value();
    auto instance = u64_field(args, "instance");
    SNCF_TRY(instance);
    request.instance = FunctionInstanceId::from_value(instance.value());
    auto lease = u64_field(args, "lease");
    SNCF_TRY(lease);
    request.lease = LeaseId::from_value(lease.value());
    auto token = u64_field(args, "token");
    SNCF_TRY(token);
    request.token = FencingToken::from_value(token.value());
    auto result = fabric_.release_authority(std::move(request));
    SNCF_TRY(result);
    return merge({
        {"duplicate", Value::boolean(result.value().duplicate)},
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(result.value().reason)))},
        {"sequence", Value::uint_value(result.value().sequence.value())},
    });
  }
  if (operation == "plan_deployment") {
    PlanRequest request;
    auto smartnic = u64_field(args, "smartnic");
    SNCF_TRY(smartnic);
    request.smartnic = SmartNicId::from_value(smartnic.value());
    auto device = u64_field(args, "device");
    SNCF_TRY(device);
    request.device = DeviceId::from_value(device.value());
    auto incarnation = u64_field(args, "incarnation");
    SNCF_TRY(incarnation);
    const auto incarnation32 = narrow_checked<std::uint32_t>(incarnation.value());
    if (!incarnation32.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "incarnation exceeds 32 bits");
    }
    request.incarnation = DeviceIncarnation::from_value(*incarnation32);
    auto package = u64_field(args, "package");
    SNCF_TRY(package);
    request.package = FunctionPackageId::from_value(package.value());
    auto port = u64_field(args, "port");
    SNCF_TRY(port);
    const auto port16 = narrow_checked<std::uint16_t>(port.value());
    if (!port16.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "port exceeds 16 bits");
    }
    request.port = PortId::from_value(*port16);
    auto queue = u64_field(args, "queue");
    SNCF_TRY(queue);
    const auto queue32 = narrow_checked<std::uint32_t>(queue.value());
    if (!queue32.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "queue exceeds 32 bits");
    }
    request.queue = QueueId::from_value(*queue32);
    auto instance = u64_field(args, "instance");
    SNCF_TRY(instance);
    request.instance = FunctionInstanceId::from_value(instance.value());
    auto policy = u64_field(args, "policy_generation");
    SNCF_TRY(policy);
    request.policy_generation = PolicyGeneration::from_value(policy.value());
    auto result = fabric_.plan_deployment(request);
    SNCF_TRY(result);
    return merge({
        {"ok", Value::boolean(true)},
        {"plan", result.value().to_value()},
    });
  }
  if (operation == "activate") {
    ActivateRequest request;
    auto header = decode_header(args);
    SNCF_TRY(header);
    request.header = header.value();
    auto smartnic = u64_field(args, "smartnic");
    SNCF_TRY(smartnic);
    request.smartnic = SmartNicId::from_value(smartnic.value());
    auto device = u64_field(args, "device");
    SNCF_TRY(device);
    request.device = DeviceId::from_value(device.value());
    auto incarnation = u64_field(args, "incarnation");
    SNCF_TRY(incarnation);
    const auto incarnation32 = narrow_checked<std::uint32_t>(incarnation.value());
    if (!incarnation32.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "incarnation exceeds 32 bits");
    }
    request.incarnation = DeviceIncarnation::from_value(*incarnation32);
    auto package = u64_field(args, "package");
    SNCF_TRY(package);
    request.package = FunctionPackageId::from_value(package.value());
    auto port = u64_field(args, "port");
    SNCF_TRY(port);
    const auto port16 = narrow_checked<std::uint16_t>(port.value());
    if (!port16.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "port exceeds 16 bits");
    }
    request.port = PortId::from_value(*port16);
    auto queue = u64_field(args, "queue");
    SNCF_TRY(queue);
    const auto queue32 = narrow_checked<std::uint32_t>(queue.value());
    if (!queue32.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "queue exceeds 32 bits");
    }
    request.queue = QueueId::from_value(*queue32);
    auto instance = u64_field(args, "instance");
    SNCF_TRY(instance);
    request.instance = FunctionInstanceId::from_value(instance.value());
    auto claim = decode_claim(args);
    if (claim.ok()) {
      request.claim = claim.value();
    } else if (instance.value() != 0U) {
      return claim.status();
    }
    auto policy = u64_field(args, "policy_generation");
    SNCF_TRY(policy);
    request.policy_generation = PolicyGeneration::from_value(policy.value());
    auto ttl = u64_field(args, "lease_ttl_ns");
    SNCF_TRY(ttl);
    request.lease_ttl = DurationNs::from_value(ttl.value());
    auto result = fabric_.activate(std::move(request));
    SNCF_TRY(result);
    return merge({
        {"attempt", Value::uint_value(result.value().attempt.value())},
        {"duplicate", Value::boolean(result.value().outcome.duplicate)},
        {"generation", Value::uint_value(result.value().generation.value())},
        {"instance", Value::uint_value(result.value().instance.value())},
        {"lease", Value::uint_value(result.value().lease.value())},
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(result.value().outcome.reason)))},
        {"scope", Value::string(result.value().scope.to_key())},
        {"sequence", Value::uint_value(result.value().outcome.sequence.value())},
        {"token", Value::uint_value(result.value().token.value())},
    });
  }
  if (operation == "acknowledge") {
    AcknowledgeRequest request;
    auto header = decode_header(args);
    SNCF_TRY(header);
    request.header = header.value();
    auto instance = u64_field(args, "instance");
    SNCF_TRY(instance);
    request.instance = FunctionInstanceId::from_value(instance.value());
    auto attempt = u64_field(args, "attempt");
    SNCF_TRY(attempt);
    const auto attempt32 = narrow_checked<std::uint32_t>(attempt.value());
    if (!attempt32.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "attempt exceeds 32 bits");
    }
    request.attempt = AttemptId::from_value(*attempt32);
    auto generation = u64_field(args, "generation");
    SNCF_TRY(generation);
    request.generation = DeploymentGeneration::from_value(generation.value());
    auto token = u64_field(args, "token");
    SNCF_TRY(token);
    request.token = FencingToken::from_value(token.value());
    auto source = decode_source(args);
    SNCF_TRY(source);
    request.source = source.value();
    auto observed = u64_field(args, "observed_at");
    SNCF_TRY(observed);
    request.observed_at = TimestampNs::from_value(observed.value());
    auto result = fabric_.acknowledge(std::move(request));
    SNCF_TRY(result);
    return merge({
        {"duplicate", Value::boolean(result.value().duplicate)},
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(result.value().reason)))},
        {"sequence", Value::uint_value(result.value().sequence.value())},
    });
  }
  if (operation == "report_effect") {
    EffectReportRequest request;
    auto header = decode_header(args);
    SNCF_TRY(header);
    request.header = header.value();
    auto instance = u64_field(args, "instance");
    SNCF_TRY(instance);
    request.instance = FunctionInstanceId::from_value(instance.value());
    auto attempt = u64_field(args, "attempt");
    SNCF_TRY(attempt);
    const auto attempt32 = narrow_checked<std::uint32_t>(attempt.value());
    if (!attempt32.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "attempt exceeds 32 bits");
    }
    request.attempt = AttemptId::from_value(*attempt32);
    auto generation = u64_field(args, "generation");
    SNCF_TRY(generation);
    request.generation = DeploymentGeneration::from_value(generation.value());
    auto token = u64_field(args, "token");
    SNCF_TRY(token);
    request.token = FencingToken::from_value(token.value());
    auto outcome = field_string(args, "outcome");
    SNCF_TRY(outcome);
    request.outcome = effect_outcome_from_name(outcome.value());
    if (request.outcome == EffectOutcome::Unknown) {
      return Status(ReasonCode::RefusedInvalidEnumValue, "effect outcome is not known");
    }
    auto source = decode_source(args);
    SNCF_TRY(source);
    request.source = source.value();
    auto digest = decode_digest(args, "payload_digest");
    SNCF_TRY(digest);
    request.payload_digest = digest.value();
    auto observed = u64_field(args, "observed_at");
    SNCF_TRY(observed);
    request.observed_at = TimestampNs::from_value(observed.value());
    auto synthetic = field_bool(args, "synthetic");
    SNCF_TRY(synthetic);
    request.synthetic = synthetic.value();
    auto detail = field_string(args, "detail");
    SNCF_TRY(detail);
    request.detail = detail.value();
    auto result = fabric_.report_effect(std::move(request));
    SNCF_TRY(result);
    return merge({
        {"duplicate", Value::boolean(result.value().duplicate)},
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(result.value().reason)))},
        {"sequence", Value::uint_value(result.value().sequence.value())},
    });
  }
  if (operation == "request_quiesce" || operation == "request_withdraw" || operation == "request_rollback") {
    auto header = decode_header(args);
    SNCF_TRY(header);
    auto instance = u64_field(args, "instance");
    SNCF_TRY(instance);
    auto claim = decode_claim(args);
    SNCF_TRY(claim);
    Result<IntentAcceptance> result = Status(ReasonCode::RefusedOutOfBoundary, "unreachable");
    if (operation == "request_quiesce") {
      QuiesceRequest request;
      request.header = header.value();
      request.instance = FunctionInstanceId::from_value(instance.value());
      request.claim = claim.value();
      auto reason = field_string(args, "reason");
      if (reason.ok()) {
        request.reason = reason_from_name(reason.value());
        if (reason.value() != "unknown" && request.reason == ReasonCode::Unknown) {
          return Status(ReasonCode::RefusedInvalidEnumValue, "the drain reason is not a known reason code");
        }
      }
      result = fabric_.request_quiesce(std::move(request));
    } else if (operation == "request_withdraw") {
      WithdrawRequest request;
      request.header = header.value();
      request.instance = FunctionInstanceId::from_value(instance.value());
      request.claim = claim.value();
      auto reason = field_string(args, "reason");
      if (reason.ok()) {
        request.reason = reason_from_name(reason.value());
        if (reason.value() != "unknown" && request.reason == ReasonCode::Unknown) {
          return Status(ReasonCode::RefusedInvalidEnumValue, "the withdrawal reason is not a known reason code");
        }
      }
      result = fabric_.request_withdraw(std::move(request));
    } else {
      RollbackRequest request;
      request.header = header.value();
      request.instance = FunctionInstanceId::from_value(instance.value());
      request.claim = claim.value();
      auto ttl = u64_field(args, "lease_ttl_ns");
      SNCF_TRY(ttl);
      request.lease_ttl = DurationNs::from_value(ttl.value());
      result = fabric_.request_rollback(std::move(request));
    }
    SNCF_TRY(result);
    return merge({
        {"authority", Value::string(std::string(authority_state_name(result.value().authority)))},
        {"desired", Value::string(std::string(desired_state_name(result.value().desired)))},
        {"duplicate", Value::boolean(result.value().outcome.duplicate)},
        {"lifecycle", Value::string(std::string(lifecycle_state_name(result.value().lifecycle)))},
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(result.value().outcome.reason)))},
        {"sequence", Value::uint_value(result.value().outcome.sequence.value())},
    });
  }
  if (operation == "inspect_instance") {
    auto instance = u64_field(args, "instance");
    SNCF_TRY(instance);
    auto result = fabric_.inspect_instance(FunctionInstanceId::from_value(instance.value()));
    SNCF_TRY(result);
    return merge({
        {"instance", instance_to_value(result.value())},
        {"ok", Value::boolean(true)},
    });
  }
  if (operation == "explain_instance") {
    auto instance = u64_field(args, "instance");
    SNCF_TRY(instance);
    auto result = fabric_.explain_instance(FunctionInstanceId::from_value(instance.value()));
    SNCF_TRY(result);
    return merge({
        {"explanation", result.value().to_value()},
        {"ok", Value::boolean(true)},
    });
  }
  if (operation == "explain_last_decision") {
    auto result = fabric_.explain_last_decision();
    SNCF_TRY(result);
    return merge({
        {"explanation", result.value().to_value()},
        {"ok", Value::boolean(true)},
    });
  }
  if (operation == "export") {
    return merge({
        {"export", fabric_.export_value()},
        {"ok", Value::boolean(true)},
    });
  }
  if (operation == "counters") {
    return merge({
        {"counters", fabric_.counters().to_value()},
        {"ok", Value::boolean(true)},
        {"reasons", fabric_.reasons().to_value()},
    });
  }
  if (operation == "recovery") {
    return merge({
        {"ok", Value::boolean(true)},
        {"recovery", fabric_.recovery().to_value()},
    });
  }
  if (operation == "events" || operation == "history") {
    const std::vector<FabricEvent> items = operation == "events" ? fabric_.events() : fabric_.history();
    Value::array_type array;
    array.reserve(items.size());
    for (const auto& event : items) {
      array.push_back(Value::object({
          {"at", Value::uint_value(event.at.value())},
          {"detail", Value::string(event.detail)},
          {"reason", Value::string(std::string(reason_name(event.reason)))},
          {"sequence", Value::uint_value(event.sequence.value())},
          {"subject", Value::string(event.subject)},
      }));
    }
    return merge({
        {"items", Value::array(std::move(array))},
        {"ok", Value::boolean(true)},
    });
  }
  if (operation == "shutdown") {
    shutdown_requested_.store(true, std::memory_order_release);
    return merge({
        {"ok", Value::boolean(true)},
        {"reason", Value::string(std::string(reason_name(ReasonCode::Accepted)))},
    });
  }
  return Status(ReasonCode::RefusedOutOfBoundary, "unknown operation: " + std::string(operation));
}

}  // namespace sncf
