// Coordinator lifecycle: durable open, replay, conservative recovery, commit and
// canonical export. Operation decisions live in fabric_ops.cpp; both halves share
// one state machine through Impl::apply.
// Copyright 2026 Summon Software Labs.
#include "fabric_impl.hpp"

#include <algorithm>
#include <limits>

#include "sncf/version.hpp"

namespace sncf {
namespace {

constexpr std::size_t kMaxEventDetailBytes = 256U;

std::string clamp_text(std::string text) {
  if (text.size() > kMaxEventDetailBytes) {
    text.resize(kMaxEventDetailBytes);
  }
  return text;
}

template <class Record>
void upsert_sorted(std::vector<Record>& items, Record record, auto key) {
  const auto id = key(record);
  const auto it = std::lower_bound(items.begin(), items.end(), id,
                                   [&key](const Record& item, const auto& value) { return key(item) < value; });
  if (it != items.end() && key(*it) == id) {
    *it = std::move(record);
    return;
  }
  items.insert(it, std::move(record));
}

constexpr std::string_view kSubjectSep = ":";

}  // namespace

std::string subject_of(FunctionInstanceId id) { return "instance:" + std::to_string(id.value()); }
std::string subject_of(DeviceId id) { return "device:" + std::to_string(id.value()); }
std::string subject_of(SmartNicId id) { return "smartnic:" + std::to_string(id.value()); }
std::string subject_of(FunctionPackageId id) { return "package:" + std::to_string(id.value()); }
std::string subject_of(LeaseId id) { return "lease:" + std::to_string(id.value()); }

bool subject_has_prefix(std::string_view subject, std::string_view prefix) noexcept {
  return subject.size() > prefix.size() && subject.compare(0, prefix.size(), prefix) == 0 &&
         subject[prefix.size()] == kSubjectSep[0];
}

std::optional<std::uint64_t> parse_subject_id(std::string_view subject) noexcept {
  const auto position = subject.find(kSubjectSep);
  if (position == std::string_view::npos || position + 1U >= subject.size()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (std::size_t i = position + 1U; i < subject.size(); ++i) {
    const char c = subject[i];
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    std::uint64_t scaled = 0;
    if (!mul_checked<std::uint64_t>(value, 10U, scaled)) {
      return std::nullopt;
    }
    std::uint64_t summed = 0;
    if (!add_checked<std::uint64_t>(scaled, static_cast<std::uint64_t>(c - '0'), summed)) {
      return std::nullopt;
    }
    value = summed;
  }
  return value;
}

/// Encodes durable fencing high-water marks as an applyable patch.
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

Value allocator_value(const FabricState& state) {
  return Value::object({
      {"next_device", Value::uint_value(state.next_device)},
      {"next_evidence", Value::uint_value(state.next_evidence)},
      {"next_instance", Value::uint_value(state.next_instance)},
      {"next_lease", Value::uint_value(state.next_lease)},
      {"next_package", Value::uint_value(state.next_package)},
      {"next_smartnic", Value::uint_value(state.next_smartnic)},
  });
}

Value::object_type allocator_advanced(const FabricState& state, std::string_view key, std::uint64_t value) {
  Value::object_type fields = allocator_value(state).as_object();
  for (auto& field : fields) {
    if (field.first == key) {
      field.second = Value::uint_value(value);
    }
  }
  return fields;
}

Result<void> apply_allocator(const Value& value, FabricState& state) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "allocator snapshot must be an object");
  }
  auto device = field_uint(value, "next_device");
  SNCF_TRY(device);
  auto evidence = field_uint(value, "next_evidence");
  SNCF_TRY(evidence);
  auto instance = field_uint(value, "next_instance");
  SNCF_TRY(instance);
  auto lease = field_uint(value, "next_lease");
  SNCF_TRY(lease);
  auto package = field_uint(value, "next_package");
  SNCF_TRY(package);
  auto smartnic = field_uint(value, "next_smartnic");
  SNCF_TRY(smartnic);
  if (device.value() == 0U || evidence.value() == 0U || instance.value() == 0U || lease.value() == 0U ||
      package.value() == 0U || smartnic.value() == 0U) {
    return Status(ReasonCode::RefusedNilIdentity, "allocator high-water marks must be non-zero");
  }
  state.next_device = device.value();
  state.next_evidence = evidence.value();
  state.next_instance = instance.value();
  state.next_lease = lease.value();
  state.next_package = package.value();
  state.next_smartnic = smartnic.value();
  return {};
}

Fabric::Impl::Impl(Fabric::Options options_in, std::unique_ptr<Journal> journal_in) noexcept
    : options(std::move(options_in)), journal(std::move(journal_in)) {
  // The injected clock is the runtime's only notion of time; when none is
  // supplied the wall clock is used. Reading it from the stored options matters:
  // getting this wrong silently changes every freshness decision.
  clock = options.clock ? options.clock : std::make_shared<SystemClock>();
  state.next_event = 1;
  recovery = journal->report();
}

TimestampNs Fabric::Impl::now() const noexcept { return clock->now(); }

