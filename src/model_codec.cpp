// Canonical codec for coordinator state, records and evidence.
// Copyright 2026 Summon Software Labs.
#include <algorithm>

#include "sncf/model.hpp"
#include "sncf/version.hpp"

namespace sncf {
namespace {

Value::field_type field_u64(std::string_view key, std::uint64_t value) {
  return {std::string(key), Value::uint_value(value)};
}

Value::field_type field_string_value(std::string_view key, std::string_view value) {
  return {std::string(key), Value::string(std::string(value))};
}

Result<Digest256> digest_from(const Value& object, std::string_view key) noexcept {
  auto hex = field_string(object, key);
  SNCF_TRY(hex);
  const auto parsed = Digest256::from_hex(hex.value());
  if (!parsed.has_value()) {
    return Status(ReasonCode::RefusedInvalidDigest,
                  std::string(key) + " is not a 64 character lowercase hex digest");
  }
  return *parsed;
}

Value::field_type field_digest(std::string_view key, const Digest256& digest) {
  return {std::string(key), Value::string(digest.to_hex())};
}

Value envelope_to_value(const EvidenceEnvelope& envelope) {
  return Value::object({
      field_u64("accepted_at", envelope.accepted_at.value()),
      field_u64("epoch", envelope.accepted_epoch.value()),
      field_string_value("freshness", freshness_name(envelope.freshness)),
      field_u64("id", envelope.id.value()),
      field_string_value("kind", evidence_kind_name(envelope.kind)),
      field_u64("observed_at", envelope.observed_at.value()),
      {"payload_digest", Value::string(envelope.payload_digest.to_hex())},
      field_u64("policy_generation", envelope.policy_generation.value()),
      field_string_value("source", evidence_source_name(envelope.source)),
      {"synthetic", Value::boolean(envelope.synthetic)},
  });
}

Result<EvidenceEnvelope> envelope_from(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "evidence envelope must be an object");
  }
  EvidenceEnvelope envelope;
  auto id = field_uint(value, "id");
  SNCF_TRY(id);
  envelope.id = EvidenceId::from_value(id.value());

  auto kind_text = field_string(value, "kind");
  SNCF_TRY(kind_text);
  envelope.kind = evidence_kind_from_name(kind_text.value());
  if (envelope.kind == EvidenceKind::Unknown && kind_text.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "kind is not a known evidence kind");
  }

  auto source_text = field_string(value, "source");
  SNCF_TRY(source_text);
  envelope.source = evidence_source_from_name(source_text.value());
  if (envelope.source == EvidenceSource::Unknown && source_text.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "source is not a known evidence source");
  }

  auto digest = field_string(value, "payload_digest");
  SNCF_TRY(digest);
  const auto parsed_digest = Digest256::from_hex(digest.value());
  if (!parsed_digest.has_value()) {
    return Status(ReasonCode::RefusedInvalidDigest, "payload_digest is not canonical");
  }
  envelope.payload_digest = *parsed_digest;

  auto observed = field_uint(value, "observed_at");
  SNCF_TRY(observed);
  envelope.observed_at = TimestampNs::from_value(observed.value());

  auto accepted = field_uint(value, "accepted_at");
  SNCF_TRY(accepted);
  envelope.accepted_at = TimestampNs::from_value(accepted.value());

  auto epoch = field_uint(value, "epoch");
  SNCF_TRY(epoch);
  envelope.accepted_epoch = CoordinatorEpoch::from_value(epoch.value());

  auto policy = field_uint(value, "policy_generation");
  SNCF_TRY(policy);
  envelope.policy_generation = PolicyGeneration::from_value(policy.value());

  auto freshness_text = field_string(value, "freshness");
  SNCF_TRY(freshness_text);
  envelope.freshness = freshness_from_name(freshness_text.value());
  if (envelope.freshness == Freshness::Unknown && freshness_text.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "freshness is not a known value");
  }

  auto synthetic = field_bool(value, "synthetic");
  SNCF_TRY(synthetic);
  envelope.synthetic = synthetic.value();
  return envelope;
}

}  // namespace

Value smartnic_to_value(const SmartNicRecord& record) {
  return Value::object({
      field_u64("id", record.id.value()),
      {"label", Value::string(record.label)},
      field_u64("port_count", record.port_count),
      field_u64("registered_at", record.registered_at.value()),
  });
}

Result<SmartNicRecord> smartnic_from_value(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "smartnic record must be an object");
  }
  SmartNicRecord record;
  auto id = field_uint(value, "id");
  SNCF_TRY(id);
  record.id = SmartNicId::from_value(id.value());
  if (record.id.is_nil()) {
    return Status(ReasonCode::RefusedNilIdentity, "smartnic id must be non-zero");
  }
  auto label = field_string(value, "label");
  SNCF_TRY(label);
  record.label = label.value();
  auto ports = field_uint(value, "port_count");
  SNCF_TRY(ports);
  const auto port_count = narrow_checked<std::uint32_t>(ports.value());
  if (!port_count.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "port_count exceeds 32 bits");
  }
  record.port_count = *port_count;
  auto registered = field_uint(value, "registered_at");
  SNCF_TRY(registered);
  record.registered_at = TimestampNs::from_value(registered.value());
  return record;
}

Value device_to_value(const DeviceRecord& record) {
  return Value::object({
      field_u64("capability_evidence", record.capability_evidence.value()),
      field_u64("capability_generation", record.capability_generation.value()),
      {"capabilities", Value::string(record.capabilities.to_hex())},
      field_u64("compatibility_generation", record.compatibility_generation.value()),
      {"firmware", semver_to_value(record.firmware)},
      {"freshness", Value::string(std::string(freshness_name(record.capability_freshness)))},
      field_u64("id", record.id.value()),
      field_u64("incarnation", record.incarnation.value()),
      field_u64("last_observed_at", record.last_observed_at.value()),
      field_u64("model", record.model.value()),
      field_u64("port", record.port.value()),
      {"present", Value::boolean(record.present)},
      field_u64("registered_at", record.registered_at.value()),
      {"serial", Value::string(record.serial)},
      field_u64("smartnic", record.smartnic.value()),
  });
}

