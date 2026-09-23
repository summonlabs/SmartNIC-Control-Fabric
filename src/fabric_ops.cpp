// Operation decisions. Every mutating entry point follows the same shape:
// dedupe, decide, journal, apply. Nothing mutates state before the decision is
// durable, so a refusal can never leave a partial effect.
// Copyright 2026 Summon Software Labs.
#include <algorithm>

#include "fabric_impl.hpp"

namespace sncf {
namespace {

constexpr std::uint16_t kMaxPortCount = 4096;

template <class Id>
Id id_from_subject(std::string_view subject, std::string_view prefix) noexcept {
  if (!subject_has_prefix(subject, prefix)) {
    return Id{};
  }
  const auto value = parse_subject_id(subject);
  if (!value.has_value()) {
    return Id{};
  }
  const auto narrowed = narrow_checked<typename Id::rep_type>(*value);
  if (!narrowed.has_value()) {
    return Id{};
  }
  return Id::from_value(*narrowed);
}

bool is_trusted_capability_source(EvidenceSource source) noexcept {
  return source == EvidenceSource::CapabilityEvidenceProvider || source == EvidenceSource::Operator ||
         source == EvidenceSource::SyntheticFixture;
}

bool is_trusted_observation_source(EvidenceSource source) noexcept {
  return source == EvidenceSource::DeviceObservationRuntime || source == EvidenceSource::Operator ||
         source == EvidenceSource::SyntheticFixture;
}

bool is_trusted_execution_source(EvidenceSource source) noexcept {
  return source == EvidenceSource::ExecutionRuntime || source == EvidenceSource::SyntheticFixture;
}

bool models_contain(const std::vector<DeviceModelId>& models, DeviceModelId model) noexcept {
  return std::binary_search(models.begin(), models.end(), model);
}

Value::array_type fencing_entries(const std::vector<ExclusiveScope>& scopes, const FencingLedger& ledger) {
  Value::array_type entries;
  entries.reserve(scopes.size());
  for (const auto& scope : scopes) {
    entries.push_back(Value::object({
        {"device", Value::uint_value(scope.device.value())},
        {"port", Value::uint_value(scope.port.value())},
        {"smartnic", Value::uint_value(scope.smartnic.value())},
        {"token", Value::uint_value(ledger.high_water(scope).value())},
    }));
  }
  return entries;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared checks
// ---------------------------------------------------------------------------

Result<void> Fabric::Impl::check_authority(const AuthorityClaim& claim, const InstanceRecord& instance,
                                           Decision& decision, bool require_authority) const {
  if (!require_authority) {
    return {};
  }
  if (!claim.lease.valid()) {
    decision.refuse(ReasonCode::RefusedNoAuthority, "no lease was presented");
    return Status(decision.reason, decision.detail);
  }
  const Lease* lease = state.find_lease(claim.lease);
  if (lease == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownLease, "the presented lease is not known");
    return Status(decision.reason, decision.detail);
  }
  const ExclusiveScope instance_scope{instance.smartnic, instance.device, instance.port};
  if (!(lease->scope == claim.scope) || !(claim.scope == instance_scope)) {
    decision.refuse(ReasonCode::RefusedLeaseScopeMismatch, "the lease does not cover this instance scope");
    return Status(decision.reason, decision.detail);
  }
  if (lease->instance != instance.id) {
    decision.refuse(ReasonCode::RefusedNoAuthority, "the lease belongs to a different instance");
    return Status(decision.reason, decision.detail);
  }
  if (claim.principal.valid() && lease->holder != claim.principal) {
    decision.refuse(ReasonCode::RefusedPermissionDenied, "the lease is held by a different principal");
    return Status(decision.reason, decision.detail);
  }
  if (lease->revoked) {
    decision.refuse(ReasonCode::RefusedAuthorityRevoked, "the lease was revoked");
    return Status(decision.reason, decision.detail);
  }
  const TimestampNs at = now();
  if (at.value() >= lease->expires_at.value()) {
    decision.refuse(ReasonCode::RefusedLeaseExpired, "the lease expired before the operation");
    return Status(decision.reason, decision.detail);
  }
  if (claim.epoch != state.epoch || lease->epoch != state.epoch) {
    decision.refuse(ReasonCode::RefusedStaleEpoch, "the authority was issued under an older coordinator epoch");
    return Status(decision.reason, decision.detail);
  }
  const FencingToken high = state.fencing.high_water(instance_scope);
  if (claim.token.value() < high.value() || claim.token != lease->token) {
    decision.refuse(ReasonCode::RefusedStaleFencingToken,
                    "a newer fencing token exists for this scope");
    return Status(decision.reason, decision.detail);
  }
  decision.note_with_generation(ReasonCode::Accepted, subject_of(instance.id), "authority accepted",
                                claim.token.value());
  return {};
}

bool Fabric::Impl::prune_leases(Decision& decision) const {
  if (state.leases.size() < options.config.max_leases) {
    return true;
  }
  const TimestampNs at = now();
  Value::array_type removals;
  std::vector<LeaseId> candidates;
  for (const auto& lease : state.leases) {
    if (lease.revoked || at.value() >= lease.expires_at.value()) {
      candidates.push_back(lease.id);
    }
  }
  std::sort(candidates.begin(), candidates.end());
  const std::size_t live = state.leases.size() - candidates.size();
  if (live >= options.config.max_leases) {
    return false;
  }
  const std::size_t removable = state.leases.size() - options.config.max_leases + 1U;
  for (std::size_t index = 0; index < removable && index < candidates.size(); ++index) {
    removals.push_back(Value::uint_value(candidates[index].value()));
    ++decision.lease_evictions;
  }
  if (removals.empty()) {
    return state.leases.size() < options.config.max_leases;
  }
  decision.body.emplace_back("leases_removed", Value::array(std::move(removals)));
  return true;
}

const CapabilityEvidenceRecord* Fabric::Impl::live_capability_evidence(const DeviceRecord& device,
                                                                       Decision& decision) const {
  const CapabilityEvidenceRecord* evidence = state.find_capability_evidence(device.id);
  if (evidence == nullptr || !evidence->evidence.envelope.id.valid()) {
    decision.refuse(ReasonCode::RefusedCapabilityEvidenceAbsent,
                    "no capability evidence has been accepted for this device");
    return nullptr;
  }
  if (evidence->evidence.incarnation != device.incarnation) {
    decision.refuse(ReasonCode::RefusedStaleDeviceIncarnation,
                    "the stored capability evidence describes a different device incarnation");
    return nullptr;
  }
  switch (evidence->evidence.envelope.freshness) {
    case Freshness::Fresh:
      break;
    case Freshness::PendingReverification:
      decision.refuse(ReasonCode::RefusedEvidenceNotReverified,
                      "the evidence survived a restart and has not been revalidated under this epoch");
      return nullptr;
    case Freshness::Expired:
      decision.refuse(ReasonCode::RefusedExpiredEvidence, "the capability evidence has expired");
      return nullptr;
    case Freshness::Stale:
      decision.refuse(ReasonCode::RefusedStaleEvidence, "the capability evidence was superseded");
      return nullptr;
    case Freshness::Unknown:
      decision.refuse(ReasonCode::RefusedUnknownFreshness, "the freshness of the evidence is unknown");
      return nullptr;
  }
  return evidence;
}

Result<void> Fabric::Impl::check_compatibility(const InstanceRecord& instance, const PackageRecord& package,
                                               Decision& decision) const {
  const DeviceRecord* device = state.find_device(instance.device);
  if (device == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownDevice, "the target device is not registered");
    return Status(decision.reason, decision.detail);
  }
  if (device->incarnation != instance.incarnation) {
    decision.refuse(ReasonCode::RefusedDeviceIncarnationMismatch,
                    "the device incarnation no longer matches the instance binding");
    return Status(decision.reason, decision.detail);
  }
  // Capability evidence is the more fundamental gate, so it is reported first;
  // both are refusals, and neither is ever coerced into a default.
  const CapabilityEvidenceRecord* evidence = live_capability_evidence(*device, decision);
  if (evidence == nullptr) {
    return Status(decision.reason, decision.detail);
  }
  // Liveness is evidence, not a durable fact: it must be within the observation
  // window at the moment of the decision, which is what keeps a removed or
  // vanished device from being activated long after it was last seen.
  const ObservationRecord* observation = state.find_observation(device->id);
  if (observation == nullptr) {
    decision.refuse(ReasonCode::RefusedEvidenceSourceUnavailable,
                    "no device observation has been accepted for this device");
    return Status(decision.reason, decision.detail);
  }
  if (observation->evidence.incarnation != device->incarnation) {
    decision.refuse(ReasonCode::RefusedStaleDeviceIncarnation,
                    "the last observation describes a different device incarnation");
    return Status(decision.reason, decision.detail);
  }
  if (!observation->evidence.present || !device->present) {
    decision.refuse(ReasonCode::RefusedEvidenceSourceUnavailable,
                    "the device is not currently reported present");
    return Status(decision.reason, decision.detail);
  }
  auto observation_freshness = evaluate_freshness(
      observation->evidence.envelope.observed_at, now(), options.config.freshness.observation_max_age);
  if (!observation_freshness.ok()) {
    decision.refuse(observation_freshness.code(), observation_freshness.status().detail());
    return Status(decision.reason, decision.detail);
  }
  if (observation_freshness.value() != Freshness::Fresh) {
    decision.refuse(ReasonCode::RefusedExpiredEvidence,
                    "the last device observation is outside the freshness window");
    return Status(decision.reason, decision.detail);
  }
  const Digest256 digest = evidence->evidence.envelope.payload_digest;
  if (evidence->evidence.capability_generation.value() < package.min_capability_generation.value()) {
    decision.note_with_generation(ReasonCode::RefusedIncompatibleCapabilityGeneration, subject_of(device->id),
                                  "device capability generation is below the package requirement",
                                  evidence->evidence.capability_generation.value());
    decision.refuse(ReasonCode::RefusedIncompatibleCapabilityGeneration,
                    "device capability generation is below the package requirement");
    return Status(decision.reason, decision.detail);
  }
  if (!models_contain(package.supported_models, device->model)) {
    decision.refuse(ReasonCode::RefusedUnsupportedDeviceModel, "the package does not support this device model");
    return Status(decision.reason, decision.detail);
  }
  if (!evidence->evidence.capabilities.contains_all(package.required_capabilities)) {
    decision.note_with_evidence(ReasonCode::RefusedUnsupportedRequiredCapability, subject_of(device->id),
                                "the device does not advertise a capability the package requires", digest);
    decision.refuse(ReasonCode::RefusedUnsupportedRequiredCapability,
                    "the device does not advertise a capability the package requires");
    return Status(decision.reason, decision.detail);
  }
  if (!package.compatibility.contains(evidence->evidence.compatibility_generation)) {
    decision.note_with_generation(ReasonCode::RefusedIncompatibleCompatibilityGeneration,
                                  subject_of(device->id),
                                  "firmware/runtime compatibility generation is outside the package range",
                                  evidence->evidence.compatibility_generation.value());
    decision.refuse(ReasonCode::RefusedIncompatibleCompatibilityGeneration,
                    "firmware/runtime compatibility generation is outside the package range");
    return Status(decision.reason, decision.detail);
  }
  if (evidence->evidence.firmware < package.min_firmware) {
    decision.refuse(ReasonCode::RefusedIncompatibleFirmware, "observed firmware is below the package minimum");
    return Status(decision.reason, decision.detail);
  }
  decision.note_with_evidence(ReasonCode::Accepted, subject_of(device->id),
                              "compatibility evidence accepted", digest);
  return {};
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

Decision Fabric::Impl::decide_register_smartnic(const RegisterSmartNicRequest& request) const {
  Decision decision;
  decision.subject = "smartnic:new";
  if (!request.header.command.valid()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "command id must be non-zero");
    return decision;
  }
  if (!request.header.principal.valid()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "principal id must be non-zero");
    return decision;
  }
  auto label = validate_label(request.label);
  if (!label.ok()) {
    decision.refuse(label.code(), label.status().detail());
    return decision;
  }
  if (request.port_count == 0U || request.port_count > kMaxPortCount) {
    decision.refuse(ReasonCode::RefusedInvalidRange, "port_count must be between 1 and 4096");
    return decision;
  }
  if (state.smartnics.size() >= options.config.max_smartnics) {
    decision.refuse(ReasonCode::RefusedCapacityExceeded, "the SmartNIC registry is full");
    return decision;
  }
  std::uint64_t next = 0;
  if (!add_checked<std::uint64_t>(state.next_smartnic, 1U, next)) {
    decision.refuse(ReasonCode::RefusedArithmeticOverflow, "SmartNIC identity space is exhausted");
    return decision;
  }
  const SmartNicId id = SmartNicId::from_value(state.next_smartnic);
  SmartNicRecord record;
  record.id = id;
  record.label = label.value();
  record.port_count = request.port_count;
  record.registered_at = now();