void Fabric::Impl::emit_event(ReasonCode reason, std::string subject, std::string detail) noexcept {
  FabricEvent event;
  event.sequence = EventSequence::from_value(state.next_event);
  if (state.next_event < (std::numeric_limits<std::uint64_t>::max)()) {
    ++state.next_event;
  }
  event.at = now();
  event.reason = reason;
  event.subject = std::move(subject);
  event.detail = clamp_text(std::move(detail));
  if (state.events.size() >= options.config.max_events) {
    state.events.erase(state.events.begin());
    ++state.counters.events_evicted;
  }
  state.events.push_back(std::move(event));
  ++state.counters.events_emitted;
}

void Fabric::Impl::record_reason(ReasonCode reason) noexcept { state.reasons.record(reason); }

void Fabric::Impl::remember_decision(const Explanation& explanation) noexcept {
  last_decision = explanation;
  has_last_decision = true;
}

Explanation Fabric::Impl::make_explanation(const Decision& decision, std::string operation, TimestampNs at,
                                           CoordinatorEpoch epoch) const {
  Explanation explanation;
  explanation.operation = std::move(operation);
  explanation.subject = decision.subject;
  explanation.accepted = decision.accept;
  explanation.primary_reason = decision.reason;
  explanation.factors = decision.factors;
  explanation.epoch = epoch;
  explanation.at = at;
  return explanation;
}

void Fabric::Impl::note_refusal(const Decision& decision, std::string_view operation) noexcept {
  remember_decision(make_explanation(decision, std::string(operation), now(), state.epoch));
  ++state.counters.commands_refused;
  record_reason(decision.reason);
  emit_event(decision.reason, decision.subject, decision.detail);
}

Result<void> Fabric::Impl::apply(const JournalRecord& record) noexcept {
  if (state.last_sequence.value() != 0U && record.sequence.value() <= state.last_sequence.value()) {
    return Status(ReasonCode::RefusedReplayedRecord, "record sequence is not greater than the last applied");
  }
  const Value& body = record.body;
  if (!body.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "record body must be an object");
  }

  // A record is a set of entity patches. The kind labels the decision for the
  // explanation and history surfaces; the patches are what replay reproduces.
  const Value* epoch_value = body.find("epoch");
  const Value* boot_value = body.find("boot");
  if (epoch_value != nullptr || boot_value != nullptr) {
    if (epoch_value == nullptr || boot_value == nullptr) {
      return Status(ReasonCode::RefusedMissingField, "epoch and boot identity travel together");
    }
    auto epoch = field_uint(body, "epoch");
    SNCF_TRY(epoch);
    auto boot = field_uint(body, "boot");
    SNCF_TRY(boot);
    if (epoch.value() == 0U) {
      return Status(ReasonCode::RefusedNilIdentity, "coordinator epoch must be non-zero");
    }
    state.epoch = CoordinatorEpoch::from_value(epoch.value());
    state.boot = BootId::from_value(boot.value());
  }

  if (const Value* value = body.find("smartnic"); value != nullptr) {
    auto decoded = smartnic_from_value(*value);
    SNCF_TRY(decoded);
    upsert_sorted(state.smartnics, decoded.value(), [](const SmartNicRecord& r) { return r.id; });
  }
  if (const Value* value = body.find("device"); value != nullptr) {
    auto decoded = device_from_value(*value);
    SNCF_TRY(decoded);
    upsert_sorted(state.devices, decoded.value(), [](const DeviceRecord& r) { return r.id; });
  }
  if (const Value* value = body.find("package"); value != nullptr) {
    auto decoded = package_from_value(*value);
    SNCF_TRY(decoded);
    upsert_sorted(state.packages, decoded.value(), [](const PackageRecord& r) { return r.id; });
  }
  if (const Value* value = body.find("evidence"); value != nullptr) {
    auto decoded = capability_evidence_from_value(*value);
    SNCF_TRY(decoded);
    upsert_sorted(state.capability_evidence, decoded.value(),
                  [](const CapabilityEvidenceRecord& r) { return r.evidence.device; });
  }
  if (const Value* value = body.find("observation"); value != nullptr) {
    auto decoded = observation_from_value(*value);
    SNCF_TRY(decoded);
    upsert_sorted(state.observations, decoded.value(),
                  [](const ObservationRecord& r) { return r.evidence.device; });
  }
  if (const Value* value = body.find("lease"); value != nullptr) {
    auto decoded = lease_from_value(*value);
    SNCF_TRY(decoded);
    upsert_sorted(state.leases, decoded.value(), [](const Lease& l) { return l.id; });
  }
  if (const Value* value = body.find("leases"); value != nullptr) {
    if (!value->is_array()) {
      return Status(ReasonCode::RefusedMalformedInput, "leases patch must be an array");
    }
    if (value->as_array().size() > options.config.max_leases) {
      return Status(ReasonCode::RefusedOversizedInput, "leases patch exceeds the configured bound");
    }
    for (const auto& entry : value->as_array()) {
      auto decoded = lease_from_value(entry);
      SNCF_TRY(decoded);
      upsert_sorted(state.leases, decoded.value(), [](const Lease& l) { return l.id; });
    }
  }
  if (const Value* value = body.find("leases_removed"); value != nullptr) {
    if (!value->is_array()) {
      return Status(ReasonCode::RefusedMalformedInput, "leases_removed patch must be an array");
    }
    if (value->as_array().size() > options.config.max_leases) {
      return Status(ReasonCode::RefusedOversizedInput, "leases_removed patch exceeds the configured bound");
    }
    for (const auto& entry : value->as_array()) {
      if (!entry.is_uint()) {
        return Status(ReasonCode::RefusedMalformedInput, "a removed lease identity must be an integer");
      }
      const LeaseId doomed = LeaseId::from_value(entry.as_uint());
      const auto it = std::lower_bound(state.leases.begin(), state.leases.end(), doomed,
                                       [](const Lease& l, LeaseId key) { return l.id < key; });
      if (it != state.leases.end() && it->id == doomed) {
        state.leases.erase(it);
        ++state.counters.lease_evictions;
      }
    }
  }
  if (const Value* value = body.find("instance"); value != nullptr) {
    auto decoded = instance_from_value(*value);
    SNCF_TRY(decoded);
    upsert_sorted(state.instances, decoded.value(), [](const InstanceRecord& r) { return r.id; });
  }
  if (const Value* value = body.find("instances"); value != nullptr) {
    if (!value->is_array()) {
      return Status(ReasonCode::RefusedMalformedInput, "instances patch must be an array");
    }
    if (value->as_array().size() > options.config.max_instances) {
      return Status(ReasonCode::RefusedOversizedInput, "instances patch exceeds the configured bound");
    }
    for (const auto& entry : value->as_array()) {
      auto decoded = instance_from_value(entry);
      SNCF_TRY(decoded);
      upsert_sorted(state.instances, decoded.value(), [](const InstanceRecord& r) { return r.id; });
    }
  }
  if (const Value* value = body.find("fencing"); value != nullptr) {
    if (!value->is_array()) {
      return Status(ReasonCode::RefusedMalformedInput, "fencing patch must be an array");
    }
    if (value->as_array().size() > options.config.max_leases * 4U + 64U) {
      return Status(ReasonCode::RefusedOversizedInput, "fencing patch exceeds the configured bound");
    }
    for (const auto& entry : value->as_array()) {
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
      const ExclusiveScope scope{SmartNicId::from_value(smartnic.value()), DeviceId::from_value(device.value()),
                                 PortId::from_value(*port16)};
      if (!scope.valid() || token.value() == 0U) {
        return Status(ReasonCode::RefusedNilIdentity, "fencing entry must name a real scope and token");
      }
      state.fencing.observe(scope, FencingToken::from_value(token.value()));
    }
  }

  if (const Value* alloc = body.find("alloc"); alloc != nullptr) {
    SNCF_TRY(apply_allocator(*alloc, state));
  }

  if (record.command.valid()) {
    push_dedupe(record);
  }
  push_history(record);
  state.last_sequence = record.sequence;
  return {};
}