Result<DeviceRecord> device_from_value(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "device record must be an object");
  }
  DeviceRecord record;
  auto id = field_uint(value, "id");
  SNCF_TRY(id);
  record.id = DeviceId::from_value(id.value());
  if (record.id.is_nil()) {
    return Status(ReasonCode::RefusedNilIdentity, "device id must be non-zero");
  }
  auto smartnic = field_uint(value, "smartnic");
  SNCF_TRY(smartnic);
  record.smartnic = SmartNicId::from_value(smartnic.value());
  auto model = field_uint(value, "model");
  SNCF_TRY(model);
  const auto model16 = narrow_checked<std::uint16_t>(model.value());
  if (!model16.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "model exceeds 16 bits");
  }
  record.model = DeviceModelId::from_value(*model16);
  auto incarnation = field_uint(value, "incarnation");
  SNCF_TRY(incarnation);
  const auto incarnation32 = narrow_checked<std::uint32_t>(incarnation.value());
  if (!incarnation32.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "incarnation exceeds 32 bits");
  }
  record.incarnation = DeviceIncarnation::from_value(*incarnation32);
  auto serial = field_string(value, "serial");
  SNCF_TRY(serial);
  record.serial = serial.value();
  auto port = field_uint(value, "port");
  SNCF_TRY(port);
  const auto port16 = narrow_checked<std::uint16_t>(port.value());
  if (!port16.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "port exceeds 16 bits");
  }
  record.port = PortId::from_value(*port16);
  auto present = field_bool(value, "present");
  SNCF_TRY(present);
  record.present = present.value();
  auto capability_generation = field_uint(value, "capability_generation");
  SNCF_TRY(capability_generation);
  record.capability_generation = CapabilityGeneration::from_value(capability_generation.value());
  auto compatibility_generation = field_uint(value, "compatibility_generation");
  SNCF_TRY(compatibility_generation);
  record.compatibility_generation = CompatibilityGeneration::from_value(compatibility_generation.value());
  auto firmware = field_value(value, "firmware");
  SNCF_TRY(firmware);
  auto semver = semver_from_value(*firmware.value());
  SNCF_TRY(semver);
  record.firmware = semver.value();
  auto capabilities = field_string(value, "capabilities");
  SNCF_TRY(capabilities);
  const auto mask = CapabilityMask::from_hex(capabilities.value());
  if (!mask.has_value()) {
    return Status(ReasonCode::RefusedMalformedInput, "capabilities is not a mask");
  }
  record.capabilities = *mask;
  auto evidence = field_uint(value, "capability_evidence");
  SNCF_TRY(evidence);
  record.capability_evidence = EvidenceId::from_value(evidence.value());
  auto freshness = field_string(value, "freshness");
  SNCF_TRY(freshness);
  record.capability_freshness = freshness_from_name(freshness.value());
  if (record.capability_freshness == Freshness::Unknown && freshness.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "device freshness is not a known value");
  }
  auto observed = field_uint(value, "last_observed_at");
  SNCF_TRY(observed);
  record.last_observed_at = TimestampNs::from_value(observed.value());
  auto registered = field_uint(value, "registered_at");
  SNCF_TRY(registered);
  record.registered_at = TimestampNs::from_value(registered.value());
  return record;
}

Value package_to_value(const PackageRecord& record) {
  Value::array_type models;
  models.reserve(record.supported_models.size());
  for (const auto model : record.supported_models) {
    models.push_back(Value::uint_value(model.value()));
  }
  return Value::object({
      {"capabilities", Value::string(record.required_capabilities.to_hex())},
      field_u64("compatibility_max", record.compatibility.max.value()),
      field_u64("compatibility_min", record.compatibility.min.value()),
      field_u64("demand_memory_bytes", record.demand.memory_bytes),
      field_u64("demand_ports", record.demand.ports),
      field_u64("demand_queues", record.demand.queues),
      {"digest", Value::string(record.digest.to_hex())},
      {"exclusive_scope", Value::boolean(record.exclusive_scope)},
      field_u64("id", record.id.value()),
      field_u64("layout_revision", record.layout_revision.value()),
      field_u64("min_capability_generation", record.min_capability_generation.value()),
      {"min_firmware", semver_to_value(record.min_firmware)},
      {"name", Value::string(record.name)},
      field_u64("registered_at", record.registered_at.value()),
      {"supported_models", Value::array(std::move(models))},
      {"version", semver_to_value(record.version)},
  });
}