  decision.kind = RecordKind::SmartNicRegistered;
  decision.override_allocator("next_smartnic", next);
  decision.body.emplace_back("smartnic", smartnic_to_value(record));
  decision.subject = subject_of(id);
  decision.accept = true;
  decision.reason = ReasonCode::Accepted;
  decision.note(ReasonCode::Accepted, decision.subject, "SmartNIC registered");
  return decision;
}

Decision Fabric::Impl::decide_register_device(const RegisterDeviceRequest& request) const {
  Decision decision;
  decision.subject = "device:new";
  if (!request.header.command.valid() || !request.header.principal.valid()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "command and principal ids must be non-zero");
    return decision;
  }
  const SmartNicRecord* smartnic = state.find_smartnic(request.smartnic);
  if (smartnic == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownSmartNic, "the SmartNIC is not registered");
    return decision;
  }
  if (request.incarnation.is_nil()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "device incarnation must be non-zero");
    return decision;
  }
  if (!request.port.valid() || request.port.value() > smartnic->port_count) {
    decision.refuse(ReasonCode::RefusedInvalidRange, "device port is outside the registered SmartNIC ports");
    return decision;
  }
  auto serial = validate_label(request.serial);
  if (!serial.ok()) {
    decision.refuse(serial.code(), serial.status().detail());
    return decision;
  }

  const DeviceRecord* existing = nullptr;
  for (const auto& device : state.devices) {
    if (device.smartnic == request.smartnic && device.port == request.port) {
      existing = &device;
      break;
    }
  }
  DeviceId id;
  if (existing != nullptr) {
    if (request.incarnation.value() < existing->incarnation.value()) {
      decision.refuse(ReasonCode::RefusedStaleDeviceIncarnation,
                      "the supplied incarnation is older than the recorded one");
      return decision;
    }
    if (request.incarnation.value() == existing->incarnation.value()) {
      decision.kind = RecordKind::DeviceRegistered;
      decision.subject = subject_of(existing->id);
      decision.accept = true;
      decision.reason = ReasonCode::AcceptedNoChange;
      decision.note(ReasonCode::AcceptedNoChange, decision.subject, "device already registered unchanged");
      DeviceRecord unchanged = *existing;
      decision.body.emplace_back("device", device_to_value(unchanged));
      return decision;
    }
    id = existing->id;
  } else {
    if (state.devices.size() >= options.config.max_devices) {
      decision.refuse(ReasonCode::RefusedCapacityExceeded, "the device registry is full");
      return decision;
    }
    std::uint64_t next = 0;
    if (!add_checked<std::uint64_t>(state.next_device, 1U, next)) {
      decision.refuse(ReasonCode::RefusedArithmeticOverflow, "device identity space is exhausted");
      return decision;
    }
    id = DeviceId::from_value(state.next_device);
    decision.override_allocator("next_device", next);
  }

  DeviceRecord record;
  record.id = id;
  record.smartnic = request.smartnic;
  record.model = request.model;
  record.incarnation = request.incarnation;
  record.serial = serial.value();
  record.port = request.port;
  record.present = false;
  if (existing != nullptr) {
    record.capability_evidence = existing->capability_evidence;
    record.capability_freshness = Freshness::Stale;
    record.registered_at = existing->registered_at;
  } else {
    record.registered_at = now();
  }

  decision.kind = RecordKind::DeviceRegistered;
  decision.body.emplace_back("device", device_to_value(record));
  decision.subject = subject_of(id);

  if (existing != nullptr) {
    // A new incarnation fences every function bound to the old one.
    Value::array_type stale;
    std::vector<ExclusiveScope> scopes;
    for (const auto& instance : state.instances) {
      if (instance.device != id || instance.incarnation == request.incarnation) {
        continue;
      }
      InstanceRecord updated = instance;
      updated.lifecycle = LifecycleState::Stale;
      updated.authority = AuthorityState::Revoked;
      updated.lease = LeaseId{};
      updated.freshness = Freshness::Stale;
      updated.last_reason = ReasonCode::RefusedStaleDeviceIncarnation;
      updated.updated_at = now();
      scopes.push_back(ExclusiveScope{updated.smartnic, updated.device, updated.port});
      stale.push_back(instance_to_value(updated));
      decision.note(ReasonCode::RefusedStaleDeviceIncarnation, subject_of(instance.id),
                    "instance bound to a superseded device incarnation");
    }
    if (!stale.empty()) {
      decision.body.emplace_back("instances", Value::array(std::move(stale)));
      FencingLedger probe = state.fencing;
      Value::array_type advances;
      for (const auto& scope : scopes) {
        const auto next_token = probe.advance(scope);
        if (!next_token.has_value()) {
          decision.refuse(ReasonCode::RefusedArithmeticOverflow, "fencing token space is exhausted");
          return decision;
        }
        advances.push_back(Value::object({
            {"device", Value::uint_value(scope.device.value())},
            {"port", Value::uint_value(scope.port.value())},
            {"smartnic", Value::uint_value(scope.smartnic.value())},
            {"token", Value::uint_value(next_token->value())},
        }));
      }
      decision.body.emplace_back("fencing", Value::array(std::move(advances)));
      decision.note(ReasonCode::AcceptedSupersededPrior, subject_of(id),
                    "device incarnation advanced and prior authority was fenced");
    }
    // Every live lease on the device is revoked, not just the first: a device
    // re-incarnation must leave no usable authority behind anywhere on it.
    Value::array_type revoked_leases;
    for (const auto& lease : state.leases) {
      if (lease.scope.device != id || lease.revoked) {
        continue;
      }
      Lease revoked = lease;
      revoked.revoked = true;
      revoked_leases.push_back(lease_to_value(revoked));
    }
    if (!revoked_leases.empty()) {
      decision.body.emplace_back("leases", Value::array(std::move(revoked_leases)));
    }
  }

  decision.accept = true;
  decision.reason = ReasonCode::Accepted;
  decision.note(ReasonCode::Accepted, decision.subject, "device registered");
  return decision;
}

Decision Fabric::Impl::decide_register_package(const RegisterPackageRequest& request) const {
  Decision decision;
  decision.subject = "package:new";
  if (!request.header.command.valid() || !request.header.principal.valid()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "command and principal ids must be non-zero");
    return decision;
  }
  auto name = validate_label(request.name);
  if (!name.ok()) {
    decision.refuse(name.code(), name.status().detail());
    return decision;
  }
  if (request.digest.is_zero()) {
    decision.refuse(ReasonCode::RefusedInvalidDigest, "package digest must be a real content digest");
    return decision;
  }
  if (request.supported_models.empty()) {
    decision.refuse(ReasonCode::RefusedMissingField, "at least one supported device model is required");
    return decision;
  }
  if (request.supported_models.size() > 256U) {
    decision.refuse(ReasonCode::RefusedOversizedInput, "at most 256 supported models are accepted");
    return decision;
  }
  if (!request.compatibility.valid()) {
    decision.refuse(ReasonCode::RefusedInvalidRange, "the compatibility range is inverted");
    return decision;
  }
  if (request.required_capabilities.size() > 64U) {
    decision.refuse(ReasonCode::RefusedOversizedInput, "at most 64 required capabilities are accepted");
    return decision;
  }

  std::vector<DeviceModelId> models = request.supported_models;
  std::sort(models.begin(), models.end());
  if (std::adjacent_find(models.begin(), models.end()) != models.end()) {
    decision.refuse(ReasonCode::RefusedDuplicateIdentity, "supported models must be unique");
    return decision;
  }
  for (const auto model : models) {
    if (model.is_nil()) {
      decision.refuse(ReasonCode::RefusedNilIdentity, "device model ids must be non-zero");
      return decision;
    }
  }
  auto capabilities = CapabilityMask::from_codes(request.required_capabilities);
  if (!capabilities.has_value()) {
    decision.refuse(ReasonCode::RefusedInvalidEnumValue, "a required capability code is not known");
    return decision;
  }

  for (const auto& package : state.packages) {
    if (package.name != name.value() || !(package.version == request.version)) {
      continue;
    }
    if (package.digest != request.digest) {
      decision.refuse(ReasonCode::RefusedDuplicateIdentity,
                      "the same package name and version already exists with a different digest");
      return decision;
    }
    decision.kind = RecordKind::PackageRegistered;
    decision.subject = subject_of(package.id);
    decision.accept = true;
    decision.reason = ReasonCode::AcceptedNoChange;
    decision.note(ReasonCode::AcceptedNoChange, decision.subject, "identical package already registered");
    decision.body.emplace_back("package", package_to_value(package));
    return decision;
  }

  if (state.packages.size() >= options.config.max_packages) {
    decision.refuse(ReasonCode::RefusedCapacityExceeded, "the package registry is full");
    return decision;
  }
  std::uint64_t next = 0;
  if (!add_checked<std::uint64_t>(state.next_package, 1U, next)) {
    decision.refuse(ReasonCode::RefusedArithmeticOverflow, "package identity space is exhausted");
    return decision;
  }
  const FunctionPackageId id = FunctionPackageId::from_value(state.next_package);

  PackageRecord record;
  record.id = id;
  record.name = name.value();
  record.version = request.version;
  record.digest = request.digest;
  record.supported_models = std::move(models);
  record.required_capabilities = *capabilities;
  record.min_capability_generation = request.min_capability_generation;
  record.compatibility = request.compatibility;
  record.min_firmware = request.min_firmware;
  record.demand = request.demand;
  record.exclusive_scope = request.exclusive_scope;
  record.layout_revision = request.layout_revision;
  record.registered_at = now();

  decision.kind = RecordKind::PackageRegistered;
  decision.override_allocator("next_package", next);
  decision.body.emplace_back("package", package_to_value(record));
  decision.subject = subject_of(id);
  decision.accept = true;
  decision.reason = ReasonCode::Accepted;
  decision.note_with_evidence(ReasonCode::Accepted, decision.subject, "package registered", request.digest);
  return decision;
}

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