std::optional<CommandId> Fabric::Impl::dedupe_floor_for(PrincipalId principal) const noexcept {
  const auto it = std::lower_bound(state.dedupe_floor.begin(), state.dedupe_floor.end(), principal,
                                   [](const auto& entry, PrincipalId key) { return entry.first < key; });
  if (it != state.dedupe_floor.end() && it->first == principal) {
    return it->second;
  }
  return std::nullopt;
}

void Fabric::Impl::raise_dedupe_floor(PrincipalId principal, CommandId command) noexcept {
  const auto it = std::lower_bound(state.dedupe_floor.begin(), state.dedupe_floor.end(), principal,
                                   [](const auto& entry, PrincipalId key) { return entry.first < key; });
  if (it != state.dedupe_floor.end() && it->first == principal) {
    if (command > it->second) {
      it->second = command;
    }
  } else {
    state.dedupe_floor.insert(it, {principal, command});
  }
  // The floor table is bounded like every other table; losing the oldest
  // principal's floor is accounted for rather than silent.
  while (state.dedupe_floor.size() > options.config.max_dedupe_entries && !state.dedupe_floor.empty()) {
    state.dedupe_floor.erase(state.dedupe_floor.begin());
    ++state.counters.dedupe_floor_evictions;
  }
}

void Fabric::Impl::push_dedupe(const JournalRecord& record) noexcept {
  const auto same = [&record](const DedupeEntry& entry) {
    return entry.command == record.command && entry.principal == record.principal;
  };
  state.dedupe.erase(std::remove_if(state.dedupe.begin(), state.dedupe.end(), same), state.dedupe.end());
  DedupeEntry entry;
  entry.command = record.command;
  entry.principal = record.principal;
  entry.outcome = record.outcome;
  entry.subject = record.subject;
  entry.sequence = record.sequence;
  state.dedupe.push_back(std::move(entry));
  while (state.dedupe.size() > options.config.max_dedupe_entries && !state.dedupe.empty()) {
    // Remember how far the window has slid for this principal, so a later
    // redelivery of the evicted command is refused instead of re-executed.
    raise_dedupe_floor(state.dedupe.front().principal, state.dedupe.front().command);
    state.dedupe.erase(state.dedupe.begin());
    ++state.counters.dedupe_evictions;
  }
  state.counters.dedupe_entries = state.dedupe.size();
}