Result<PackageRecord> package_from_value(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "package record must be an object");
  }
  PackageRecord record;
  auto id = field_uint(value, "id");
  SNCF_TRY(id);
  record.id = FunctionPackageId::from_value(id.value());
  if (record.id.is_nil()) {
    return Status(ReasonCode::RefusedNilIdentity, "package id must be non-zero");
  }
  auto name = field_string(value, "name");
  SNCF_TRY(name);
  record.name = name.value();
  auto version = field_value(value, "version");
  SNCF_TRY(version);
  auto semver = semver_from_value(*version.value());
  SNCF_TRY(semver);
  record.version = semver.value();
  auto digest = field_string(value, "digest");
  SNCF_TRY(digest);
  const auto parsed_digest = Digest256::from_hex(digest.value());
  if (!parsed_digest.has_value()) {
    return Status(ReasonCode::RefusedInvalidDigest, "package digest is not canonical");
  }
  record.digest = *parsed_digest;
  auto models = field_array(value, "supported_models");
  SNCF_TRY(models);
  if (models.value()->size() > 256U) {
    return Status(ReasonCode::RefusedOversizedInput, "supported_models exceeds 256 entries");
  }
  std::uint16_t previous = 0;
  for (const auto& entry : *models.value()) {
    if (!entry.is_uint()) {
      return Status(ReasonCode::RefusedInvalidEnumValue, "supported model must be an integer");
    }
    const auto narrowed = narrow_checked<std::uint16_t>(entry.as_uint());
    if (!narrowed.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "supported model exceeds 16 bits");
    }
    if (*narrowed <= previous || *narrowed == 0U) {
      return Status(ReasonCode::RefusedNonCanonicalOrder, "supported_models must be sorted and unique");
    }
    previous = *narrowed;
    record.supported_models.push_back(DeviceModelId::from_value(*narrowed));
  }
  auto capabilities = field_string(value, "capabilities");
  SNCF_TRY(capabilities);
  const auto mask = CapabilityMask::from_hex(capabilities.value());
  if (!mask.has_value()) {
    return Status(ReasonCode::RefusedMalformedInput, "capabilities is not a mask");
  }
  record.required_capabilities = *mask;
  auto min_generation = field_uint(value, "min_capability_generation");
  SNCF_TRY(min_generation);
  record.min_capability_generation = CapabilityGeneration::from_value(min_generation.value());
  auto compat_min = field_uint(value, "compatibility_min");
  SNCF_TRY(compat_min);
  auto compat_max = field_uint(value, "compatibility_max");
  SNCF_TRY(compat_max);
  record.compatibility.min = CompatibilityGeneration::from_value(compat_min.value());
  record.compatibility.max = CompatibilityGeneration::from_value(compat_max.value());
  if (!record.compatibility.valid()) {
    return Status(ReasonCode::RefusedInvalidRange, "compatibility range is inverted");
  }
  auto min_firmware = field_value(value, "min_firmware");
  SNCF_TRY(min_firmware);
  auto firmware = semver_from_value(*min_firmware.value());
  SNCF_TRY(firmware);
  record.min_firmware = firmware.value();
  auto demand_ports = field_uint(value, "demand_ports");
  SNCF_TRY(demand_ports);
  auto demand_queues = field_uint(value, "demand_queues");
  SNCF_TRY(demand_queues);
  auto demand_memory = field_uint(value, "demand_memory_bytes");
  SNCF_TRY(demand_memory);
  const auto ports32 = narrow_checked<std::uint32_t>(demand_ports.value());
  const auto queues32 = narrow_checked<std::uint32_t>(demand_queues.value());
  if (!ports32.has_value() || !queues32.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "resource demand exceeds 32 bits");
  }
  record.demand.ports = *ports32;
  record.demand.queues = *queues32;
  record.demand.memory_bytes = demand_memory.value();
  auto exclusive = field_bool(value, "exclusive_scope");
  SNCF_TRY(exclusive);
  record.exclusive_scope = exclusive.value();
  auto layout = field_uint(value, "layout_revision");
  SNCF_TRY(layout);
  record.layout_revision = LayoutRevision::from_value(layout.value());
  auto registered = field_uint(value, "registered_at");
  SNCF_TRY(registered);
  record.registered_at = TimestampNs::from_value(registered.value());
  return record;
}

Value attempt_to_value(const AttemptRecord& record) {
  return Value::object({
      field_u64("deployment_generation", record.deployment_generation.value()),
      {"detail", Value::string(record.detail)},
      field_u64("dispatched_at", record.dispatched_at.value()),
      field_digest("effect_digest", record.effect_digest),
      {"effect_synthetic", Value::boolean(record.effect_synthetic)},
      field_u64("id", record.id.value()),
      field_u64("lease", record.lease.value()),
      {"outcome", Value::string(std::string(effect_outcome_name(record.outcome)))},
      field_u64("package", record.package.value()),
      {"phase", Value::string(std::string(attempt_phase_name(record.phase)))},
      {"reason", Value::string(std::string(reason_name(record.terminal_reason)))},
      field_u64("settled_at", record.settled_at.value()),
      field_u64("staged_at", record.staged_at.value()),
      field_u64("token", record.token.value()),
  });
}

Result<AttemptRecord> attempt_from_value(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "attempt record must be an object");
  }
  AttemptRecord record;
  auto id = field_uint(value, "id");
  SNCF_TRY(id);
  const auto id32 = narrow_checked<std::uint32_t>(id.value());
  if (!id32.has_value() || *id32 == 0U) {
    return Status(ReasonCode::RefusedNilIdentity, "attempt id must be a non-zero 32 bit value");
  }
  record.id = AttemptId::from_value(*id32);
  auto phase = field_string(value, "phase");
  SNCF_TRY(phase);
  record.phase = attempt_phase_from_name(phase.value());
  if (record.phase == AttemptPhase::Unknown && phase.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "attempt phase is not known");
  }
  auto generation = field_uint(value, "deployment_generation");
  SNCF_TRY(generation);
  record.deployment_generation = DeploymentGeneration::from_value(generation.value());
  auto token = field_uint(value, "token");
  SNCF_TRY(token);
  record.token = FencingToken::from_value(token.value());
  auto lease = field_uint(value, "lease");
  SNCF_TRY(lease);
  record.lease = LeaseId::from_value(lease.value());
  auto package = field_uint(value, "package");
  SNCF_TRY(package);
  record.package = FunctionPackageId::from_value(package.value());
  auto staged = field_uint(value, "staged_at");
  SNCF_TRY(staged);
  record.staged_at = TimestampNs::from_value(staged.value());
  auto dispatched = field_uint(value, "dispatched_at");
  SNCF_TRY(dispatched);
  record.dispatched_at = TimestampNs::from_value(dispatched.value());
  auto settled = field_uint(value, "settled_at");
  SNCF_TRY(settled);
  record.settled_at = TimestampNs::from_value(settled.value());
  auto outcome = field_string(value, "outcome");
  SNCF_TRY(outcome);
  record.outcome = effect_outcome_from_name(outcome.value());
  if (record.outcome == EffectOutcome::Unknown && outcome.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "effect outcome is not known");
  }
  auto digest = digest_from(value, "effect_digest");
  SNCF_TRY(digest);
  record.effect_digest = digest.value();
  auto synthetic = field_bool(value, "effect_synthetic");
  SNCF_TRY(synthetic);
  record.effect_synthetic = synthetic.value();
  auto reason = field_string(value, "reason");
  SNCF_TRY(reason);
  record.terminal_reason = reason_from_name(reason.value());
  if (record.terminal_reason == ReasonCode::Unknown && reason.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "attempt reason is not known");
  }
  auto detail = field_string(value, "detail");
  SNCF_TRY(detail);
  record.detail = detail.value();
  return record;
}