Decision Fabric::Impl::decide_capability_evidence(const CapabilityEvidenceRequest& request) const {
  Decision decision;
  decision.subject = "device:new";
  if (!request.header.command.valid() || !request.header.principal.valid()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "command and principal ids must be non-zero");
    return decision;
  }
  if (!is_trusted_capability_source(request.source)) {
    decision.refuse(ReasonCode::RefusedEvidenceProvenanceUntrusted,
                    "capability evidence must come from a capability evidence provider, an operator or a "
                    "labelled synthetic fixture");
    return decision;
  }
  if (request.payload_digest.is_zero()) {
    decision.refuse(ReasonCode::RefusedMissingEvidence,
                    "capability evidence must carry the digest of the report it summarises");
    return decision;
  }
  const TimestampNs at = now();
  auto freshness = evaluate_freshness(request.observed_at, at, options.config.freshness.capability_max_age);
  if (!freshness.ok()) {
    decision.refuse(freshness.code(), freshness.status().detail());
    return decision;
  }
  if (freshness.value() == Freshness::Expired) {
    decision.refuse(ReasonCode::RefusedExpiredEvidence, "the capability report is older than the freshness window");
    return decision;
  }
  if (request.incarnation.is_nil()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "the device incarnation must be non-zero");
    return decision;
  }
  const SmartNicRecord* smartnic = state.find_smartnic(request.smartnic);
  if (smartnic == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownSmartNic, "the SmartNIC is not registered");
    return decision;
  }
  const DeviceRecord* device = state.find_device(request.device);
  if (device == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownDevice, "the device is not registered");
    return decision;
  }
  if (device->smartnic != request.smartnic) {
    decision.refuse(ReasonCode::RefusedUnknownDevice, "the device does not belong to that SmartNIC");
    return decision;
  }
  if (request.incarnation != device->incarnation) {
    decision.refuse(request.incarnation.value() < device->incarnation.value()
                        ? ReasonCode::RefusedStaleDeviceIncarnation
                        : ReasonCode::RefusedDeviceIncarnationMismatch,
                    "the report describes a different device incarnation");
    return decision;
  }
  if (request.capability_generation.value() < device->capability_generation.value()) {
    decision.refuse(ReasonCode::RefusedStaleGeneration,
                    "the report carries a capability generation older than the recorded one");
    return decision;
  }
  auto capabilities = CapabilityMask::from_codes(request.capabilities);
  if (!capabilities.has_value()) {
    decision.refuse(ReasonCode::RefusedInvalidEnumValue, "a capability code is not known");
    return decision;
  }
  if (request.firmware.major == 0U && request.firmware.minor == 0U && request.firmware.patch == 0U) {
    decision.refuse(ReasonCode::RefusedMissingField, "firmware version must be reported");
    return decision;
  }
  std::uint64_t next_evidence = 0;
  if (!add_checked<std::uint64_t>(state.next_evidence, 1U, next_evidence)) {
    decision.refuse(ReasonCode::RefusedArithmeticOverflow, "evidence identity space is exhausted");
    return decision;
  }

  CapabilityEvidenceRecord record;
  record.evidence.envelope.id = EvidenceId::from_value(state.next_evidence);
  record.evidence.envelope.kind = EvidenceKind::CapabilityReport;
  record.evidence.envelope.source = request.source;
  record.evidence.envelope.payload_digest = request.payload_digest;
  record.evidence.envelope.observed_at = request.observed_at;
  record.evidence.envelope.accepted_at = at;
  record.evidence.envelope.accepted_epoch = state.epoch;
  record.evidence.envelope.policy_generation = state.policy_generation;
  record.evidence.envelope.freshness = Freshness::Fresh;
  record.evidence.envelope.synthetic = request.synthetic || request.source == EvidenceSource::SyntheticFixture;
  record.evidence.smartnic = request.smartnic;
  record.evidence.device = request.device;
  record.evidence.incarnation = request.incarnation;
  record.evidence.model = request.model;
  record.evidence.capability_generation = request.capability_generation;
  record.evidence.compatibility_generation = request.compatibility_generation;
  record.evidence.firmware = request.firmware;
  record.evidence.capabilities = *capabilities;
  std::uint64_t next_sequence = 0;
  if (!add_checked<std::uint64_t>(state.last_sequence.value(), 1U, next_sequence)) {
    decision.refuse(ReasonCode::RefusedArithmeticOverflow, "journal sequence exhausted");
    return decision;
  }
  record.sequence = RecordSequence::from_value(next_sequence);

  DeviceRecord updated = *device;
  updated.capability_generation = request.capability_generation;
  updated.compatibility_generation = request.compatibility_generation;
  updated.firmware = request.firmware;
  updated.capabilities = *capabilities;
  updated.capability_evidence = record.evidence.envelope.id;
  updated.capability_freshness = Freshness::Fresh;
  updated.last_observed_at = request.observed_at;

  decision.kind = RecordKind::CapabilityEvidenceAccepted;
  decision.override_allocator("next_evidence", next_evidence);
  decision.body.emplace_back("device", device_to_value(updated));
  decision.body.emplace_back("evidence", capability_evidence_to_value(record));
  decision.subject = subject_of(request.device);
  decision.accept = true;
  decision.reason = ReasonCode::Accepted;
  decision.note_with_evidence(ReasonCode::Accepted, decision.subject, "capability evidence accepted",
                              request.payload_digest);
  return decision;
}

Decision Fabric::Impl::decide_observation(const ObservationRequest& request) const {
  Decision decision;
  decision.subject = "device:new";
  if (!request.header.command.valid() || !request.header.principal.valid()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "command and principal ids must be non-zero");
    return decision;
  }
  if (!is_trusted_observation_source(request.source)) {
    decision.refuse(ReasonCode::RefusedEvidenceProvenanceUntrusted,
                    "observations must come from an observation runtime, an operator or a labelled fixture");
    return decision;
  }
  if (request.payload_digest.is_zero()) {
    decision.refuse(ReasonCode::RefusedMissingEvidence, "an observation must carry the digest of its source report");
    return decision;
  }
  const TimestampNs at = now();
  auto freshness = evaluate_freshness(request.observed_at, at, options.config.freshness.observation_max_age);
  if (!freshness.ok()) {
    decision.refuse(freshness.code(), freshness.status().detail());
    return decision;
  }
  if (freshness.value() == Freshness::Expired) {
    decision.refuse(ReasonCode::RefusedExpiredEvidence, "the observation is older than the freshness window");
    return decision;
  }
  const DeviceRecord* device = state.find_device(request.device);
  if (device == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownDevice, "the device is not registered");
    return decision;
  }
  if (state.find_smartnic(request.smartnic) == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownSmartNic, "the SmartNIC is not registered");
    return decision;
  }
  if (device->smartnic != request.smartnic) {
    decision.refuse(ReasonCode::RefusedUnknownDevice, "the device does not belong to that SmartNIC");
    return decision;
  }
  if (request.incarnation != device->incarnation) {
    decision.refuse(request.incarnation.value() < device->incarnation.value()
                        ? ReasonCode::RefusedStaleDeviceIncarnation
                        : ReasonCode::RefusedDeviceIncarnationMismatch,
                    "the observation describes a different device incarnation");
    return decision;
  }
  if (request.capability_generation.value() < device->capability_generation.value()) {
    decision.refuse(ReasonCode::RefusedStaleGeneration, "the observation is older than the recorded generation");
    return decision;
  }

  std::uint64_t next_evidence = 0;
  if (!add_checked<std::uint64_t>(state.next_evidence, 1U, next_evidence)) {
    decision.refuse(ReasonCode::RefusedArithmeticOverflow, "evidence identity space is exhausted");
    return decision;
  }
  std::uint64_t next_sequence = 0;
  if (!add_checked<std::uint64_t>(state.last_sequence.value(), 1U, next_sequence)) {
    decision.refuse(ReasonCode::RefusedArithmeticOverflow, "journal sequence exhausted");
    return decision;
  }
  ObservationRecord record;
  record.evidence.envelope.id = EvidenceId::from_value(state.next_evidence);
  record.evidence.envelope.kind = EvidenceKind::DeviceObservation;
  record.evidence.envelope.source = request.source;
  record.evidence.envelope.payload_digest = request.payload_digest;
  record.evidence.envelope.observed_at = request.observed_at;
  record.evidence.envelope.accepted_at = at;
  record.evidence.envelope.accepted_epoch = state.epoch;
  record.evidence.envelope.policy_generation = state.policy_generation;
  record.evidence.envelope.freshness = Freshness::Fresh;
  record.evidence.envelope.synthetic = request.synthetic || request.source == EvidenceSource::SyntheticFixture;
  record.evidence.smartnic = request.smartnic;
  record.evidence.device = request.device;
  record.evidence.incarnation = request.incarnation;
  record.evidence.present = request.present;
  record.evidence.capability_generation = request.capability_generation;
  record.evidence.compatibility_generation = request.compatibility_generation;
  record.sequence = RecordSequence::from_value(next_sequence);

  DeviceRecord updated = *device;
  updated.present = request.present;
  updated.last_observed_at = request.observed_at;

  decision.kind = RecordKind::ObservationAccepted;
  decision.override_allocator("next_evidence", next_evidence);
  decision.body.emplace_back("device", device_to_value(updated));
  decision.body.emplace_back("observation", observation_to_value(record));
  decision.subject = subject_of(request.device);
  decision.accept = true;
  decision.reason = ReasonCode::Accepted;
  decision.note_with_evidence(ReasonCode::Accepted, decision.subject, "device observation accepted",
                              request.payload_digest);
  return decision;
}