void Fabric::Impl::push_history(const JournalRecord& record) noexcept {
  FabricEvent event;
  event.sequence = EventSequence::from_value(record.sequence.value());
  event.at = record.at;
  event.reason = record.outcome;
  event.subject = record.subject;
  event.detail = std::string(record_kind_name(record.kind));
  if (state.history.size() >= options.config.max_history) {
    state.history.erase(state.history.begin());
    ++state.counters.history_evictions;
  }
  state.history.push_back(std::move(event));
  state.counters.history_entries = state.history.size();
}

Fabric::Impl::DedupeState Fabric::Impl::dedupe_state(const CommandHeader& header, ReasonCode& outcome,
                                                     std::string& subject) const noexcept {
  for (auto it = state.dedupe.rbegin(); it != state.dedupe.rend(); ++it) {
    if (it->command == header.command && it->principal == header.principal) {
      outcome = it->outcome;
      subject = it->subject;
      return DedupeState::Replay;
    }
  }
  const auto floor = dedupe_floor_for(header.principal);
  if (floor.has_value() && header.command.value() <= floor->value()) {
    return DedupeState::BelowFloor;
  }
  return DedupeState::New;
}

bool Fabric::Impl::dedupe_lookup(const CommandHeader& header, ReasonCode& outcome,
                                 std::string& subject) const noexcept {
  return dedupe_state(header, outcome, subject) == DedupeState::Replay;
}

std::optional<CommandOutcome> Fabric::Impl::dedupe_outcome(const CommandHeader& header) const noexcept {
  ReasonCode outcome = ReasonCode::Unknown;
  std::string subject;
  if (!dedupe_lookup(header, outcome, subject)) {
    return std::nullopt;
  }
  CommandOutcome result;
  result.reason = outcome;
  result.duplicate = true;
  return result;
}

Result<RecordSequence> Fabric::Impl::commit(const Decision& decision, const CommandHeader& header,
                                            ReasonCode outcome, std::string_view operation) noexcept {
  JournalRecord record;
  std::uint64_t next_sequence = 0;
  if (!add_checked<std::uint64_t>(state.last_sequence.value(), 1U, next_sequence)) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "journal sequence exhausted");
  }
  record.sequence = RecordSequence::from_value(next_sequence);
  record.kind = decision.kind;
  record.command = header.command;
  record.principal = header.principal;
  record.epoch = state.epoch;
  record.at = now();
  record.outcome = outcome;
  record.subject = decision.subject;

  // Allocator high-water marks are merged into exactly one patch. Two
  // allocations in a single command must never collide in the canonical object
  // encoding, which would silently drop one of them.
  Value::object_type fields = decision.body;
  Value::object_type allocator = allocator_value(state).as_object();
  for (const auto& override_entry : decision.allocator_overrides) {
    bool replaced = false;
    for (auto& field : allocator) {
      if (field.first == override_entry.first) {
        field.second = Value::uint_value(override_entry.second);
        replaced = true;
      }
    }
    if (!replaced) {
      allocator.emplace_back(override_entry.first, Value::uint_value(override_entry.second));
    }
  }
  fields.emplace_back("alloc", Value::object(std::move(allocator)));
  record.body = Value::object(std::move(fields));

  auto appended = journal->append(record.to_value());
  if (!appended.ok()) {
    ++state.counters.commands_refused;
    record_reason(appended.code());
    emit_event(appended.code(), decision.subject, appended.status().detail());
    return appended.status();
  }
  auto applied = apply(record);
  if (!applied.ok()) {
    ++state.counters.internal_apply_failures;
    record_reason(applied.code());
    emit_event(ReasonCode::RefusedCommitFailed, decision.subject,
               "durable record could not be applied in memory: " + applied.status().detail());
    return applied.status();
  }
  ++state.counters.journal_records_appended;
  state.counters.journal_bytes_retained = journal->bytes();
  state.counters.journal_max_records =
      (std::max)(state.counters.journal_max_records, journal->record_count());
  // The decision is durable, so it may now be accounted and published.
  ++state.counters.commands_accepted;
  state.counters.lease_evictions += decision.lease_evictions;
  remember_decision(make_explanation(decision, std::string(operation), record.at, state.epoch));
  record_reason(decision.reason);
  emit_event(decision.reason, decision.subject, decision.detail);
  return record.sequence;
}

Result<void> Fabric::Impl::load_snapshot_state() noexcept {
  auto document = journal->load_snapshot();
  SNCF_TRY(document);
  SNCF_TRY(state_from_value(document.value(), state, options.config));
  state.next_event = 1;
  recovery.resumed_from_snapshot = true;
  recovery.snapshot_sequence = journal->report().snapshot_sequence;
  return {};
}

Result<void> Fabric::Impl::replay() noexcept {
  std::uint64_t applied = 0;
  for (const auto& record : journal->recovered()) {
    auto result = apply(record);
    if (!result.ok()) {
      recovery.disposition = RecoveryDisposition::CorruptRefused;
      recovery.reason = result.code();
      recovery.detail = "replay refused a durable record: " + result.status().detail();
      return result.status();
    }
    ++applied;
  }
  recovery.records_recovered = applied;
  state.counters.journal_records_replayed = applied;
  return {};
}