Value instance_to_value(const InstanceRecord& record) {
  Value::array_type attempts;
  attempts.reserve(record.attempts.size());
  for (const auto& attempt : record.attempts) {
    attempts.push_back(attempt_to_value(attempt));
  }
  return Value::object({
      {"authority_state", Value::string(std::string(authority_state_name(record.authority)))},
      field_u64("created_at", record.created_at.value()),
      field_u64("current_attempt", record.current_attempt.value()),
      field_u64("deployment_generation", record.deployment_generation.value()),
      {"desired", Value::string(std::string(desired_state_name(record.desired)))},
      field_u64("device", record.device.value()),
      field_u64("holder", record.holder.value()),
      field_u64("id", record.id.value()),
      field_u64("incarnation", record.incarnation.value()),
      field_digest("last_effect_digest", record.last_effect_digest),
      {"last_effect_outcome", Value::string(std::string(effect_outcome_name(record.last_effect_outcome)))},
      field_u64("last_settled_attempt", record.last_settled_attempt.value()),
      {"lifecycle", Value::string(std::string(lifecycle_state_name(record.lifecycle)))},
      {"lifecycle_freshness", Value::string(std::string(freshness_name(record.freshness)))},
      field_u64("package", record.package.value()),
      field_u64("previous_deployment_generation", record.previous_deployment_generation.value()),
      field_u64("previous_package", record.previous_package.value()),
      {"pending_intent_ambiguous", Value::boolean(record.pending_intent_ambiguous)},
      field_u64("policy_generation", record.policy_generation.value()),
      field_u64("port", record.port.value()),
      field_u64("queue", record.queue.value()),
      field_u64("reason", static_cast<std::uint64_t>(record.last_reason)),
      field_u64("smartnic", record.smartnic.value()),
      field_u64("token", record.token.value()),
      {"attempts", Value::array(std::move(attempts))},
      field_u64("updated_at", record.updated_at.value()),
      field_u64("lease", record.lease.value()),
  });
}

Result<InstanceRecord> instance_from_value(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "instance record must be an object");
  }
  InstanceRecord record;
  auto id = field_uint(value, "id");
  SNCF_TRY(id);
  record.id = FunctionInstanceId::from_value(id.value());
  if (record.id.is_nil()) {
    return Status(ReasonCode::RefusedNilIdentity, "instance id must be non-zero");
  }
  auto smartnic = field_uint(value, "smartnic");
  SNCF_TRY(smartnic);
  record.smartnic = SmartNicId::from_value(smartnic.value());
  auto device = field_uint(value, "device");
  SNCF_TRY(device);
  record.device = DeviceId::from_value(device.value());
  auto incarnation = field_uint(value, "incarnation");
  SNCF_TRY(incarnation);
  const auto incarnation32 = narrow_checked<std::uint32_t>(incarnation.value());
  if (!incarnation32.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "instance incarnation exceeds 32 bits");
  }
  record.incarnation = DeviceIncarnation::from_value(*incarnation32);
  auto package = field_uint(value, "package");
  SNCF_TRY(package);
  record.package = FunctionPackageId::from_value(package.value());
  auto port = field_uint(value, "port");
  SNCF_TRY(port);
  const auto port16 = narrow_checked<std::uint16_t>(port.value());
  if (!port16.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "instance port exceeds 16 bits");
  }
  record.port = PortId::from_value(*port16);
  auto queue = field_uint(value, "queue");
  SNCF_TRY(queue);
  const auto queue32 = narrow_checked<std::uint32_t>(queue.value());
  if (!queue32.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "instance queue exceeds 32 bits");
  }
  record.queue = QueueId::from_value(*queue32);

  auto desired = field_string(value, "desired");
  SNCF_TRY(desired);
  record.desired = desired_state_from_name(desired.value());
  if (record.desired == DesiredState::None && desired.value() != "none") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "desired state is not known");
  }
  auto lifecycle = field_string(value, "lifecycle");
  SNCF_TRY(lifecycle);
  record.lifecycle = lifecycle_state_from_name(lifecycle.value());
  if (record.lifecycle == LifecycleState::Unknown && lifecycle.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "lifecycle state is not known");
  }
  auto authority = field_string(value, "authority_state");
  SNCF_TRY(authority);
  record.authority = authority_state_from_name(authority.value());
  if (record.authority == AuthorityState::None && authority.value() != "none") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "authority state is not known");
  }
  auto freshness = field_string(value, "lifecycle_freshness");
  SNCF_TRY(freshness);
  record.freshness = freshness_from_name(freshness.value());
  if (record.freshness == Freshness::Unknown && freshness.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "instance freshness is not known");
  }

  auto generation = field_uint(value, "deployment_generation");
  SNCF_TRY(generation);
  record.deployment_generation = DeploymentGeneration::from_value(generation.value());
  auto policy = field_uint(value, "policy_generation");
  SNCF_TRY(policy);
  record.policy_generation = PolicyGeneration::from_value(policy.value());
  auto token = field_uint(value, "token");
  SNCF_TRY(token);
  record.token = FencingToken::from_value(token.value());
  auto lease = field_uint(value, "lease");
  SNCF_TRY(lease);
  record.lease = LeaseId::from_value(lease.value());
  auto holder = field_uint(value, "holder");
  SNCF_TRY(holder);
  record.holder = PrincipalId::from_value(holder.value());
  auto current = field_uint(value, "current_attempt");
  SNCF_TRY(current);
  const auto current32 = narrow_checked<std::uint32_t>(current.value());
  if (!current32.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "current_attempt exceeds 32 bits");
  }
  record.current_attempt = AttemptId::from_value(*current32);
  auto settled = field_uint(value, "last_settled_attempt");
  SNCF_TRY(settled);
  const auto settled32 = narrow_checked<std::uint32_t>(settled.value());
  if (!settled32.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "last_settled_attempt exceeds 32 bits");
  }
  record.last_settled_attempt = AttemptId::from_value(*settled32);

  auto attempts = field_array(value, "attempts");
  SNCF_TRY(attempts);
  if (attempts.value()->size() > 1024U) {
    return Status(ReasonCode::RefusedOversizedInput, "attempt history exceeds 1024 entries");
  }
  std::uint32_t previous_attempt = 0;
  for (const auto& entry : *attempts.value()) {
    auto attempt = attempt_from_value(entry);
    SNCF_TRY(attempt);
    if (attempt.value().id.value() <= previous_attempt) {
      return Status(ReasonCode::RefusedNonCanonicalOrder, "attempt history must be ordered and unique");
    }
    previous_attempt = attempt.value().id.value();
    record.attempts.push_back(std::move(attempt.value()));
  }

  auto previous_package = field_uint(value, "previous_package");
  SNCF_TRY(previous_package);
  record.previous_package = FunctionPackageId::from_value(previous_package.value());
  auto previous_generation = field_uint(value, "previous_deployment_generation");
  SNCF_TRY(previous_generation);
  record.previous_deployment_generation = DeploymentGeneration::from_value(previous_generation.value());
  auto ambiguous = field_bool(value, "pending_intent_ambiguous");
  SNCF_TRY(ambiguous);
  record.pending_intent_ambiguous = ambiguous.value();
  auto reason = field_uint(value, "reason");
  SNCF_TRY(reason);
  const auto reason16 = narrow_checked<std::uint16_t>(reason.value());
  if (!reason16.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "reason exceeds 16 bits");
  }
  record.last_reason = static_cast<ReasonCode>(*reason16);
  auto last_digest = digest_from(value, "last_effect_digest");
  SNCF_TRY(last_digest);
  record.last_effect_digest = last_digest.value();
  auto outcome = field_string(value, "last_effect_outcome");
  SNCF_TRY(outcome);
  record.last_effect_outcome = effect_outcome_from_name(outcome.value());
  if (record.last_effect_outcome == EffectOutcome::Unknown && outcome.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "last effect outcome is not known");
  }
  auto created = field_uint(value, "created_at");
  SNCF_TRY(created);
  record.created_at = TimestampNs::from_value(created.value());
  auto updated = field_uint(value, "updated_at");
  SNCF_TRY(updated);
  record.updated_at = TimestampNs::from_value(updated.value());
  return record;
}