Decision Fabric::Impl::decide_reverify(const ReverifyEvidenceRequest& request) const {
  Decision decision;
  decision.subject = "device:new";
  if (!request.header.command.valid() || !request.header.principal.valid()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "command and principal ids must be non-zero");
    return decision;
  }
  if (!is_trusted_capability_source(request.source)) {
    decision.refuse(ReasonCode::RefusedEvidenceProvenanceUntrusted, "the source cannot revalidate capability evidence");
    return decision;
  }
  if (request.payload_digest.is_zero()) {
    decision.refuse(ReasonCode::RefusedMissingEvidence, "revalidation must carry a fresh report digest");
    return decision;
  }
  const DeviceRecord* device = state.find_device(request.device);
  if (device == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownDevice, "the device is not registered");
    return decision;
  }
  if (device->incarnation != request.incarnation) {
    decision.refuse(ReasonCode::RefusedStaleDeviceIncarnation,
                    "revalidation names a different device incarnation");
    return decision;
  }
  const CapabilityEvidenceRecord* evidence = state.find_capability_evidence(request.device);
  if (evidence == nullptr) {
    decision.refuse(ReasonCode::RefusedMissingEvidence, "there is no persisted capability evidence to revalidate");
    return decision;
  }
  const TimestampNs at = now();
  auto freshness = evaluate_freshness(request.observed_at, at, options.config.freshness.capability_max_age);
  if (!freshness.ok()) {
    decision.refuse(freshness.code(), freshness.status().detail());
    return decision;
  }
  if (freshness.value() == Freshness::Expired) {
    decision.refuse(ReasonCode::RefusedExpiredEvidence, "the revalidating report is older than the freshness window");
    return decision;
  }
  if (evidence->evidence.envelope.freshness == Freshness::Fresh &&
      evidence->evidence.envelope.accepted_epoch == state.epoch) {
    decision.kind = RecordKind::EvidenceReverified;
    decision.subject = subject_of(request.device);
    decision.accept = true;
    decision.reason = ReasonCode::AcceptedNoChange;
    decision.note(ReasonCode::AcceptedNoChange, decision.subject, "evidence is already current");
    decision.body.emplace_back("device", device_to_value(*device));
    decision.body.emplace_back("evidence", capability_evidence_to_value(*evidence));
    return decision;
  }

  CapabilityEvidenceRecord updated_evidence = *evidence;
  updated_evidence.evidence.envelope.freshness = Freshness::Fresh;
  updated_evidence.evidence.envelope.accepted_at = at;
  updated_evidence.evidence.envelope.accepted_epoch = state.epoch;
  updated_evidence.evidence.envelope.observed_at = request.observed_at;
  updated_evidence.evidence.envelope.payload_digest = request.payload_digest;
  updated_evidence.evidence.envelope.source = request.source;
  DeviceRecord updated_device = *device;
  updated_device.capability_freshness = Freshness::Fresh;
  updated_device.last_observed_at = request.observed_at;

  decision.kind = RecordKind::EvidenceReverified;
  decision.body.emplace_back("device", device_to_value(updated_device));
  decision.body.emplace_back("evidence", capability_evidence_to_value(updated_evidence));
  decision.subject = subject_of(request.device);
  decision.accept = true;
  decision.reason = ReasonCode::Accepted;
  decision.note_with_evidence(ReasonCode::Accepted, decision.subject,
                              "persisted evidence revalidated under the live epoch", request.payload_digest);
  return decision;
}

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

Decision Fabric::Impl::decide_acquire(const AcquireAuthorityRequest& request) const {
  Decision decision;
  decision.subject = "lease:new";
  if (!request.header.command.valid() || !request.header.principal.valid()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "command and principal ids must be non-zero");
    return decision;
  }
  const InstanceRecord* instance = state.find_instance(request.instance);
  if (instance == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownInstance, "no such function instance");
    return decision;
  }
  const ExclusiveScope instance_scope{instance->smartnic, instance->device, instance->port};
  if (!(request.scope == instance_scope)) {
    decision.refuse(ReasonCode::RefusedLeaseScopeMismatch, "the requested scope is not this instance scope");
    return decision;
  }
  if (instance->pending_intent_ambiguous) {
    decision.refuse(ReasonCode::RefusedAmbiguousPendingReverification,
                    "the instance has an unresolved activation and cannot be re-authorised");
    return decision;
  }
  if (!permits_mutation(instance->lifecycle)) {
    if (instance->lifecycle == LifecycleState::Quiesced) {
      decision.refuse(ReasonCode::RefusedInstanceQuiesced, "the instance is quiesced");
    } else if (instance->lifecycle == LifecycleState::Withdrawn) {
      decision.refuse(ReasonCode::RefusedInstanceWithdrawn, "the instance is withdrawn");
    } else {
      decision.refuse(ReasonCode::RefusedIllegalTransition, "the instance lifecycle does not permit authorisation");
    }
    return decision;
  }
  const TimestampNs at = now();
  for (const auto& lease : state.leases) {
    if (lease.revoked || !(lease.scope == request.scope)) {
      continue;
    }
    if (at.value() >= lease.expires_at.value()) {
      continue;
    }
    if (lease.instance == request.instance && lease.holder == request.header.principal &&
        lease.epoch == state.epoch) {
      decision.kind = RecordKind::AuthorityGranted;
      decision.subject = subject_of(lease.id);
      decision.accept = true;
      decision.reason = ReasonCode::AcceptedNoChange;
      decision.activation.lease = lease.id;
      decision.activation.token = lease.token;
      decision.activation.instance = request.instance;
      decision.activation.scope = request.scope;
      decision.note_with_generation(ReasonCode::AcceptedNoChange, decision.subject,
                                    "a live lease already covers this scope", lease.token.value());
      decision.body.emplace_back("lease", lease_to_value(lease));
      return decision;
    }
    decision.refuse(ReasonCode::RefusedLeaseHeldByOther, "another live lease covers this scope");
    return decision;
  }
  const DeviceRecord* device = state.find_device(instance->device);
  if (device == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownDevice, "the bound device is not registered");
    return decision;
  }
  if (device->incarnation != instance->incarnation) {
    decision.refuse(ReasonCode::RefusedStaleDeviceIncarnation, "the device incarnation moved on");
    return decision;
  }
  // The durable lease table is bounded exactly like the snapshot decoder that
  // has to read it back, so the runtime can always reopen what it wrote.
  if (!prune_leases(decision)) {
    decision.refuse(ReasonCode::RefusedCapacityExceeded,
                    "the durable lease table is full of unexpired leases");
    return decision;
  }

  DurationNs ttl = request.ttl.value() == 0U ? options.config.lease_ttl : request.ttl;
  if (ttl.value() == 0U) {
    decision.refuse(ReasonCode::RefusedInvalidRange, "lease lifetime must be greater than zero");
    return decision;
  }
  const TimestampNs expires = [&] {
    const auto value = timestamp_add(at, ttl);
    return value.has_value() ? *value : TimestampNs{};
  }();
  if (expires.is_nil()) {
    decision.refuse(ReasonCode::RefusedArithmeticOverflow, "lease expiry overflows the timestamp range");
    return decision;
  }
  std::uint64_t next_lease_id = 0;
  if (!add_checked<std::uint64_t>(state.next_lease, 1U, next_lease_id)) {
    decision.refuse(ReasonCode::RefusedArithmeticOverflow, "lease identity space is exhausted");
    return decision;
  }
  FencingLedger probe = state.fencing;
  const auto token = probe.advance(request.scope);
  if (!token.has_value()) {
    decision.refuse(ReasonCode::RefusedArithmeticOverflow, "fencing token space is exhausted");
    return decision;
  }

  Lease lease;
  lease.id = LeaseId::from_value(state.next_lease);
  lease.scope = request.scope;
  lease.instance = request.instance;
  lease.holder = request.header.principal;
  lease.epoch = state.epoch;
  lease.token = *token;
  lease.granted_at = at;
  lease.expires_at = expires;
  lease.revoked = false;

  InstanceRecord updated = *instance;
  if (updated.authority != AuthorityState::Granted) {
    updated.authority = AuthorityState::Granted;
  }
  if (updated.lifecycle == LifecycleState::Unknown || updated.lifecycle == LifecycleState::Desired ||
      updated.lifecycle == LifecycleState::Eligible) {
    updated.lifecycle = LifecycleState::Authorized;
  }
  updated.lease = lease.id;
  updated.token = lease.token;
  updated.holder = request.header.principal;
  updated.last_reason = ReasonCode::Accepted;
  updated.updated_at = at;
  if (updated.desired == DesiredState::None) {
    updated.desired = DesiredState::Active;
  }

  decision.kind = RecordKind::AuthorityGranted;
  decision.override_allocator("next_lease", next_lease_id);
  decision.body.emplace_back("fencing", Value::array(fencing_entries({request.scope}, probe)));
  decision.body.emplace_back("instance", instance_to_value(updated));
  decision.body.emplace_back("lease", lease_to_value(lease));
  decision.subject = subject_of(lease.id);
  decision.accept = true;
  decision.reason = ReasonCode::Accepted;
  decision.activation.lease = lease.id;
  decision.activation.token = lease.token;
  decision.activation.instance = request.instance;
  decision.activation.scope = request.scope;
  decision.activation.expires_at = lease.expires_at;
  decision.note_with_generation(ReasonCode::Accepted, decision.subject, "authority granted", lease.token.value());
  return decision;
}

Decision Fabric::Impl::decide_release(const ReleaseAuthorityRequest& request) const {
  Decision decision;
  decision.subject = "lease:new";
  if (!request.header.command.valid() || !request.header.principal.valid()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "command and principal ids must be non-zero");
    return decision;
  }
  const InstanceRecord* instance = state.find_instance(request.instance);
  if (instance == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownInstance, "no such function instance");
    return decision;
  }
  const Lease* lease = state.find_lease(request.lease);
  if (lease == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownLease, "no such lease");
    return decision;
  }
  decision.subject = subject_of(request.lease);
  if (lease->instance != request.instance) {
    decision.refuse(ReasonCode::RefusedNoAuthority, "the lease belongs to a different instance");
    return decision;
  }
  if (lease->revoked) {
    decision.kind = RecordKind::AuthorityRevoked;
    decision.accept = true;
    decision.reason = ReasonCode::AcceptedNoChange;
    decision.body.emplace_back("lease", lease_to_value(*lease));
    decision.note(ReasonCode::AcceptedNoChange, decision.subject, "the lease is already revoked");
    return decision;
  }
  if (request.token != lease->token) {
    decision.refuse(ReasonCode::RefusedStaleFencingToken, "the supplied token does not match the lease");
    return decision;
  }
  Lease revoked = *lease;
  revoked.revoked = true;
  InstanceRecord updated = *instance;
  if (updated.authority == AuthorityState::Granted && updated.lease == lease->id) {
    updated.authority = AuthorityState::Revoked;
    updated.lease = LeaseId{};
  }
  updated.updated_at = now();
  decision.kind = RecordKind::AuthorityRevoked;
  decision.body.emplace_back("instance", instance_to_value(updated));
  decision.body.emplace_back("lease", lease_to_value(revoked));
  decision.subject = subject_of(lease->id);
  decision.accept = true;
  decision.reason = ReasonCode::Accepted;
  decision.note_with_generation(ReasonCode::Accepted, decision.subject, "authority released", lease->token.value());
  return decision;
}

// ---------------------------------------------------------------------------
// Planning and activation
// ---------------------------------------------------------------------------

