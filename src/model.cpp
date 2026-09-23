// Copyright 2026 Summon Software Labs.
#include "sncf/model.hpp"

#include <algorithm>

#include "sncf/version.hpp"

namespace sncf {

void ReasonCounters::record(ReasonCode code) noexcept {
  const auto it = std::lower_bound(counts_.begin(), counts_.end(), code,
                                   [](const auto& entry, ReasonCode key) { return entry.first < key; });
  if (it != counts_.end() && it->first == code) {
    ++it->second;
    return;
  }
  counts_.insert(it, {code, 1U});
}

void ReasonCounters::clear() noexcept { counts_.clear(); }

std::uint64_t ReasonCounters::count(ReasonCode code) const noexcept {
  const auto it = std::lower_bound(counts_.begin(), counts_.end(), code,
                                   [](const auto& entry, ReasonCode key) { return entry.first < key; });
  if (it != counts_.end() && it->first == code) {
    return it->second;
  }
  return 0;
}

std::uint64_t ReasonCounters::total_acceptances() const noexcept {
  std::uint64_t total = 0;
  for (const auto& entry : counts_) {
    if (is_acceptance(entry.first)) {
      total += entry.second;
    }
  }
  return total;
}

std::uint64_t ReasonCounters::total_refusals() const noexcept {
  std::uint64_t total = 0;
  for (const auto& entry : counts_) {
    if (is_refusal(entry.first)) {
      total += entry.second;
    }
  }
  return total;
}

Value ReasonCounters::to_value() const {
  Value::object_type fields;
  fields.reserve(counts_.size());
  for (const auto& entry : counts_) {
    if (entry.second == 0U) {
      continue;
    }
    fields.emplace_back(std::string(reason_name(entry.first)), Value::uint_value(entry.second));
  }
  return Value::object(std::move(fields));
}

Value FabricCounters::to_value() const {
  return Value::object({
      {"attempt_history_evictions", Value::uint_value(attempt_history_evictions)},
      {"attempts_ambiguous", Value::uint_value(attempts_ambiguous)},
      {"attempts_settled", Value::uint_value(attempts_settled)},
      {"attempts_staged", Value::uint_value(attempts_staged)},
      {"commands_accepted", Value::uint_value(commands_accepted)},
      {"commands_considered", Value::uint_value(commands_considered)},
      {"commands_deduplicated", Value::uint_value(commands_deduplicated)},
      {"commands_refused", Value::uint_value(commands_refused)},
      {"decode_malformed", Value::uint_value(decode_malformed)},
      {"decode_oversized", Value::uint_value(decode_oversized)},
      {"decode_truncations", Value::uint_value(decode_truncations)},
      {"dedupe_entries", Value::uint_value(dedupe_entries)},
      {"dedupe_evictions", Value::uint_value(dedupe_evictions)},
      {"dedupe_floor_evictions", Value::uint_value(dedupe_floor_evictions)},
      {"events_emitted", Value::uint_value(events_emitted)},
      {"events_evicted", Value::uint_value(events_evicted)},
      {"evidence_accepted", Value::uint_value(evidence_accepted)},
      {"evidence_left_pending_reverification", Value::uint_value(evidence_left_pending_reverification)},
      {"evidence_reverified", Value::uint_value(evidence_reverified)},
      {"evidence_superseded", Value::uint_value(evidence_superseded)},
      {"history_entries", Value::uint_value(history_entries)},
      {"history_evictions", Value::uint_value(history_evictions)},
      {"instances_created", Value::uint_value(instances_created)},
      {"instances_replaced", Value::uint_value(instances_replaced)},
      {"instances_withdrawn", Value::uint_value(instances_withdrawn)},
      {"internal_apply_failures", Value::uint_value(internal_apply_failures)},
      {"journal_bytes_retained", Value::uint_value(journal_bytes_retained)},
      {"journal_compactions", Value::uint_value(journal_compactions)},
      {"journal_max_records", Value::uint_value(journal_max_records)},
      {"journal_records_appended", Value::uint_value(journal_records_appended)},
      {"journal_records_dropped_on_recovery", Value::uint_value(journal_records_dropped_on_recovery)},
      {"journal_records_replayed", Value::uint_value(journal_records_replayed)},
      {"lease_evictions", Value::uint_value(lease_evictions)},
      {"leases_expired", Value::uint_value(leases_expired)},
      {"leases_granted", Value::uint_value(leases_granted)},
      {"leases_revoked", Value::uint_value(leases_revoked)},
      {"replayed_commands_fenced", Value::uint_value(replayed_commands_fenced)},
      {"tokens_advanced_at_recovery", Value::uint_value(tokens_advanced_at_recovery)},
  });
}

const SmartNicRecord* FabricState::find_smartnic(SmartNicId id) const noexcept {
  const auto it = std::lower_bound(smartnics.begin(), smartnics.end(), id,
                                   [](const SmartNicRecord& record, SmartNicId key) { return record.id < key; });
  return (it != smartnics.end() && it->id == id) ? &*it : nullptr;
}

SmartNicRecord* FabricState::find_smartnic(SmartNicId id) noexcept {
  const auto it = std::lower_bound(smartnics.begin(), smartnics.end(), id,
                                   [](const SmartNicRecord& record, SmartNicId key) { return record.id < key; });
  return (it != smartnics.end() && it->id == id) ? &*it : nullptr;
}

const DeviceRecord* FabricState::find_device(DeviceId id) const noexcept {
  const auto it = std::lower_bound(devices.begin(), devices.end(), id,
                                   [](const DeviceRecord& record, DeviceId key) { return record.id < key; });
  return (it != devices.end() && it->id == id) ? &*it : nullptr;
}

DeviceRecord* FabricState::find_device(DeviceId id) noexcept {
  const auto it = std::lower_bound(devices.begin(), devices.end(), id,
                                   [](const DeviceRecord& record, DeviceId key) { return record.id < key; });
  return (it != devices.end() && it->id == id) ? &*it : nullptr;
}

const PackageRecord* FabricState::find_package(FunctionPackageId id) const noexcept {
  const auto it = std::lower_bound(packages.begin(), packages.end(), id,
                                   [](const PackageRecord& record, FunctionPackageId key) { return record.id < key; });
  return (it != packages.end() && it->id == id) ? &*it : nullptr;
}

PackageRecord* FabricState::find_package(FunctionPackageId id) noexcept {
  const auto it = std::lower_bound(packages.begin(), packages.end(), id,
                                   [](const PackageRecord& record, FunctionPackageId key) { return record.id < key; });
  return (it != packages.end() && it->id == id) ? &*it : nullptr;
}

const InstanceRecord* FabricState::find_instance(FunctionInstanceId id) const noexcept {
  const auto it = std::lower_bound(instances.begin(), instances.end(), id,
                                   [](const InstanceRecord& record, FunctionInstanceId key) { return record.id < key; });
  return (it != instances.end() && it->id == id) ? &*it : nullptr;
}

InstanceRecord* FabricState::find_instance(FunctionInstanceId id) noexcept {
  const auto it = std::lower_bound(instances.begin(), instances.end(), id,
                                   [](const InstanceRecord& record, FunctionInstanceId key) { return record.id < key; });
  return (it != instances.end() && it->id == id) ? &*it : nullptr;
}

const CapabilityEvidenceRecord* FabricState::find_capability_evidence(DeviceId id) const noexcept {
  const auto it = std::lower_bound(
      capability_evidence.begin(), capability_evidence.end(), id,
      [](const CapabilityEvidenceRecord& record, DeviceId key) { return record.evidence.device < key; });
  return (it != capability_evidence.end() && it->evidence.device == id) ? &*it : nullptr;
}

const ObservationRecord* FabricState::find_observation(DeviceId id) const noexcept {
  const auto it = std::lower_bound(
      observations.begin(), observations.end(), id,
      [](const ObservationRecord& record, DeviceId key) { return record.evidence.device < key; });
  return (it != observations.end() && it->evidence.device == id) ? &*it : nullptr;
}

const Lease* FabricState::find_lease(LeaseId id) const noexcept {
  const auto it = std::lower_bound(leases.begin(), leases.end(), id,
                                   [](const Lease& lease, LeaseId key) { return lease.id < key; });
  return (it != leases.end() && it->id == id) ? &*it : nullptr;
}

Lease* FabricState::find_lease(LeaseId id) noexcept {
  const auto it = std::lower_bound(leases.begin(), leases.end(), id,
                                   [](const Lease& lease, LeaseId key) { return lease.id < key; });
  return (it != leases.end() && it->id == id) ? &*it : nullptr;
}

FunctionInstanceId FabricState::scope_owner(const ExclusiveScope& scope) const noexcept {
  FunctionInstanceId owner;
  for (const auto& instance : instances) {
    if (instance.authority != AuthorityState::Granted || !instance.lease.valid()) {
      continue;
    }
    const ExclusiveScope candidate{instance.smartnic, instance.device, instance.port};
    if (candidate == scope) {
      if (!owner.valid() || instance.id < owner) {
        owner = instance.id;
      }
    }
  }
  return owner;
}

}  // namespace sncf