Value capability_evidence_to_value(const CapabilityEvidenceRecord& record) {
  const auto& evidence = record.evidence;
  return Value::object({
      {"capabilities", Value::string(evidence.capabilities.to_hex())},
      field_u64("capability_generation", evidence.capability_generation.value()),
      field_u64("compatibility_generation", evidence.compatibility_generation.value()),
      field_u64("device", evidence.device.value()),
      {"envelope", envelope_to_value(evidence.envelope)},
      {"firmware", semver_to_value(evidence.firmware)},
      field_u64("incarnation", evidence.incarnation.value()),
      field_u64("model", evidence.model.value()),
      field_u64("sequence", record.sequence.value()),
      field_u64("smartnic", evidence.smartnic.value()),
  });
}

Result<CapabilityEvidenceRecord> capability_evidence_from_value(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "capability evidence must be an object");
  }
  CapabilityEvidenceRecord record;
  auto device = field_uint(value, "device");
  SNCF_TRY(device);
  record.evidence.device = DeviceId::from_value(device.value());
  if (record.evidence.device.is_nil()) {
    return Status(ReasonCode::RefusedNilIdentity, "capability evidence device must be non-zero");
  }
  auto smartnic = field_uint(value, "smartnic");
  SNCF_TRY(smartnic);
  record.evidence.smartnic = SmartNicId::from_value(smartnic.value());
  auto incarnation = field_uint(value, "incarnation");
  SNCF_TRY(incarnation);
  const auto incarnation32 = narrow_checked<std::uint32_t>(incarnation.value());
  if (!incarnation32.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "evidence incarnation exceeds 32 bits");
  }
  record.evidence.incarnation = DeviceIncarnation::from_value(*incarnation32);
  auto model = field_uint(value, "model");
  SNCF_TRY(model);
  const auto model16 = narrow_checked<std::uint16_t>(model.value());
  if (!model16.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "evidence model exceeds 16 bits");
  }
  record.evidence.model = DeviceModelId::from_value(*model16);
  auto capability_generation = field_uint(value, "capability_generation");
  SNCF_TRY(capability_generation);
  record.evidence.capability_generation = CapabilityGeneration::from_value(capability_generation.value());
  auto compatibility_generation = field_uint(value, "compatibility_generation");
  SNCF_TRY(compatibility_generation);
  record.evidence.compatibility_generation = CompatibilityGeneration::from_value(compatibility_generation.value());
  auto firmware = field_value(value, "firmware");
  SNCF_TRY(firmware);
  auto semver = semver_from_value(*firmware.value());
  SNCF_TRY(semver);
  record.evidence.firmware = semver.value();
  auto capabilities = field_string(value, "capabilities");
  SNCF_TRY(capabilities);
  const auto mask = CapabilityMask::from_hex(capabilities.value());
  if (!mask.has_value()) {
    return Status(ReasonCode::RefusedMalformedInput, "evidence capabilities is not a mask");
  }
  record.evidence.capabilities = *mask;
  auto envelope = field_value(value, "envelope");
  SNCF_TRY(envelope);
  auto decoded = envelope_from(*envelope.value());
  SNCF_TRY(decoded);
  record.evidence.envelope = decoded.value();
  auto sequence = field_uint(value, "sequence");
  SNCF_TRY(sequence);
  record.sequence = RecordSequence::from_value(sequence.value());
  return record;
}

Value observation_to_value(const ObservationRecord& record) {
  const auto& evidence = record.evidence;
  return Value::object({
      field_u64("capability_generation", evidence.capability_generation.value()),
      field_u64("compatibility_generation", evidence.compatibility_generation.value()),
      field_u64("device", evidence.device.value()),
      {"envelope", envelope_to_value(evidence.envelope)},
      field_u64("incarnation", evidence.incarnation.value()),
      {"present", Value::boolean(evidence.present)},
      field_u64("sequence", record.sequence.value()),
      field_u64("smartnic", evidence.smartnic.value()),
  });
}