Decision Fabric::Impl::decide_activate(const ActivateRequest& request, bool for_plan) const {
  Decision decision;
  decision.subject = "instance:new";
  const TimestampNs at = now();
  // A dry run mutates nothing, so it needs no command identity.
  if (!for_plan && (!request.header.command.valid() || !request.header.principal.valid())) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "command and principal ids must be non-zero");
    return decision;
  }
  if (!request.smartnic.valid() || !request.device.valid() || !request.package.valid()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "SmartNIC, device and package identities are required");
    return decision;
  }
  const PackageRecord* package = state.find_package(request.package);
  if (package == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownPackage, "the function package is not registered");
    return decision;
  }
  const SmartNicRecord* smartnic = state.find_smartnic(request.smartnic);
  if (smartnic == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownSmartNic, "the SmartNIC is not registered");
    return decision;
  }
  const DeviceRecord* device = state.find_device(request.device);
  if (device == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownDevice, "the device is not registered");
    return decision;
  }
  if (device->smartnic != request.smartnic) {
    decision.refuse(ReasonCode::RefusedUnknownDevice, "the device does not belong to that SmartNIC");
    return decision;
  }
  if (request.incarnation.is_nil()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "the device incarnation is required");
    return decision;
  }
  if (request.incarnation != device->incarnation) {
    decision.refuse(request.incarnation.value() < device->incarnation.value()
                        ? ReasonCode::RefusedStaleDeviceIncarnation
                        : ReasonCode::RefusedDeviceIncarnationMismatch,
                    "the request names a different device incarnation");
    return decision;
  }
  if (request.port.valid() && request.port.value() > smartnic->port_count) {
    decision.refuse(ReasonCode::RefusedInvalidRange, "the request names a port outside the SmartNIC");
    return decision;
  }
  if (package->exclusive_scope && !request.port.valid()) {
    decision.refuse(ReasonCode::RefusedMissingField, "this package requires an exclusive port scope");
    return decision;
  }

  const ExclusiveScope scope{request.smartnic, request.device,
                             package->exclusive_scope ? request.port : PortId{}};
  const InstanceRecord* existing = nullptr;
  if (request.instance.valid()) {
    existing = state.find_instance(request.instance);
    if (existing == nullptr) {
      decision.refuse(ReasonCode::RefusedUnknownInstance, "no such function instance");
      return decision;
    }
    if (existing->device != request.device || existing->smartnic != request.smartnic) {
      decision.refuse(ReasonCode::RefusedLeaseScopeMismatch, "the instance is bound to another scope");
      return decision;
    }
    if (existing->incarnation != request.incarnation) {
      decision.refuse(ReasonCode::RefusedStaleDeviceIncarnation, "the instance is bound to an older incarnation");
      return decision;
    }
  } else if (state.instances.size() >= options.config.max_instances) {
    decision.refuse(ReasonCode::RefusedCapacityExceeded, "the instance registry is full");
    return decision;
  }

  for (const auto& other : state.instances) {
    if (existing != nullptr && other.id == existing->id) {
      continue;
    }
    if (other.authority != AuthorityState::Granted) {
      continue;
    }
    if (ExclusiveScope{other.smartnic, other.device, other.port} == scope) {
      decision.refuse(ReasonCode::RefusedScopeConflict, "another function instance owns this scope");
      return decision;
    }
  }
  for (const auto& lease : state.leases) {
    if (lease.revoked || !(lease.scope == scope) || at.value() >= lease.expires_at.value()) {
      continue;
    }
    if (existing != nullptr && lease.instance == existing->id) {
      continue;
    }
    decision.refuse(ReasonCode::RefusedScopeConflict, "another live lease covers this scope");
    return decision;
  }

  if (existing != nullptr) {
    if (!for_plan) {
      auto authority = check_authority(request.claim, *existing, decision, true);
      if (!authority.ok()) {
        return decision;
      }
    }
    if (!permits_mutation(existing->lifecycle)) {
      if (existing->lifecycle == LifecycleState::Quiesced) {
        decision.refuse(ReasonCode::RefusedInstanceQuiesced, "the instance is quiesced");
      } else if (existing->lifecycle == LifecycleState::Withdrawn) {
        decision.refuse(ReasonCode::RefusedInstanceWithdrawn, "the instance is withdrawn");
      } else {
        decision.refuse(ReasonCode::RefusedIllegalTransition,
                        "the instance lifecycle does not permit activation");
      }
      return decision;
    }
    if (existing->pending_intent_ambiguous) {
      decision.refuse(ReasonCode::RefusedAmbiguousPendingReverification,
                      "the previous activation crossed a crash boundary and has not been reverified");
      return decision;
    }
    // Only attempts still awaiting an enforcement-side report occupy the bound.
    // A settled attempt is finished work even when it can still be verified.
    std::size_t in_flight = 0;
    for (const auto& attempt : existing->attempts) {
      if (attempt.phase == AttemptPhase::Staged || attempt.phase == AttemptPhase::Authorized ||
          attempt.phase == AttemptPhase::Dispatched || attempt.phase == AttemptPhase::Acknowledged) {
        ++in_flight;
      }
    }
    if (in_flight >= options.config.max_concurrent_attempts_per_instance) {
      decision.refuse(ReasonCode::RefusedPendingAttemptLimit,
                      "the bounded number of in-flight attempts for this instance is reached");
      return decision;
    }
  }

  InstanceRecord target;
  if (existing != nullptr) {
    target = *existing;
  } else {
    target.smartnic = request.smartnic;
    target.device = request.device;
    target.incarnation = request.incarnation;
    target.package = request.package;
    target.port = request.port;
    target.queue = request.queue;
  }
  auto compatibility = check_compatibility(target, *package, decision);
  if (!compatibility.ok()) {
    return decision;
  }
  if (request.policy_generation.valid() && request.policy_generation != state.policy_generation) {
    decision.refuse(ReasonCode::RefusedSupersededGeneration,
                    "the request was formed under a superseded policy generation");
    return decision;
  }
  if (request.policy_generation.valid()) {
    decision.note_with_generation(ReasonCode::Accepted, subject_of(request.device), "policy generation current",
                                  request.policy_generation.value());
  }

  const bool creates = existing == nullptr;
  const bool replaces = existing != nullptr && existing->package != request.package;
  std::uint64_t attempt_value = 1;
  if (existing != nullptr && !existing->attempts.empty()) {
    if (!add_checked<std::uint64_t>(existing->attempts.back().id.value(), 1U, attempt_value)) {
      decision.refuse(ReasonCode::RefusedArithmeticOverflow, "attempt identity space is exhausted");
      return decision;
    }
  }
  if (attempt_value > 0xFFFFFFFFULL) {
    decision.refuse(ReasonCode::RefusedArithmeticOverflow, "attempt identity exceeds 32 bits");
    return decision;
  }
  std::uint64_t generation_value = 1;
  if (existing != nullptr) {
    if (!add_checked<std::uint64_t>(existing->deployment_generation.value(), 1U, generation_value)) {
      decision.refuse(ReasonCode::RefusedArithmeticOverflow, "deployment generation space is exhausted");
      return decision;
    }
  }

  InstanceRecord updated = existing != nullptr ? *existing : InstanceRecord{};
  if (creates) {
    std::uint64_t next_instance_id = 0;
    if (!add_checked<std::uint64_t>(state.next_instance, 1U, next_instance_id)) {
      decision.refuse(ReasonCode::RefusedArithmeticOverflow, "instance identity space is exhausted");
      return decision;
    }
    updated.id = FunctionInstanceId::from_value(state.next_instance);
    updated.created_at = at;
    decision.override_allocator("next_instance", next_instance_id);
  }
  updated.smartnic = request.smartnic;
  updated.device = request.device;
  updated.incarnation = request.incarnation;
  updated.port = request.port;
  updated.queue = request.queue;
  updated.package = request.package;
  if (replaces) {
    updated.previous_package = existing->package;
    updated.previous_deployment_generation = existing->deployment_generation;
  }
  updated.deployment_generation = DeploymentGeneration::from_value(generation_value);
  updated.policy_generation =
      request.policy_generation.valid() ? request.policy_generation : state.policy_generation;
  updated.desired = DesiredState::Active;
  updated.lifecycle = LifecycleState::Authorized;
  updated.freshness = Freshness::Fresh;
  updated.authority = AuthorityState::Granted;
  updated.holder = request.header.principal;
  updated.pending_intent_ambiguous = false;
  updated.last_reason = ReasonCode::Accepted;
  updated.updated_at = at;

  FencingToken token;
  if (creates) {
    if (!prune_leases(decision)) {
      decision.refuse(ReasonCode::RefusedCapacityExceeded,
                      "the durable lease table is full of unexpired leases");
      return decision;
    }
    std::uint64_t next_lease_id = 0;
    if (!add_checked<std::uint64_t>(state.next_lease, 1U, next_lease_id)) {
      decision.refuse(ReasonCode::RefusedArithmeticOverflow, "lease identity space is exhausted");
      return decision;
    }
    DurationNs ttl = request.lease_ttl.value() == 0U ? options.config.lease_ttl : request.lease_ttl;
    if (ttl.value() == 0U) {
      decision.refuse(ReasonCode::RefusedInvalidRange, "lease lifetime must be greater than zero");
      return decision;
    }
    const auto expires = timestamp_add(at, ttl);
    if (!expires.has_value()) {
      decision.refuse(ReasonCode::RefusedArithmeticOverflow, "lease expiry overflows the timestamp range");
      return decision;
    }
    FencingLedger probe = state.fencing;
    const auto minted = probe.advance(scope);
    if (!minted.has_value()) {
      decision.refuse(ReasonCode::RefusedArithmeticOverflow, "fencing token space is exhausted");
      return decision;
    }
    token = *minted;
    Lease lease;
    lease.id = LeaseId::from_value(state.next_lease);
    lease.scope = scope;
    lease.instance = updated.id;
    lease.holder = request.header.principal;
    lease.epoch = state.epoch;
    lease.token = token;
    lease.granted_at = at;
    lease.expires_at = *expires;
    lease.revoked = false;
    updated.lease = lease.id;
    updated.token = token;
    decision.activation.lease = lease.id;
    decision.activation.expires_at = lease.expires_at;
    decision.override_allocator("next_lease", next_lease_id);
    decision.body.emplace_back("fencing", Value::array(fencing_entries({scope}, probe)));
    decision.body.emplace_back("lease", lease_to_value(lease));
  } else {
    token = existing->token;
    updated.lease = existing->lease;
    updated.token = token;
    decision.activation.lease = existing->lease;
    decision.activation.expires_at = at;
  }

  AttemptRecord attempt;
  attempt.id = AttemptId::from_value(static_cast<std::uint32_t>(attempt_value));
  attempt.phase = AttemptPhase::Dispatched;
  attempt.deployment_generation = updated.deployment_generation;
  attempt.token = token;
  attempt.lease = updated.lease;
  attempt.package = request.package;
  attempt.staged_at = at;
  attempt.dispatched_at = at;
  attempt.terminal_reason = ReasonCode::Accepted;
  updated.current_attempt = attempt.id;
  updated.attempts.push_back(attempt);
  while (updated.attempts.size() > options.config.max_attempts_per_instance && !updated.attempts.empty()) {
    updated.attempts.erase(updated.attempts.begin());
    ++decision.attempt_history_evictions;
  }

  decision.kind = creates ? RecordKind::InstanceCreated
                          : (replaces ? RecordKind::InstanceReplaced : RecordKind::AttemptDispatched);
  decision.body.emplace_back("instance", instance_to_value(updated));
  decision.subject = subject_of(updated.id);
  decision.accept = true;
  decision.reason = ReasonCode::Accepted;
  decision.activation.instance = updated.id;
  decision.activation.attempt = attempt.id;
  decision.activation.generation = updated.deployment_generation;
  decision.activation.token = token;
  decision.activation.scope = scope;
  decision.activation.creates = creates;
  decision.activation.replaces = replaces;
  decision.note_with_generation(ReasonCode::Accepted, decision.subject, "activation dispatched",
                                updated.deployment_generation.value());
  return decision;
}