Result<void> Fabric::Impl::conservative_recovery() noexcept {
  const auto write_recovery = [this](RecordKind kind, std::string subject, Value::object_type body,
                                     ReasonCode outcome) -> Result<void> {
    JournalRecord record;
    std::uint64_t next_sequence = 0;
    if (!add_checked<std::uint64_t>(state.last_sequence.value(), 1U, next_sequence)) {
      return Status(ReasonCode::RefusedArithmeticOverflow, "journal sequence exhausted");
    }
    record.sequence = RecordSequence::from_value(next_sequence);
    record.kind = kind;
    record.command = CommandId{};
    record.principal = PrincipalId{};
    record.epoch = state.epoch;
    record.at = now();
    record.outcome = outcome;
    record.subject = std::move(subject);
    body.emplace_back("alloc", allocator_value(state));
    record.body = Value::object(std::move(body));
    auto appended = journal->append(record.to_value());
    if (!appended.ok()) {
      return appended.status();
    }
    auto applied = apply(record);
    if (!applied.ok()) {
      return applied.status();
    }
    ++state.counters.journal_records_appended;
    return {};
  };

  // Advance the coordinator epoch and boot identity. Every authority token
  // issued by a previous incarnation is fenced by the marks that follow.
  std::uint64_t next_epoch = 0;
  std::uint64_t next_boot = 0;
  if (!add_checked<std::uint64_t>(state.epoch.value(), 1U, next_epoch) ||
      !add_checked<std::uint64_t>(state.boot.value(), 1U, next_boot)) {
    recovery.disposition = RecoveryDisposition::CorruptRefused;
    recovery.reason = ReasonCode::RefusedArithmeticOverflow;
    recovery.detail = "coordinator epoch or boot identity is exhausted";
    return Status(ReasonCode::RefusedArithmeticOverflow, recovery.detail);
  }
  {
    Value::object_type body;
    body.emplace_back("boot", Value::uint_value(next_boot));
    body.emplace_back("epoch", Value::uint_value(next_epoch));
    SNCF_TRY(write_recovery(RecordKind::EpochAdvanced, "coordinator", std::move(body), ReasonCode::Accepted));
  }

  // Every lease that was live is superseded. Nothing survives a restart as
  // usable authority.
  const std::vector<LeaseId> live_leases = [this] {
    std::vector<LeaseId> ids;
    for (const auto& lease : state.leases) {
      if (!lease.revoked) {
        ids.push_back(lease.id);
      }
    }
    return ids;
  }();
  for (const auto lease_id : live_leases) {
    const Lease* existing = state.find_lease(lease_id);
    if (existing == nullptr) {
      continue;
    }
    Lease revoked = *existing;
    revoked.revoked = true;
    Value::object_type body;
    body.emplace_back("lease", lease_to_value(revoked));
    SNCF_TRY(write_recovery(RecordKind::RecoverySupersededAuthority, subject_of(lease_id), std::move(body),
                            ReasonCode::RefusedStaleAuthority));
    ++state.counters.leases_revoked;
  }

  // Instances never keep granted authority across a restart, and an attempt
  // that was in flight becomes explicitly ambiguous rather than assumed.
  const std::vector<FunctionInstanceId> instance_ids = [this] {
    std::vector<FunctionInstanceId> ids;
    for (const auto& instance : state.instances) {
      if (instance.authority == AuthorityState::Granted || instance.pending_intent_ambiguous ||
          instance.lease.valid()) {
        ids.push_back(instance.id);
      }
    }
    return ids;
  }();
  for (const auto instance_id : instance_ids) {
    InstanceRecord* instance = state.find_instance(instance_id);
    if (instance == nullptr) {
      continue;
    }
    InstanceRecord updated = *instance;
    if (updated.authority == AuthorityState::Granted) {
      updated.authority = AuthorityState::Superseded;
    }
    for (auto& attempt : updated.attempts) {
      const bool in_flight = attempt.phase == AttemptPhase::Staged || attempt.phase == AttemptPhase::Authorized ||
                             attempt.phase == AttemptPhase::Dispatched ||
                             attempt.phase == AttemptPhase::Acknowledged;
      if (in_flight) {
        attempt.phase = AttemptPhase::Ambiguous;
        attempt.terminal_reason = ReasonCode::RefusedAmbiguousPendingReverification;
        updated.pending_intent_ambiguous = true;
        updated.lifecycle = updated.lifecycle == LifecycleState::Unknown ? LifecycleState::Stale : updated.lifecycle;
        ++state.counters.attempts_ambiguous;
      }
    }
    updated.lease = LeaseId{};
    updated.updated_at = now();
    Value::object_type body;
    body.emplace_back("instance", instance_to_value(updated));
    SNCF_TRY(write_recovery(RecordKind::RecoveryAmbiguousAttempt, subject_of(instance_id), std::move(body),
                            ReasonCode::RefusedStaleAuthority));
  }

  // Persisted evidence never returns to Fresh by itself.
  const std::vector<DeviceId> device_ids = [this] {
    std::vector<DeviceId> ids;
    for (const auto& record : state.capability_evidence) {
      ids.push_back(record.evidence.device);
    }
    return ids;
  }();
  for (const auto device_id : device_ids) {
    const CapabilityEvidenceRecord* evidence = state.find_capability_evidence(device_id);
    const DeviceRecord* device = state.find_device(device_id);
    if (evidence == nullptr || device == nullptr) {
      continue;
    }
    CapabilityEvidenceRecord updated_evidence = *evidence;
    const Freshness refreshed = refresh_after_restart(evidence->evidence.envelope.freshness,
                                                      evidence->evidence.envelope.accepted_epoch, state.epoch);
    updated_evidence.evidence.envelope.freshness = refreshed;
    DeviceRecord updated_device = *device;
    updated_device.capability_freshness = refreshed;
    if (refreshed == Freshness::PendingReverification) {
      ++state.counters.evidence_left_pending_reverification;
    }
    Value::object_type body;
    body.emplace_back("device", device_to_value(updated_device));
    body.emplace_back("evidence", capability_evidence_to_value(updated_evidence));
    SNCF_TRY(write_recovery(RecordKind::RecoveryEvidencePendingReverification, subject_of(device_id),
                            std::move(body), ReasonCode::Accepted));
  }

  // Presence is liveness, and liveness never survives a coordinator restart.
  const std::vector<DeviceId> observed_devices = [this] {
    std::vector<DeviceId> ids;
    for (const auto& record : state.observations) {
      ids.push_back(record.evidence.device);
    }
    return ids;
  }();
  for (const auto device_id : observed_devices) {
    const ObservationRecord* observation = state.find_observation(device_id);
    const DeviceRecord* device = state.find_device(device_id);
    if (observation == nullptr || device == nullptr) {
      continue;
    }
    ObservationRecord updated_observation = *observation;
    updated_observation.evidence.envelope.freshness = Freshness::PendingReverification;
    DeviceRecord updated_device = *device;
    updated_device.present = false;
    Value::object_type body;
    body.emplace_back("device", device_to_value(updated_device));
    body.emplace_back("observation", observation_to_value(updated_observation));
    SNCF_TRY(write_recovery(RecordKind::RecoveryObservationInvalidated, subject_of(device_id),
                            std::move(body), ReasonCode::Accepted));
  }

  // Fence every scope that could have carried authority. The marks are durable,
  // so a token minted before this restart can never be accepted after it.
  const std::vector<ExclusiveScope> scopes = [this] {
    std::vector<ExclusiveScope> found;
    for (const auto& lease : state.leases) {
      found.push_back(lease.scope);
    }
    for (const auto& instance : state.instances) {
      found.push_back(ExclusiveScope{instance.smartnic, instance.device, instance.port});
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
  }();
  for (const auto& scope : scopes) {
    if (!scope.valid()) {
      continue;
    }
    const auto next = state.fencing.advance(scope);
    if (!next.has_value()) {
      recovery.disposition = RecoveryDisposition::CorruptRefused;
      recovery.reason = ReasonCode::RefusedArithmeticOverflow;
      recovery.detail = "fencing token space is exhausted";
      return Status(ReasonCode::RefusedArithmeticOverflow, recovery.detail);
    }
    // The patch must use exactly the shape the applier understands: a fencing
    // array. Scalar keys would collide with the entity patches.
    (void)next;
    Value::object_type body;
    body.emplace_back("fencing", Value::array(fencing_entries({scope}, state.fencing)));
    SNCF_TRY(write_recovery(RecordKind::FencingAdvanced, scope.to_key(), std::move(body),
                            ReasonCode::Accepted));
    ++state.counters.tokens_advanced_at_recovery;
  }

  if (recovery.resumed_from_snapshot) {
    recovery.detail = std::string(recovery.detail) + "; resumed from snapshot";
  }
  return {};
}

Result<void> Fabric::Impl::initialize() noexcept {
  if (options.read_only) {
    // Inspection mode: rebuild the in-memory view and change nothing durable.
    const bool have_snapshot = journal->report().snapshot_sequence > 0U;
    if (have_snapshot) {
      auto loaded = load_snapshot_state();
      if (!loaded.ok()) {
        recovery.disposition = RecoveryDisposition::SnapshotRejected;
        recovery.reason = loaded.code();
        recovery.detail = loaded.status().detail();
        if (journal->first_sequence().value() > 1U) {
          return loaded.status();
        }
        state = FabricState{};
        state.next_event = 1;
      }
    }
    auto replayed = replay();
    if (!replayed.ok()) {
      return replayed.status();
    }
    recovery.detail = "opened read-only; no recovery record was written";
    return {};
  }
  const bool have_snapshot = journal->report().snapshot_sequence > 0U;
  if (have_snapshot) {
    auto loaded = load_snapshot_state();
    if (!loaded.ok()) {
      recovery.disposition = RecoveryDisposition::SnapshotRejected;
      recovery.reason = loaded.code();
      recovery.detail = loaded.status().detail();
      if (journal->first_sequence().value() > 1U) {
        recovery.detail += "; journal no longer covers the full history, so recovery is refused";
        return loaded.status();
      }
      state = FabricState{};
      state.next_event = 1;
    }
  }
  auto replayed = replay();
  if (!replayed.ok()) {
    return replayed.status();
  }
  auto recovered = conservative_recovery();
  if (!recovered.ok()) {
    return recovered.status();
  }
  state.counters.journal_records_dropped_on_recovery = recovery.records_dropped;
  return {};
}

Result<std::unique_ptr<Fabric>> Fabric::open(Options options) noexcept {
  if (options.store_directory.empty()) {
    return Status(ReasonCode::RefusedJournalUnavailable, "a store directory is required");
  }
  Journal::Options journal_options;
  journal_options.directory = options.store_directory;
  journal_options.max_record_bytes = options.config.max_record_bytes;
  journal_options.max_records_between_compactions = options.config.max_journal_records;
  journal_options.max_journal_bytes = options.config.max_journal_bytes;
  journal_options.sync_on_commit = options.sync_on_commit;
  journal_options.read_only = options.read_only;

  auto opened = Journal::open(std::move(journal_options));
  SNCF_TRY(opened);
  auto journal = std::move(opened.value());
  if (!recovery_is_usable(journal->report().disposition)) {
    return Status(journal->report().reason, journal->report().detail);
  }
  auto impl = std::make_unique<Impl>(std::move(options), std::move(journal));
  auto initialized = impl->initialize();
  if (!initialized.ok()) {
    return initialized.status();
  }
  return std::unique_ptr<Fabric>(new Fabric(std::move(impl)));
}

Fabric::Fabric(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Fabric::~Fabric() { shutdown(); }

// --- operations forwarded to the implementation -----------------------------

#define SNCF_FORWARD_LOCKED(expr)                  \
  std::lock_guard<std::mutex> guard(impl_->mutex); \
  if (impl_->options.read_only) {                  \
    return Status(ReasonCode::RefusedReadOnlyStore, "the store was opened read-only"); \
  }                                          \
  return impl_->expr

Result<SmartNicRegistration> Fabric::register_smartnic(RegisterSmartNicRequest request) noexcept {
  SNCF_FORWARD_LOCKED(register_smartnic(std::move(request)));
}

Result<DeviceRegistration> Fabric::register_device(RegisterDeviceRequest request) noexcept {
  SNCF_FORWARD_LOCKED(register_device(std::move(request)));
}

Result<PackageRegistration> Fabric::register_package(RegisterPackageRequest request) noexcept {
  SNCF_FORWARD_LOCKED(register_package(std::move(request)));
}

Result<EvidenceAcceptance> Fabric::submit_capability_evidence(CapabilityEvidenceRequest request) noexcept {
  SNCF_FORWARD_LOCKED(submit_capability_evidence(std::move(request)));
}

Result<EvidenceAcceptance> Fabric::submit_observation(ObservationRequest request) noexcept {
  SNCF_FORWARD_LOCKED(submit_observation(std::move(request)));
}

Result<EvidenceAcceptance> Fabric::reverify_evidence(ReverifyEvidenceRequest request) noexcept {
  SNCF_FORWARD_LOCKED(reverify_evidence(std::move(request)));
}

Result<AuthorityGrant> Fabric::acquire_authority(AcquireAuthorityRequest request) noexcept {
  SNCF_FORWARD_LOCKED(acquire_authority(std::move(request)));
}

Result<CommandOutcome> Fabric::release_authority(ReleaseAuthorityRequest request) noexcept {
  SNCF_FORWARD_LOCKED(release_authority(std::move(request)));
}

Result<DeploymentPlan> Fabric::plan_deployment(const PlanRequest& request) const noexcept {
  SNCF_FORWARD_LOCKED(plan_deployment(request));
}

Result<ActivationGrant> Fabric::activate(ActivateRequest request) noexcept {
  SNCF_FORWARD_LOCKED(activate(std::move(request)));
}

Result<CommandOutcome> Fabric::acknowledge(AcknowledgeRequest request) noexcept {
  SNCF_FORWARD_LOCKED(acknowledge(std::move(request)));
}

Result<CommandOutcome> Fabric::report_effect(EffectReportRequest request) noexcept {
  SNCF_FORWARD_LOCKED(report_effect(std::move(request)));
}

Result<IntentAcceptance> Fabric::request_quiesce(QuiesceRequest request) noexcept {
  SNCF_FORWARD_LOCKED(request_quiesce(std::move(request)));
}

Result<IntentAcceptance> Fabric::request_withdraw(WithdrawRequest request) noexcept {
  SNCF_FORWARD_LOCKED(request_withdraw(std::move(request)));
}

Result<IntentAcceptance> Fabric::request_rollback(RollbackRequest request) noexcept {
  SNCF_FORWARD_LOCKED(request_rollback(std::move(request)));
}

#undef SNCF_FORWARD_LOCKED

// --- inspection -------------------------------------------------------------

Result<InstanceRecord> Fabric::inspect_instance(FunctionInstanceId instance) const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const InstanceRecord* found = impl_->state.find_instance(instance);
  if (found == nullptr) {
    return Status(ReasonCode::RefusedUnknownInstance, "no such function instance");
  }
  return *found;
}

Result<Explanation> Fabric::explain_instance(FunctionInstanceId instance) const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const InstanceRecord* found = impl_->state.find_instance(instance);
  if (found == nullptr) {
    return Status(ReasonCode::RefusedUnknownInstance, "no such function instance");
  }
  Explanation explanation;
  explanation.operation = "describe_instance";
  explanation.subject = subject_of(instance);
  explanation.accepted = true;
  explanation.primary_reason = found->last_reason;
  explanation.epoch = impl_->state.epoch;
  explanation.at = impl_->now();
  explanation.factors.push_back(factor_of(found->last_reason, explanation.subject,
                                          std::string("lifecycle=") + std::string(lifecycle_state_name(found->lifecycle)) +
                                              " desired=" + std::string(desired_state_name(found->desired))));
  explanation.factors.push_back(
      factor_of(ReasonCode::Accepted, explanation.subject,
                std::string("authority=") + std::string(authority_state_name(found->authority)) +
                    " lease=" + std::to_string(found->lease.value())));
  explanation.factors.push_back(
      factor_of(ReasonCode::Accepted, explanation.subject,
                std::string("freshness=") + std::string(freshness_name(found->freshness)) +
                    " policy_generation=" + std::to_string(found->policy_generation.value())));
  explanation.factors.push_back(
      factor_with_generation(ReasonCode::Accepted, explanation.subject, "deployment_generation",
                             found->deployment_generation.value()));
  explanation.factors.push_back(
      factor_with_generation(ReasonCode::Accepted, explanation.subject, "fencing_token", found->token.value()));
  if (found->pending_intent_ambiguous) {
    explanation.factors.push_back(
        factor_of(ReasonCode::RefusedAmbiguousPendingReverification, explanation.subject,
                  "the last activation crossed a crash boundary and stays ambiguous until reverified"));
  }
  if (!found->last_effect_digest.is_zero()) {
    // The claim is reported together with the digest of the enforcement-side
    // report that produced it, so the explanation is checkable end to end.
    explanation.factors.push_back(factor_with_evidence(
        ReasonCode::Accepted, explanation.subject,
        std::string("last_effect=") + std::string(effect_outcome_name(found->last_effect_outcome)),
        found->last_effect_digest));
  }
  return explanation;
}