Result<ObservationRecord> observation_from_value(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "observation must be an object");
  }
  ObservationRecord record;
  auto device = field_uint(value, "device");
  SNCF_TRY(device);
  record.evidence.device = DeviceId::from_value(device.value());
  if (record.evidence.device.is_nil()) {
    return Status(ReasonCode::RefusedNilIdentity, "observation device must be non-zero");
  }
  auto smartnic = field_uint(value, "smartnic");
  SNCF_TRY(smartnic);
  record.evidence.smartnic = SmartNicId::from_value(smartnic.value());
  auto incarnation = field_uint(value, "incarnation");
  SNCF_TRY(incarnation);
  const auto incarnation32 = narrow_checked<std::uint32_t>(incarnation.value());
  if (!incarnation32.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "observation incarnation exceeds 32 bits");
  }
  record.evidence.incarnation = DeviceIncarnation::from_value(*incarnation32);
  auto present = field_bool(value, "present");
  SNCF_TRY(present);
  record.evidence.present = present.value();
  auto capability_generation = field_uint(value, "capability_generation");
  SNCF_TRY(capability_generation);
  record.evidence.capability_generation = CapabilityGeneration::from_value(capability_generation.value());
  auto compatibility_generation = field_uint(value, "compatibility_generation");
  SNCF_TRY(compatibility_generation);
  record.evidence.compatibility_generation = CompatibilityGeneration::from_value(compatibility_generation.value());
  auto envelope = field_value(value, "envelope");
  SNCF_TRY(envelope);
  auto decoded = envelope_from(*envelope.value());
  SNCF_TRY(decoded);
  record.evidence.envelope = decoded.value();
  auto sequence = field_uint(value, "sequence");
  SNCF_TRY(sequence);
  record.sequence = RecordSequence::from_value(sequence.value());
  return record;
}

Value lease_to_value(const Lease& lease) {
  return Value::object({
      field_u64("device", lease.scope.device.value()),
      field_u64("epoch", lease.epoch.value()),
      field_u64("expires_at", lease.expires_at.value()),
      field_u64("granted_at", lease.granted_at.value()),
      field_u64("holder", lease.holder.value()),
      field_u64("id", lease.id.value()),
      field_u64("instance", lease.instance.value()),
      field_u64("port", lease.scope.port.value()),
      {"revoked", Value::boolean(lease.revoked)},
      field_u64("smartnic", lease.scope.smartnic.value()),
      field_u64("token", lease.token.value()),
  });
}

Result<Lease> lease_from_value(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "lease must be an object");
  }
  Lease lease;
  auto id = field_uint(value, "id");
  SNCF_TRY(id);
  lease.id = LeaseId::from_value(id.value());
  if (lease.id.is_nil()) {
    return Status(ReasonCode::RefusedNilIdentity, "lease id must be non-zero");
  }
  auto smartnic = field_uint(value, "smartnic");
  SNCF_TRY(smartnic);
  lease.scope.smartnic = SmartNicId::from_value(smartnic.value());
  auto device = field_uint(value, "device");
  SNCF_TRY(device);
  lease.scope.device = DeviceId::from_value(device.value());
  auto port = field_uint(value, "port");
  SNCF_TRY(port);
  const auto port16 = narrow_checked<std::uint16_t>(port.value());
  if (!port16.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "lease port exceeds 16 bits");
  }
  lease.scope.port = PortId::from_value(*port16);
  auto instance = field_uint(value, "instance");
  SNCF_TRY(instance);
  lease.instance = FunctionInstanceId::from_value(instance.value());
  auto holder = field_uint(value, "holder");
  SNCF_TRY(holder);
  lease.holder = PrincipalId::from_value(holder.value());
  auto epoch = field_uint(value, "epoch");
  SNCF_TRY(epoch);
  lease.epoch = CoordinatorEpoch::from_value(epoch.value());
  auto token = field_uint(value, "token");
  SNCF_TRY(token);
  lease.token = FencingToken::from_value(token.value());
  auto granted = field_uint(value, "granted_at");
  SNCF_TRY(granted);
  lease.granted_at = TimestampNs::from_value(granted.value());
  auto expires = field_uint(value, "expires_at");
  SNCF_TRY(expires);
  lease.expires_at = TimestampNs::from_value(expires.value());
  auto revoked = field_bool(value, "revoked");
  SNCF_TRY(revoked);
  lease.revoked = revoked.value();
  return lease;
}

namespace {

Value dedupe_to_value(const DedupeEntry& entry) {
  return Value::object({
      field_u64("command", entry.command.value()),
      {"outcome", Value::string(std::string(reason_name(entry.outcome)))},
      field_u64("principal", entry.principal.value()),
      field_u64("sequence", entry.sequence.value()),
      {"subject", Value::string(entry.subject)},
  });
}

Result<DedupeEntry> dedupe_from_value(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "dedupe entry must be an object");
  }
  DedupeEntry entry;
  auto command = field_uint(value, "command");
  SNCF_TRY(command);
  entry.command = CommandId::from_value(command.value());
  auto principal = field_uint(value, "principal");
  SNCF_TRY(principal);
  entry.principal = PrincipalId::from_value(principal.value());
  auto outcome = field_string(value, "outcome");
  SNCF_TRY(outcome);
  entry.outcome = reason_from_name(outcome.value());
  if (entry.outcome == ReasonCode::Unknown && outcome.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "dedupe outcome is not known");
  }
  auto sequence = field_uint(value, "sequence");
  SNCF_TRY(sequence);
  entry.sequence = RecordSequence::from_value(sequence.value());
  auto subject = field_string(value, "subject");
  SNCF_TRY(subject);
  entry.subject = subject.value();
  return entry;
}

Value event_to_value(const FabricEvent& event) {
  return Value::object({
      field_u64("at", event.at.value()),
      {"detail", Value::string(event.detail)},
      {"reason", Value::string(std::string(reason_name(event.reason)))},
      field_u64("sequence", event.sequence.value()),
      {"subject", Value::string(event.subject)},
  });
}

Result<FabricEvent> event_from_value(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "event must be an object");
  }
  FabricEvent event;
  auto sequence = field_uint(value, "sequence");
  SNCF_TRY(sequence);
  event.sequence = EventSequence::from_value(sequence.value());
  auto at = field_uint(value, "at");
  SNCF_TRY(at);
  event.at = TimestampNs::from_value(at.value());
  auto reason = field_string(value, "reason");
  SNCF_TRY(reason);
  event.reason = reason_from_name(reason.value());
  if (event.reason == ReasonCode::Unknown && reason.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "event reason is not known");
  }
  auto subject = field_string(value, "subject");
  SNCF_TRY(subject);
  event.subject = subject.value();
  auto detail = field_string(value, "detail");
  SNCF_TRY(detail);
  event.detail = detail.value();
  return event;
}