DeploymentPlan Fabric::Impl::plan_from_decision(const PlanRequest& request, const Decision& decision) const {
  DeploymentPlan plan;
  plan.verdict = decision.accept ? (decision.activation.creates ? PlanVerdict::CreatesInstance : PlanVerdict::Accepted)
                                 : PlanVerdict::Refused;
  plan.primary_reason = decision.reason;
  plan.detail = decision.detail;
  plan.instance = decision.activation.instance;
  plan.creates_instance = decision.activation.creates;
  plan.replaces_instance = decision.activation.replaces;
  plan.generation = decision.activation.generation;
  plan.package = request.package;
  plan.scope = decision.activation.scope;
  plan.required_token = decision.activation.token;
  const InstanceRecord* existing = request.instance.valid() ? state.find_instance(request.instance) : nullptr;
  if (existing != nullptr) {
    plan.previous_generation = existing->deployment_generation;
    plan.previous_package = existing->package;
    if (decision.accept) {
      plan.generation = DeploymentGeneration::from_value(existing->deployment_generation.value() + 1U);
    }
  }
  plan.steps.reserve(decision.factors.size());
  for (const auto& factor : decision.factors) {
    PlanStep step;
    step.reason = factor.reason;
    step.detail = factor.detail;
    step.has_evidence = factor.has_evidence;
    step.evidence_digest = factor.evidence_digest;
    step.has_generation = factor.has_generation;
    step.generation = factor.generation;
    plan.steps.push_back(std::move(step));
  }
  return plan;
}

Value DeploymentPlan::to_value() const {
  Value::array_type items;
  items.reserve(steps.size());
  for (const auto& step : steps) {
    Value::object_type fields{
        {"detail", Value::string(step.detail)},
        {"reason", Value::string(std::string(reason_name(step.reason)))},
    };
    if (step.has_evidence) {
      fields.emplace_back("evidence_digest", Value::string(step.evidence_digest.to_hex()));
    }
    if (step.has_generation) {
      fields.emplace_back("generation", Value::uint_value(step.generation));
    }
    items.push_back(Value::object(std::move(fields)));
  }
  const char* verdict_text = verdict == PlanVerdict::Refused
                                 ? "refused"
                                 : (verdict == PlanVerdict::CreatesInstance ? "creates" : "accepted");
  return Value::object({
      {"creates_instance", Value::boolean(creates_instance)},
      {"deployment_generation", Value::uint_value(generation.value())},
      {"instance", Value::uint_value(instance.value())},
      {"package", Value::uint_value(package.value())},
      {"previous_deployment_generation", Value::uint_value(previous_generation.value())},
      {"previous_package", Value::uint_value(previous_package.value())},
      {"primary_reason", Value::string(std::string(reason_name(primary_reason)))},
      {"replaces_instance", Value::boolean(replaces_instance)},
      {"required_fencing_token", Value::uint_value(required_token.value())},
      {"scope", Value::string(scope.to_key())},
      {"steps", Value::array(std::move(items))},
      {"verdict", Value::string(std::string(verdict_text))},
  });
}

Result<DeploymentPlan> Fabric::Impl::plan_deployment(const PlanRequest& request) const noexcept {
  ActivateRequest synthetic;
  synthetic.smartnic = request.smartnic;
  synthetic.device = request.device;
  synthetic.incarnation = request.incarnation;
  synthetic.package = request.package;
  synthetic.port = request.port;
  synthetic.queue = request.queue;
  synthetic.instance = request.instance;
  synthetic.policy_generation = request.policy_generation;
  const Decision decision = decide_activate(synthetic, true);
  if (!decision.accept && decision.reason == ReasonCode::RefusedNilIdentity) {
    return Status(decision.reason, decision.detail);
  }
  return plan_from_decision(request, decision);
}

// ---------------------------------------------------------------------------
// Execution-side reports
// ---------------------------------------------------------------------------

Decision Fabric::Impl::decide_acknowledge(const AcknowledgeRequest& request) const {
  Decision decision;
  decision.subject = "instance:new";
  if (!request.header.command.valid() || !request.header.principal.valid()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "command and principal ids must be non-zero");
    return decision;
  }
  if (!is_trusted_execution_source(request.source)) {
    decision.refuse(ReasonCode::RefusedEvidenceProvenanceUntrusted,
                    "acknowledgements must come from an execution runtime or a labelled fixture");
    return decision;
  }
  const InstanceRecord* instance = state.find_instance(request.instance);
  if (instance == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownInstance, "no such function instance");
    return decision;
  }
  decision.subject = subject_of(request.instance);
  if (request.attempt != instance->current_attempt) {
    decision.refuse(ReasonCode::RefusedAttemptNotCurrent, "the acknowledgement names a stale attempt");
    return decision;
  }
  if (request.token != instance->token) {
    decision.refuse(ReasonCode::RefusedStaleFencingToken, "the acknowledgement carries a stale fencing token");
    return decision;
  }
  if (request.generation != instance->deployment_generation) {
    decision.refuse(ReasonCode::RefusedSupersededGeneration, "the acknowledgement names a superseded generation");
    return decision;
  }
  const AttemptRecord* attempt = nullptr;
  for (const auto& candidate : instance->attempts) {
    if (candidate.id == request.attempt) {
      attempt = &candidate;
    }
  }
  if (attempt == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownAttempt, "the attempt is not recorded on this instance");
    return decision;
  }
  if (attempt->phase == AttemptPhase::Acknowledged || attempt->phase == AttemptPhase::Applied ||
      attempt->phase == AttemptPhase::Verified || attempt->phase == AttemptPhase::Ambiguous) {
    decision.kind = RecordKind::AttemptSettled;
    decision.accept = true;
    decision.reason = ReasonCode::AcceptedNoChange;
    decision.body.emplace_back("instance", instance_to_value(*instance));
    decision.note(ReasonCode::AcceptedNoChange, decision.subject, "the attempt is already acknowledged");
    return decision;
  }
  if (attempt->phase != AttemptPhase::Dispatched) {
    decision.refuse(ReasonCode::RefusedAttemptAlreadySettled,
                    "the attempt is no longer awaiting acknowledgement");
    return decision;
  }
  InstanceRecord updated = *instance;
  for (auto& candidate : updated.attempts) {
    if (candidate.id == request.attempt) {
      candidate.phase = AttemptPhase::Acknowledged;
    }
  }
  if (updated.lifecycle == LifecycleState::Authorized) {
    updated.lifecycle = LifecycleState::Acknowledged;
  }
  updated.updated_at = now();
  decision.kind = RecordKind::AttemptSettled;
  decision.body.emplace_back("instance", instance_to_value(updated));
  decision.accept = true;
  decision.reason = ReasonCode::Accepted;
  decision.note_with_generation(ReasonCode::Accepted, decision.subject, "attempt acknowledged",
                                request.token.value());
  return decision;
}

Decision Fabric::Impl::decide_effect(const EffectReportRequest& request) const {
  Decision decision;
  decision.subject = "instance:new";
  if (!request.header.command.valid() || !request.header.principal.valid()) {
    decision.refuse(ReasonCode::RefusedNilIdentity, "command and principal ids must be non-zero");
    return decision;
  }
  if (!is_trusted_execution_source(request.source)) {
    decision.refuse(ReasonCode::RefusedEvidenceProvenanceUntrusted,
                    "effect reports must come from an execution runtime or a labelled fixture");
    return decision;
  }
  if (request.payload_digest.is_zero()) {
    decision.refuse(ReasonCode::RefusedMissingEvidence,
                    "an applied or verified claim requires the digest of the enforcement-side report");
    return decision;
  }
  const InstanceRecord* instance = state.find_instance(request.instance);
  if (instance == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownInstance, "no such function instance");
    return decision;
  }
  decision.subject = subject_of(request.instance);
  if (request.attempt != instance->current_attempt) {
    decision.refuse(ReasonCode::RefusedAttemptNotCurrent, "the report names an attempt that is not current");
    return decision;
  }
  if (request.token != instance->token) {
    decision.refuse(ReasonCode::RefusedStaleFencingToken, "the report carries a stale fencing token");
    return decision;
  }
  // An effect report describes what the enforcement side already did for a
  // dispatched attempt; it requests no new authority, so the fencing high-water
  // mark does not apply. The report is bound to the attempt, token and
  // generation recorded on the instance, which is what it must match.
  if (request.generation != instance->deployment_generation) {
    decision.refuse(ReasonCode::RefusedSupersededGeneration, "the report names a superseded deployment generation");
    return decision;
  }
  if (request.outcome == EffectOutcome::Unknown) {
    decision.refuse(ReasonCode::RefusedInvalidEnumValue, "the effect outcome must be stated");
    return decision;
  }
  const TimestampNs at = now();
  auto freshness = evaluate_freshness(request.observed_at, at, options.config.freshness.effect_max_age);
  if (!freshness.ok()) {
    decision.refuse(freshness.code(), freshness.status().detail());
    return decision;
  }
  if (freshness.value() == Freshness::Expired) {
    decision.refuse(ReasonCode::RefusedExpiredEvidence, "the effect report is older than the freshness window");
    return decision;
  }
  const AttemptRecord* attempt = nullptr;
  for (const auto& candidate : instance->attempts) {
    if (candidate.id == request.attempt) {
      attempt = &candidate;
    }
  }
  if (attempt == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownAttempt, "the attempt is not recorded on this instance");
    return decision;
  }
  if (attempt->phase == AttemptPhase::Failed || attempt->phase == AttemptPhase::Aborted ||
      attempt->phase == AttemptPhase::Quiesced || attempt->phase == AttemptPhase::Withdrawn) {
    decision.refuse(ReasonCode::RefusedAttemptAlreadySettled, "the attempt already reached a terminal phase");
    return decision;
  }

  AttemptPhase target_phase = AttemptPhase::Ambiguous;
  LifecycleState target_lifecycle = instance->lifecycle;
  bool revoke_authority = false;
  switch (request.outcome) {
    case EffectOutcome::Applied:
      target_phase = AttemptPhase::Applied;
      target_lifecycle = LifecycleState::Applied;
      break;
    case EffectOutcome::Verified:
      target_phase = AttemptPhase::Verified;
      target_lifecycle = LifecycleState::Verified;
      break;
    case EffectOutcome::Quiesced:
      target_phase = AttemptPhase::Quiesced;
      target_lifecycle = LifecycleState::Quiesced;
      revoke_authority = true;
      break;
    case EffectOutcome::Removed:
      target_phase = AttemptPhase::Withdrawn;
      target_lifecycle = LifecycleState::Withdrawn;
      revoke_authority = true;
      break;
    case EffectOutcome::Failed:
    case EffectOutcome::RefusedByExecutor:
      target_phase = AttemptPhase::Failed;
      target_lifecycle = LifecycleState::Failed;
      revoke_authority = true;
      break;
    case EffectOutcome::Unknown:
      decision.refuse(ReasonCode::RefusedInvalidEnumValue, "the effect outcome must be stated");
      return decision;
  }
  if (!is_legal_attempt_transition(attempt->phase, target_phase)) {
    decision.refuse(ReasonCode::RefusedIllegalTransition, "the attempt cannot move to that phase");
    return decision;
  }
  if (target_lifecycle != instance->lifecycle &&
      !is_legal_lifecycle_transition(instance->lifecycle, target_lifecycle)) {
    decision.refuse(ReasonCode::RefusedIllegalTransition, "the instance cannot move to that lifecycle state");
    return decision;
  }

  InstanceRecord updated = *instance;
  for (auto& candidate : updated.attempts) {
    if (candidate.id == request.attempt) {
      candidate.phase = target_phase;
      candidate.outcome = request.outcome;
      candidate.settled_at = at;
      candidate.effect_digest = request.payload_digest;
      candidate.effect_synthetic = request.synthetic || request.source == EvidenceSource::SyntheticFixture;
      candidate.terminal_reason = ReasonCode::Accepted;
      candidate.detail = request.detail;
    }
  }
  updated.lifecycle = target_lifecycle;
  updated.last_settled_attempt = request.attempt;
  updated.last_effect_outcome = request.outcome;
  updated.last_effect_digest = request.payload_digest;
  updated.pending_intent_ambiguous = false;
  updated.last_reason = ReasonCode::Accepted;
  updated.updated_at = at;
  if (revoke_authority) {
    updated.authority = AuthorityState::Revoked;
    updated.lease = LeaseId{};
  }

  decision.kind = RecordKind::AttemptSettled;
  decision.body.emplace_back("instance", instance_to_value(updated));
  if (revoke_authority && instance->lease.valid()) {
    const Lease* lease = state.find_lease(instance->lease);
    if (lease != nullptr && !lease->revoked) {
      Lease revoked = *lease;
      revoked.revoked = true;
      decision.body.emplace_back("lease", lease_to_value(revoked));
    }
  }
  decision.accept = true;
  decision.reason = ReasonCode::Accepted;
  decision.note_with_evidence(ReasonCode::Accepted, decision.subject,
                              std::string("effect reported as ") +
                                  std::string(effect_outcome_name(request.outcome)),
                              request.payload_digest);
  decision.note_with_generation(ReasonCode::Accepted, decision.subject, "settled attempt",
                                request.attempt.value());
  return decision;
}