Result<Explanation> Fabric::explain_last_decision() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->has_last_decision) {
    return Status(ReasonCode::RefusedMissingEvidence, "no decision has been made yet");
  }
  return impl_->last_decision;
}

std::vector<FabricEvent> Fabric::events() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.events;
}

std::vector<FabricEvent> Fabric::history() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.history;
}

FabricCounters Fabric::counters() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.counters;
}

ReasonCounters Fabric::reasons() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.reasons;
}

RecoveryReport Fabric::recovery() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->recovery;
}

CoordinatorEpoch Fabric::epoch() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.epoch;
}

BootId Fabric::boot() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.boot;
}

const FabricConfig& Fabric::config() const noexcept { return impl_->options.config; }

Value Fabric::Impl::export_value_locked() const {
  Value durable = state_to_value(state);
  Value::array_type events;
  events.reserve(state.events.size());
  for (const auto& event : state.events) {
    events.push_back(Value::object({
        {"at", Value::uint_value(event.at.value())},
        {"detail", Value::string(event.detail)},
        {"reason", Value::string(std::string(reason_name(event.reason)))},
        {"sequence", Value::uint_value(event.sequence.value())},
        {"subject", Value::string(event.subject)},
    }));
  }
  Value::object_type runtime_fields;
  runtime_fields.emplace_back("counters", state.counters.to_value());
  runtime_fields.emplace_back("events", Value::array(std::move(events)));
  runtime_fields.emplace_back("reasons", state.reasons.to_value());
  runtime_fields.emplace_back("recovery", recovery.to_value());
  runtime_fields.emplace_back("volatile", Value::boolean(true));
  return Value::object({
      {"durable", durable},
      {"durable_digest", Value::string(durable.digest().to_hex())},
      {"runtime", Value::object(std::move(runtime_fields))},
      {"schema", Value::uint_value(kExportSchemaVersion)},
      {"semantics", Value::string(std::string(semantics_id()))},
      {"version", Value::string(std::string(version_string()))},
  });
}

Value Fabric::export_value() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->export_value_locked();
}

std::string Fabric::export_canonical() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->export_value_locked().to_canonical();
}

Digest256 Fabric::durable_digest() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return state_to_value(impl_->state).digest();
}

Result<RecordSequence> Fabric::Impl::compact_locked() noexcept {
  auto written = journal->write_snapshot(state_to_value(state), state.last_sequence);
  if (!written.ok()) {
    record_reason(written.code());
    emit_event(written.code(), "store", written.status().detail());
    return written.status();
  }
  ++state.counters.journal_compactions;
  emit_event(ReasonCode::Accepted, "store", "snapshot written and journal rewritten");
  return state.last_sequence;
}

Result<RecordSequence> Fabric::compact() noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->running) {
    return Status(ReasonCode::RefusedShuttingDown, "the runtime is shutting down");
  }
  return impl_->compact_locked();
}

void Fabric::shutdown() noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->running) {
    return;
  }
  impl_->running = false;
  impl_->journal->close();
  impl_->emit_event(ReasonCode::Accepted, "runtime", "shutdown complete");
}

bool Fabric::is_running() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->running;
}

}  // namespace sncf