template <class Record, class FromValue>
Result<void> decode_sorted_array(const Value& root, std::string_view key, std::size_t max_items,
                                 std::vector<Record>& out, FromValue from_value) noexcept {
  auto array = field_array(root, key);
  SNCF_TRY(array);
  if (array.value()->size() > max_items) {
    return Status(ReasonCode::RefusedOversizedInput, std::string(key) + " exceeds the configured bound");
  }
  out.clear();
  out.reserve(array.value()->size());
  for (const auto& entry : *array.value()) {
    auto decoded = from_value(entry);
    SNCF_TRY(decoded);
    out.push_back(std::move(decoded.value()));
  }
  return {};
}

template <class Record, class KeyFn>
Result<void> require_sorted(const std::vector<Record>& items, KeyFn key, std::string_view name) noexcept {
  for (std::size_t i = 1; i < items.size(); ++i) {
    if (!(key(items[i - 1]) < key(items[i]))) {
      return Status(ReasonCode::RefusedNonCanonicalOrder, std::string(name) + " must be sorted and unique");
    }
  }
  return {};
}

}  // namespace

Value state_to_value(const FabricState& state) {
  Value::array_type smartnics;
  smartnics.reserve(state.smartnics.size());
  for (const auto& record : state.smartnics) {
    smartnics.push_back(smartnic_to_value(record));
  }
  Value::array_type devices;
  devices.reserve(state.devices.size());
  for (const auto& record : state.devices) {
    devices.push_back(device_to_value(record));
  }
  Value::array_type packages;
  packages.reserve(state.packages.size());
  for (const auto& record : state.packages) {
    packages.push_back(package_to_value(record));
  }
  Value::array_type instances;
  instances.reserve(state.instances.size());
  for (const auto& record : state.instances) {
    instances.push_back(instance_to_value(record));
  }
  Value::array_type capability_evidence;
  capability_evidence.reserve(state.capability_evidence.size());
  for (const auto& record : state.capability_evidence) {
    capability_evidence.push_back(capability_evidence_to_value(record));
  }
  Value::array_type observations;
  observations.reserve(state.observations.size());
  for (const auto& record : state.observations) {
    observations.push_back(observation_to_value(record));
  }
  Value::array_type leases;
  leases.reserve(state.leases.size());
  for (const auto& lease : state.leases) {
    leases.push_back(lease_to_value(lease));
  }
  Value::array_type dedupe;
  dedupe.reserve(state.dedupe.size());
  for (const auto& entry : state.dedupe) {
    dedupe.push_back(dedupe_to_value(entry));
  }
  Value::array_type dedupe_floor;
  dedupe_floor.reserve(state.dedupe_floor.size());
  for (const auto& entry : state.dedupe_floor) {
    dedupe_floor.push_back(Value::object({
        {"command", Value::uint_value(entry.second.value())},
        {"principal", Value::uint_value(entry.first.value())},
    }));
  }
  Value::array_type history;
  history.reserve(state.history.size());
  for (const auto& event : state.history) {
    history.push_back(event_to_value(event));
  }
  Value::array_type fencing;
  fencing.reserve(state.fencing.entries().size());
  for (const auto& entry : state.fencing.entries()) {
    fencing.push_back(Value::object({
        field_u64("device", entry.first.device.value()),
        field_u64("port", entry.first.port.value()),
        field_u64("smartnic", entry.first.smartnic.value()),
        field_u64("token", entry.second.value()),
    }));
  }

  return Value::object({
      {"boot", Value::uint_value(state.boot.value())},
      {"capability_evidence", Value::array(std::move(capability_evidence))},
      {"dedupe", Value::array(std::move(dedupe))},
      {"dedupe_floor", Value::array(std::move(dedupe_floor))},
      {"devices", Value::array(std::move(devices))},
      {"epoch", Value::uint_value(state.epoch.value())},
      {"fencing", Value::array(std::move(fencing))},
      {"history", Value::array(std::move(history))},
      {"instances", Value::array(std::move(instances))},
      {"last_sequence", Value::uint_value(state.last_sequence.value())},
      {"leases", Value::array(std::move(leases))},
      {"next_device", Value::uint_value(state.next_device)},
      {"next_evidence", Value::uint_value(state.next_evidence)},
      {"next_instance", Value::uint_value(state.next_instance)},
      {"next_lease", Value::uint_value(state.next_lease)},
      {"next_package", Value::uint_value(state.next_package)},
      {"next_smartnic", Value::uint_value(state.next_smartnic)},
      {"observations", Value::array(std::move(observations))},
      {"packages", Value::array(std::move(packages))},
      {"policy_generation", Value::uint_value(state.policy_generation.value())},
      {"schema", Value::uint_value(kExportSchemaVersion)},
      {"semantics", Value::string(std::string(semantics_id()))},
      {"smartnics", Value::array(std::move(smartnics))},
      {"version", Value::string(std::string(version_string()))},
  });
}