// ---------------------------------------------------------------------------
// Intent: quiesce, withdraw, rollback
// ---------------------------------------------------------------------------

Decision Fabric::Impl::decide_intent(FunctionInstanceId instance_id, const AuthorityClaim& claim,
                                     DesiredState desired, ReasonCode reason, bool rollback) const {
  Decision decision;
  decision.subject = "instance:new";
  const TimestampNs at = now();
  const InstanceRecord* instance = state.find_instance(instance_id);
  if (instance == nullptr) {
    decision.refuse(ReasonCode::RefusedUnknownInstance, "no such function instance");
    return decision;
  }
  decision.subject = subject_of(instance_id);
  auto authority = check_authority(claim, *instance, decision, true);
  if (!authority.ok()) {
    return decision;
  }
  if (rollback) {
    if (instance->previous_package.is_nil()) {
      decision.refuse(ReasonCode::RefusedIllegalTransition, "there is no previous package to roll back to");
      return decision;
    }
    if (!is_effect_claim(instance->lifecycle)) {
      decision.refuse(ReasonCode::RefusedIllegalTransition,
                      "rollback requires a settled applied or verified deployment");
      return decision;
    }
    const PackageRecord* package = state.find_package(instance->previous_package);
    if (package == nullptr) {
      decision.refuse(ReasonCode::RefusedUnknownPackage, "the previous package is no longer registered");
      return decision;
    }
    std::uint64_t attempt_value = 1;
    if (!instance->attempts.empty() &&
        !add_checked<std::uint64_t>(instance->attempts.back().id.value(), 1U, attempt_value)) {
      decision.refuse(ReasonCode::RefusedArithmeticOverflow, "attempt identity space is exhausted");
      return decision;
    }
    if (attempt_value > 0xFFFFFFFFULL) {
      decision.refuse(ReasonCode::RefusedArithmeticOverflow, "attempt identity exceeds 32 bits");
      return decision;
    }
    std::uint64_t generation_value = 0;
    if (!add_checked<std::uint64_t>(instance->deployment_generation.value(), 1U, generation_value)) {
      decision.refuse(ReasonCode::RefusedArithmeticOverflow, "deployment generation space is exhausted");
      return decision;
    }
    InstanceRecord target = *instance;
    target.package = package->id;
    auto compatibility = check_compatibility(target, *package, decision);
    if (!compatibility.ok()) {
      return decision;
    }
    InstanceRecord updated = *instance;
    updated.previous_package = instance->package;
    updated.previous_deployment_generation = instance->deployment_generation;
    updated.package = package->id;
    updated.deployment_generation = DeploymentGeneration::from_value(generation_value);
    updated.lifecycle = LifecycleState::Authorized;
    updated.desired = DesiredState::Active;
    updated.pending_intent_ambiguous = false;
    updated.updated_at = at;
    AttemptRecord attempt;
    attempt.id = AttemptId::from_value(static_cast<std::uint32_t>(attempt_value));
    attempt.phase = AttemptPhase::Dispatched;
    attempt.deployment_generation = updated.deployment_generation;
    attempt.token = updated.token;
    attempt.lease = updated.lease;
    attempt.package = package->id;
    attempt.staged_at = at;
    attempt.dispatched_at = at;
    attempt.terminal_reason = ReasonCode::Accepted;
    updated.current_attempt = attempt.id;
    updated.attempts.push_back(attempt);
    while (updated.attempts.size() > options.config.max_attempts_per_instance && !updated.attempts.empty()) {
      updated.attempts.erase(updated.attempts.begin());
      ++decision.attempt_history_evictions;
    }
    decision.kind = RecordKind::IntentRecorded;
    decision.body.emplace_back("instance", instance_to_value(updated));
    decision.accept = true;
    decision.reason = ReasonCode::Accepted;
    decision.activation.instance = updated.id;
    decision.activation.attempt = attempt.id;
    decision.activation.generation = updated.deployment_generation;
    decision.activation.token = updated.token;
    decision.activation.lease = updated.lease;
    decision.note_with_generation(ReasonCode::Accepted, decision.subject, "rollback dispatched",
                                  updated.deployment_generation.value());
    return decision;
  }

  if (instance->desired == desired) {
    decision.kind = RecordKind::IntentRecorded;
    decision.accept = true;
    decision.reason = ReasonCode::AcceptedNoChange;
    decision.body.emplace_back("instance", instance_to_value(*instance));
    decision.note(ReasonCode::AcceptedNoChange, decision.subject, "the intent is already recorded");
    return decision;
  }
  if (instance->lifecycle == LifecycleState::Withdrawn && desired == DesiredState::Quiesced) {
    decision.refuse(ReasonCode::RefusedInstanceWithdrawn, "a withdrawn instance cannot be quiesced");
    return decision;
  }

  std::uint64_t attempt_value = 1;
  if (!instance->attempts.empty() &&
      !add_checked<std::uint64_t>(instance->attempts.back().id.value(), 1U, attempt_value)) {
    decision.refuse(ReasonCode::RefusedArithmeticOverflow, "attempt identity space is exhausted");
    return decision;
  }
  if (attempt_value > 0xFFFFFFFFULL) {
    decision.refuse(ReasonCode::RefusedArithmeticOverflow, "attempt identity exceeds 32 bits");
    return decision;
  }
  InstanceRecord updated = *instance;
  updated.desired = desired;
  // Authority is withdrawn the instant a drain begins: no mutation may be
  // authorised while the function is on its way out.
  updated.authority = AuthorityState::Revoked;
  if (updated.lease.valid()) {
    const Lease* lease = state.find_lease(updated.lease);
    if (lease != nullptr && !lease->revoked) {
      Lease revoked = *lease;
      revoked.revoked = true;
      decision.body.emplace_back("lease", lease_to_value(revoked));
    }
  }
  updated.lease = LeaseId{};
  // The drain is its own dispatched attempt: the confirmed lifecycle state is
  // only reached when the execution side reports that it actually happened.
  AttemptRecord attempt;
  attempt.id = AttemptId::from_value(static_cast<std::uint32_t>(attempt_value));
  attempt.phase = AttemptPhase::Dispatched;
  attempt.deployment_generation = updated.deployment_generation;
  attempt.token = updated.token;
  attempt.package = updated.package;
  attempt.staged_at = at;
  attempt.dispatched_at = at;
  attempt.terminal_reason = reason;
  updated.current_attempt = attempt.id;
  updated.attempts.push_back(attempt);
  while (updated.attempts.size() > options.config.max_attempts_per_instance && !updated.attempts.empty()) {
    updated.attempts.erase(updated.attempts.begin());
    ++decision.attempt_history_evictions;
  }
  updated.last_reason = reason;
  updated.updated_at = at;
  decision.kind = RecordKind::IntentRecorded;
  decision.body.emplace_back("instance", instance_to_value(updated));
  decision.accept = true;
  decision.reason = ReasonCode::Accepted;
  decision.activation.instance = updated.id;
  decision.activation.attempt = attempt.id;
  decision.activation.generation = updated.deployment_generation;
  decision.activation.token = updated.token;
  decision.note(ReasonCode::Accepted, decision.subject,
                std::string("intent recorded as ") + std::string(desired_state_name(desired)));
  return decision;
}

// ---------------------------------------------------------------------------
// Public operations
// ---------------------------------------------------------------------------

#define SNCF_IMPL_GUARD()                                                                     \
  ++state.counters.commands_considered;                                                       \
  if (!running) {                                                                             \
    ++state.counters.commands_refused;                                                        \
    record_reason(ReasonCode::RefusedShuttingDown);                                           \
    emit_event(ReasonCode::RefusedShuttingDown, "runtime", "the runtime is shutting down");   \
    return Status(ReasonCode::RefusedShuttingDown, "the runtime is shutting down");           \
  }

#define SNCF_IMPL_DEDUPE(header, outcome_ref, result_value, assign_identity)                   \
  {                                                                                           \
    ReasonCode replay_reason = ReasonCode::Unknown;                                           \
    std::string replay_subject;                                                               \
    const DedupeState dedupe = dedupe_state(header, replay_reason, replay_subject);            \
    if (dedupe == DedupeState::Replay) {                                                      \
      ++state.counters.commands_deduplicated;                                                 \
      record_reason(ReasonCode::AcceptedIdempotentReplay);                                    \
      emit_event(ReasonCode::AcceptedIdempotentReplay, replay_subject,                        \
                 "duplicate delivery of an already applied command");                         \
      outcome_ref.reason = replay_reason;                                                     \
      outcome_ref.duplicate = true;                                                           \
      assign_identity(replay_subject);                                                        \
      return result_value;                                                                    \
    }                                                                                         \
    if (dedupe == DedupeState::BelowFloor) {                                                  \
      ++state.counters.commands_refused;                                                      \
      ++state.counters.replayed_commands_fenced;                                              \
      record_reason(ReasonCode::RefusedReplayWindowExceeded);                                 \
      emit_event(ReasonCode::RefusedReplayWindowExceeded, "command",                          \
                 "the command identity predates the retained idempotency window");            \
      return Status(ReasonCode::RefusedReplayWindowExceeded,                                  \
                    "the command identity predates the retained idempotency window, so a "     \
                    "replay cannot be told apart from new work");                             \
    }                                                                                         \
  }

Result<SmartNicRegistration> Fabric::Impl::register_smartnic(RegisterSmartNicRequest request) noexcept {
  SNCF_IMPL_GUARD();
  SmartNicRegistration result;
  SNCF_IMPL_DEDUPE(request.header, result.outcome, result, [&](const std::string& subject) {
    result.id = id_from_subject<SmartNicId>(subject, "smartnic");
  });
  const Decision decision = decide_register_smartnic(request);
  if (!decision.accept) {
    note_refusal(decision, "register_smartnic");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "register_smartnic");
  SNCF_TRY(committed);
  result.outcome.reason = decision.reason;
  result.outcome.sequence = committed.value();
  result.id = id_from_subject<SmartNicId>(decision.subject, "smartnic");
  return result;
}

Result<DeviceRegistration> Fabric::Impl::register_device(RegisterDeviceRequest request) noexcept {
  SNCF_IMPL_GUARD();
  DeviceRegistration result;
  SNCF_IMPL_DEDUPE(request.header, result.outcome, result, [&](const std::string& subject) {
    result.id = id_from_subject<DeviceId>(subject, "device");
  });
  const Decision decision = decide_register_device(request);
  if (!decision.accept) {
    note_refusal(decision, "register_device");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "register_device");
  SNCF_TRY(committed);
  result.outcome.reason = decision.reason;
  result.outcome.sequence = committed.value();
  result.id = id_from_subject<DeviceId>(decision.subject, "device");
  return result;
}

Result<PackageRegistration> Fabric::Impl::register_package(RegisterPackageRequest request) noexcept {
  SNCF_IMPL_GUARD();
  PackageRegistration result;
  SNCF_IMPL_DEDUPE(request.header, result.outcome, result, [&](const std::string& subject) {
    result.id = id_from_subject<FunctionPackageId>(subject, "package");
  });
  const Decision decision = decide_register_package(request);
  if (!decision.accept) {
    note_refusal(decision, "register_package");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "register_package");
  SNCF_TRY(committed);
  result.outcome.reason = decision.reason;
  result.outcome.sequence = committed.value();
  result.id = id_from_subject<FunctionPackageId>(decision.subject, "package");
  return result;
}

Result<EvidenceAcceptance> Fabric::Impl::submit_capability_evidence(
    CapabilityEvidenceRequest request) noexcept {
  SNCF_IMPL_GUARD();
  EvidenceAcceptance result;
  SNCF_IMPL_DEDUPE(request.header, result.outcome, result, [&](const std::string&) {});
  if (state.find_capability_evidence(request.device) != nullptr) {
    ++state.counters.evidence_superseded;
  }
  const Decision decision = decide_capability_evidence(request);
  if (!decision.accept) {
    note_refusal(decision, "submit_capability_evidence");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "submit_capability_evidence");
  SNCF_TRY(committed);
  ++state.counters.evidence_accepted;
  result.outcome.reason = decision.reason;
  result.outcome.sequence = committed.value();
  const CapabilityEvidenceRecord* stored = state.find_capability_evidence(request.device);
  if (stored != nullptr) {
    result.id = stored->evidence.envelope.id;
    result.freshness = stored->evidence.envelope.freshness;
  }
  return result;
}

Result<EvidenceAcceptance> Fabric::Impl::submit_observation(ObservationRequest request) noexcept {
  SNCF_IMPL_GUARD();
  EvidenceAcceptance result;
  SNCF_IMPL_DEDUPE(request.header, result.outcome, result, [&](const std::string&) {});
  const Decision decision = decide_observation(request);
  if (!decision.accept) {
    note_refusal(decision, "submit_observation");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "submit_observation");
  SNCF_TRY(committed);
  ++state.counters.evidence_accepted;
  result.outcome.reason = decision.reason;
  result.outcome.sequence = committed.value();
  const ObservationRecord* stored = state.find_observation(request.device);
  if (stored != nullptr) {
    result.id = stored->evidence.envelope.id;
    result.freshness = stored->evidence.envelope.freshness;
  }
  return result;
}

Result<EvidenceAcceptance> Fabric::Impl::reverify_evidence(ReverifyEvidenceRequest request) noexcept {
  SNCF_IMPL_GUARD();
  EvidenceAcceptance result;
  SNCF_IMPL_DEDUPE(request.header, result.outcome, result, [&](const std::string&) {});
  const Decision decision = decide_reverify(request);
  if (!decision.accept) {
    note_refusal(decision, "reverify_evidence");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "reverify_evidence");
  SNCF_TRY(committed);
  if (decision.reason == ReasonCode::Accepted) {
    ++state.counters.evidence_reverified;
  }
  result.outcome.reason = decision.reason;
  result.outcome.sequence = committed.value();
  const CapabilityEvidenceRecord* stored = state.find_capability_evidence(request.device);
  if (stored != nullptr) {
    result.id = stored->evidence.envelope.id;
    result.freshness = stored->evidence.envelope.freshness;
  }
  return result;
}

Result<AuthorityGrant> Fabric::Impl::acquire_authority(AcquireAuthorityRequest request) noexcept {
  SNCF_IMPL_GUARD();
  AuthorityGrant result;
  SNCF_IMPL_DEDUPE(request.header, result.outcome, result, [&](const std::string&) {});
  const Decision decision = decide_acquire(request);
  if (!decision.accept) {
    note_refusal(decision, "acquire_authority");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "acquire_authority");
  SNCF_TRY(committed);
  if (decision.reason == ReasonCode::Accepted) {
    ++state.counters.leases_granted;
  }
  result.outcome.reason = decision.reason;
  result.outcome.sequence = committed.value();
  result.lease = decision.activation.lease;
  result.token = decision.activation.token;
  result.expires_at = decision.activation.expires_at;
  return result;
}

Result<CommandOutcome> Fabric::Impl::release_authority(ReleaseAuthorityRequest request) noexcept {
  SNCF_IMPL_GUARD();
  CommandOutcome result;
  SNCF_IMPL_DEDUPE(request.header, result, result, [&](const std::string&) {});
  const Decision decision = decide_release(request);
  if (!decision.accept) {
    note_refusal(decision, "release_authority");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "release_authority");
  SNCF_TRY(committed);
  if (decision.reason == ReasonCode::Accepted) {
    ++state.counters.leases_revoked;
  }
  result.reason = decision.reason;
  result.sequence = committed.value();
  return result;
}

Result<ActivationGrant> Fabric::Impl::activate(ActivateRequest request) noexcept {
  SNCF_IMPL_GUARD();
  ActivationGrant result;
  SNCF_IMPL_DEDUPE(request.header, result.outcome, result, [&](const std::string&) {});
  const Decision decision = decide_activate(request, false);
  if (!decision.accept) {
    note_refusal(decision, "activate");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "activate");
  SNCF_TRY(committed);
  ++state.counters.attempts_staged;
  state.counters.attempt_history_evictions += decision.attempt_history_evictions;
  if (decision.activation.creates) {
    ++state.counters.instances_created;
  }
  if (decision.activation.replaces) {
    ++state.counters.instances_replaced;
  }
  result.outcome.reason = decision.reason;
  result.outcome.sequence = committed.value();
  result.instance = decision.activation.instance;
  result.attempt = decision.activation.attempt;
  result.generation = decision.activation.generation;
  result.token = decision.activation.token;
  result.lease = decision.activation.lease;
  result.scope = decision.activation.scope;
  return result;
}

Result<CommandOutcome> Fabric::Impl::acknowledge(AcknowledgeRequest request) noexcept {
  SNCF_IMPL_GUARD();
  CommandOutcome result;
  SNCF_IMPL_DEDUPE(request.header, result, result, [&](const std::string&) {});
  const Decision decision = decide_acknowledge(request);
  if (!decision.accept) {
    note_refusal(decision, "acknowledge");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "acknowledge");
  SNCF_TRY(committed);
  result.reason = decision.reason;
  result.sequence = committed.value();
  return result;
}

Result<CommandOutcome> Fabric::Impl::report_effect(EffectReportRequest request) noexcept {
  SNCF_IMPL_GUARD();
  CommandOutcome result;
  SNCF_IMPL_DEDUPE(request.header, result, result, [&](const std::string&) {});
  const std::vector<FunctionInstanceId> ambiguous_before = [this] {
    std::vector<FunctionInstanceId> ids;
    for (const auto& instance : state.instances) {
      if (instance.pending_intent_ambiguous) {
        ids.push_back(instance.id);
      }
    }
    return ids;
  }();
  const Decision decision = decide_effect(request);
  if (!decision.accept) {
    note_refusal(decision, "report_effect");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "report_effect");
  SNCF_TRY(committed);
  ++state.counters.attempts_settled;
  for (const auto id : ambiguous_before) {
    const InstanceRecord* instance = state.find_instance(id);
    if (instance != nullptr && !instance->pending_intent_ambiguous) {
      // An ambiguous boundary was resolved by enforcement-side evidence.
      if (state.counters.attempts_ambiguous > 0U) {
        --state.counters.attempts_ambiguous;
      }
    }
  }
  result.reason = decision.reason;
  result.sequence = committed.value();
  return result;
}

Result<IntentAcceptance> Fabric::Impl::request_quiesce(QuiesceRequest request) noexcept {
  SNCF_IMPL_GUARD();
  IntentAcceptance result;
  SNCF_IMPL_DEDUPE(request.header, result.outcome, result, [&](const std::string&) {});
  const Decision decision = decide_intent(request.instance, request.claim, DesiredState::Quiesced,
                                          request.reason, false);
  if (!decision.accept) {
    note_refusal(decision, "request_quiesce");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "request_quiesce");
  SNCF_TRY(committed);
  const InstanceRecord* instance = state.find_instance(request.instance);
  if (instance != nullptr) {
    result.desired = instance->desired;
    result.lifecycle = instance->lifecycle;
    result.authority = instance->authority;
  }
  result.outcome.reason = decision.reason;
  result.outcome.sequence = committed.value();
  return result;
}

Result<IntentAcceptance> Fabric::Impl::request_withdraw(WithdrawRequest request) noexcept {
  SNCF_IMPL_GUARD();
  IntentAcceptance result;
  SNCF_IMPL_DEDUPE(request.header, result.outcome, result, [&](const std::string&) {});
  const Decision decision = decide_intent(request.instance, request.claim, DesiredState::Withdrawn,
                                          request.reason, false);
  if (!decision.accept) {
    note_refusal(decision, "request_withdraw");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "request_withdraw");
  SNCF_TRY(committed);
  ++state.counters.instances_withdrawn;
  const InstanceRecord* instance = state.find_instance(request.instance);
  if (instance != nullptr) {
    result.desired = instance->desired;
    result.lifecycle = instance->lifecycle;
    result.authority = instance->authority;
  }
  result.outcome.reason = decision.reason;
  result.outcome.sequence = committed.value();
  return result;
}

Result<IntentAcceptance> Fabric::Impl::request_rollback(RollbackRequest request) noexcept {
  SNCF_IMPL_GUARD();
  IntentAcceptance result;
  SNCF_IMPL_DEDUPE(request.header, result.outcome, result, [&](const std::string&) {});
  const Decision decision = decide_intent(request.instance, request.claim, DesiredState::Active,
                                          ReasonCode::Accepted, true);
  if (!decision.accept) {
    note_refusal(decision, "request_rollback");
    return Status(decision.reason, decision.detail);
  }
  auto committed = commit(decision, request.header, decision.reason, "request_rollback");
  SNCF_TRY(committed);
  ++state.counters.attempts_staged;
  const InstanceRecord* instance = state.find_instance(request.instance);
  if (instance != nullptr) {
    result.desired = instance->desired;
    result.lifecycle = instance->lifecycle;
    result.authority = instance->authority;
  }
  result.outcome.reason = decision.reason;
  result.outcome.sequence = committed.value();
  return result;
}

#undef SNCF_IMPL_GUARD
#undef SNCF_IMPL_DEDUPE

}  // namespace sncf