Result<void> state_from_value(const Value& value, FabricState& state, const FabricConfig& config) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "state document must be an object");
  }
  auto schema = field_uint(value, "schema");
  SNCF_TRY(schema);
  if (schema.value() != kExportSchemaVersion) {
    return Status(ReasonCode::RefusedIncompatibleStoreVersion, "state schema version is not supported");
  }
  auto semantics = field_string(value, "semantics");
  SNCF_TRY(semantics);
  if (semantics.value() != semantics_id()) {
    return Status(ReasonCode::RefusedSemanticsMismatch,
                  "state was written under different semantics and cannot be reinterpreted");
  }

  FabricState decoded;
  auto epoch = field_uint(value, "epoch");
  SNCF_TRY(epoch);
  decoded.epoch = CoordinatorEpoch::from_value(epoch.value());
  auto boot = field_uint(value, "boot");
  SNCF_TRY(boot);
  decoded.boot = BootId::from_value(boot.value());
  auto policy = field_uint(value, "policy_generation");
  SNCF_TRY(policy);
  decoded.policy_generation = PolicyGeneration::from_value(policy.value());
  auto sequence = field_uint(value, "last_sequence");
  SNCF_TRY(sequence);
  decoded.last_sequence = RecordSequence::from_value(sequence.value());

  auto next_smartnic = field_uint(value, "next_smartnic");
  SNCF_TRY(next_smartnic);
  decoded.next_smartnic = next_smartnic.value();
  auto next_device = field_uint(value, "next_device");
  SNCF_TRY(next_device);
  decoded.next_device = next_device.value();
  auto next_package = field_uint(value, "next_package");
  SNCF_TRY(next_package);
  decoded.next_package = next_package.value();
  auto next_instance = field_uint(value, "next_instance");
  SNCF_TRY(next_instance);
  decoded.next_instance = next_instance.value();
  auto next_lease = field_uint(value, "next_lease");
  SNCF_TRY(next_lease);
  decoded.next_lease = next_lease.value();
  auto next_evidence = field_uint(value, "next_evidence");
  SNCF_TRY(next_evidence);
  decoded.next_evidence = next_evidence.value();
  if (decoded.next_smartnic == 0U || decoded.next_device == 0U || decoded.next_package == 0U ||
      decoded.next_instance == 0U || decoded.next_lease == 0U || decoded.next_evidence == 0U) {
    return Status(ReasonCode::RefusedNilIdentity, "allocator high-water marks must be non-zero");
  }

  SNCF_TRY(decode_sorted_array(value, "smartnics", config.max_smartnics, decoded.smartnics,
                               smartnic_from_value));
  SNCF_TRY(decode_sorted_array(value, "devices", config.max_devices, decoded.devices, device_from_value));
  SNCF_TRY(decode_sorted_array(value, "packages", config.max_packages, decoded.packages, package_from_value));
  SNCF_TRY(decode_sorted_array(value, "instances", config.max_instances, decoded.instances,
                               instance_from_value));
  SNCF_TRY(decode_sorted_array(value, "capability_evidence", config.max_devices,
                               decoded.capability_evidence, capability_evidence_from_value));
  SNCF_TRY(decode_sorted_array(value, "observations", config.max_devices, decoded.observations,
                               observation_from_value));
  SNCF_TRY(decode_sorted_array(value, "leases", config.max_leases, decoded.leases, lease_from_value));
  SNCF_TRY(decode_sorted_array(value, "dedupe", config.max_dedupe_entries, decoded.dedupe,
                               dedupe_from_value));
  SNCF_TRY(decode_sorted_array(value, "history", config.max_history, decoded.history, event_from_value));

  SNCF_TRY(require_sorted(decoded.smartnics, [](const SmartNicRecord& r) { return r.id; }, "smartnics"));
  SNCF_TRY(require_sorted(decoded.devices, [](const DeviceRecord& r) { return r.id; }, "devices"));
  SNCF_TRY(require_sorted(decoded.packages, [](const PackageRecord& r) { return r.id; }, "packages"));
  SNCF_TRY(require_sorted(decoded.instances, [](const InstanceRecord& r) { return r.id; }, "instances"));
  SNCF_TRY(require_sorted(decoded.capability_evidence,
                          [](const CapabilityEvidenceRecord& r) { return r.evidence.device; },
                          "capability_evidence"));
  SNCF_TRY(require_sorted(decoded.observations, [](const ObservationRecord& r) { return r.evidence.device; },
                          "observations"));
  SNCF_TRY(require_sorted(decoded.leases, [](const Lease& l) { return l.id; }, "leases"));

  auto floor_array = field_array(value, "dedupe_floor");
  SNCF_TRY(floor_array);
  if (floor_array.value()->size() > config.max_dedupe_entries) {
    return Status(ReasonCode::RefusedOversizedInput, "dedupe_floor exceeds the configured bound");
  }
  for (const auto& entry : *floor_array.value()) {
    if (!entry.is_object()) {
      return Status(ReasonCode::RefusedMalformedInput, "a dedupe floor entry must be an object");
    }
    auto principal = field_uint(entry, "principal");
    SNCF_TRY(principal);
    auto command = field_uint(entry, "command");
    SNCF_TRY(command);
    decoded.dedupe_floor.emplace_back(PrincipalId::from_value(principal.value()),
                                      CommandId::from_value(command.value()));
  }
  std::sort(decoded.dedupe_floor.begin(), decoded.dedupe_floor.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (std::size_t index = 1; index < decoded.dedupe_floor.size(); ++index) {
    if (!(decoded.dedupe_floor[index - 1].first < decoded.dedupe_floor[index].first)) {
      return Status(ReasonCode::RefusedNonCanonicalOrder, "dedupe_floor must be sorted and unique");
    }
  }

  auto fencing = field_array(value, "fencing");
  SNCF_TRY(fencing);
  if (fencing.value()->size() > config.max_leases * 4U + 64U) {
    return Status(ReasonCode::RefusedOversizedInput, "fencing ledger exceeds the configured bound");
  }
  for (const auto& entry : *fencing.value()) {
    if (!entry.is_object()) {
      return Status(ReasonCode::RefusedMalformedInput, "fencing entry must be an object");
    }
    auto smartnic = field_uint(entry, "smartnic");
    SNCF_TRY(smartnic);
    auto device = field_uint(entry, "device");
    SNCF_TRY(device);
    auto port = field_uint(entry, "port");
    SNCF_TRY(port);
    auto token = field_uint(entry, "token");
    SNCF_TRY(token);
    const auto port16 = narrow_checked<std::uint16_t>(port.value());
    if (!port16.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "fencing port exceeds 16 bits");
    }
    ExclusiveScope scope{SmartNicId::from_value(smartnic.value()), DeviceId::from_value(device.value()),
                         PortId::from_value(*port16)};
    if (!scope.valid()) {
      return Status(ReasonCode::RefusedNilIdentity, "fencing entry scope must be valid");
    }
    if (token.value() == 0U) {
      return Status(ReasonCode::RefusedNilIdentity, "fencing token must be non-zero");
    }
    decoded.fencing.observe(scope, FencingToken::from_value(token.value()));
  }

  state = std::move(decoded);
  return {};
}

}  // namespace sncf
