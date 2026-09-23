// Stable reason-code tables.
// Copyright 2026 Summon Software Labs.
#include "sncf/status.hpp"

#include <array>
#include <cstddef>

namespace sncf {
namespace {

struct ReasonEntry {
  ReasonCode code;
  ReasonClass klass;
  std::string_view name;
  std::string_view description;
};

// The single source of truth for reason-code spelling. Tests assert that this
// table covers every enumerator exactly once and that names are unique.
constexpr std::array<ReasonEntry, 95> kReasons{{
    {ReasonCode::Unknown, ReasonClass::Unknown, "unknown", "no reason code recorded"},
    {ReasonCode::Accepted, ReasonClass::Accepted, "accepted", "decision accepted"},
    {ReasonCode::AcceptedIdempotentReplay, ReasonClass::Accepted, "accepted_idempotent_replay",
     "duplicate delivery of a command already applied; recorded outcome returned unchanged"},
    {ReasonCode::AcceptedNoChange, ReasonClass::Accepted, "accepted_no_change",
     "request was already satisfied; no state transition was required"},
    {ReasonCode::AcceptedSupersededPrior, ReasonClass::Accepted, "accepted_superseded_prior",
     "decision accepted and an older in-flight intent was superseded by fencing"},
    {ReasonCode::AcceptedDryRun, ReasonClass::Accepted, "accepted_dry_run",
     "evaluation only; no durable state was modified"},

    {ReasonCode::RefusedMalformedInput, ReasonClass::Refused, "refused_malformed_input",
     "input is not well formed for the declared schema"},
    {ReasonCode::RefusedOversizedInput, ReasonClass::Refused, "refused_oversized_input",
     "input exceeds a configured or contractual bound"},
    {ReasonCode::RefusedTruncatedInput, ReasonClass::Refused, "refused_truncated_input",
     "input ended before the declared structure was complete"},
    {ReasonCode::RefusedMissingField, ReasonClass::Refused, "refused_missing_field",
     "a required field was absent; absence is not treated as a default value"},
    {ReasonCode::RefusedUnknownField, ReasonClass::Refused, "refused_unknown_field",
     "an unrecognised field was present and strict decoding was requested"},
    {ReasonCode::RefusedInvalidEnumValue, ReasonClass::Refused, "refused_invalid_enum_value",
     "a symbolic field carried a value outside its closed set"},
    {ReasonCode::RefusedArithmeticOverflow, ReasonClass::Refused, "refused_arithmetic_overflow",
     "a checked arithmetic operation would have overflowed"},
    {ReasonCode::RefusedNilIdentity, ReasonClass::Refused, "refused_nil_identity",
     "a nil identity was supplied where a live entity was required"},
    {ReasonCode::RefusedDuplicateIdentity, ReasonClass::Refused, "refused_duplicate_identity",
     "the identity is already bound to a different entity or payload"},
    {ReasonCode::RefusedCapacityExceeded, ReasonClass::Refused, "refused_capacity_exceeded",
     "a configured capacity bound would have been exceeded"},
    {ReasonCode::RefusedUnsupportedVersion, ReasonClass::Refused, "refused_unsupported_version",
     "the declared version is not supported by this build"},
    {ReasonCode::RefusedInvalidDigest, ReasonClass::Refused, "refused_invalid_digest",
     "a digest field was not a well formed digest"},
    {ReasonCode::RefusedInvalidRange, ReasonClass::Refused, "refused_invalid_range",
     "a declared range was empty or inverted"},
    {ReasonCode::RefusedInvalidLabel, ReasonClass::Refused, "refused_invalid_label",
     "a label failed canonical spelling or length rules"},
    {ReasonCode::RefusedTrailingGarbage, ReasonClass::Refused, "refused_trailing_garbage",
     "bytes remained after a complete value was decoded"},
    {ReasonCode::RefusedDepthExceeded, ReasonClass::Refused, "refused_depth_exceeded",
     "nesting depth exceeded the decoder bound"},
    {ReasonCode::RefusedDuplicateKey, ReasonClass::Refused, "refused_duplicate_key",
     "an object repeated a key; the later value would silently shadow the earlier one"},
    {ReasonCode::RefusedNonCanonicalNumber, ReasonClass::Refused, "refused_non_canonical_number",
     "a number was not in canonical integer form"},
    {ReasonCode::RefusedNonCanonicalOrder, ReasonClass::Refused, "refused_non_canonical_order",
     "object keys were not in the canonical byte-wise sorted order"},

    {ReasonCode::RefusedUnknownSmartNic, ReasonClass::Refused, "refused_unknown_smartnic",
     "no registered SmartNIC matches the supplied identity"},
    {ReasonCode::RefusedUnknownDevice, ReasonClass::Refused, "refused_unknown_device",
     "no registered physical device matches the supplied identity"},
    {ReasonCode::RefusedUnknownPackage, ReasonClass::Refused, "refused_unknown_package",
     "no registered function package matches the supplied identity"},
    {ReasonCode::RefusedUnknownInstance, ReasonClass::Refused, "refused_unknown_instance",
     "no function instance matches the supplied identity"},
    {ReasonCode::RefusedUnknownLease, ReasonClass::Refused, "refused_unknown_lease",
     "no lease matches the supplied identity"},
    {ReasonCode::RefusedUnknownAttempt, ReasonClass::Refused, "refused_unknown_attempt",
     "no activation attempt matches the supplied identity for this instance"},
    {ReasonCode::RefusedUnknownScope, ReasonClass::Refused, "refused_unknown_scope",
     "the exclusive scope is not known to this coordinator"},
    {ReasonCode::RefusedOutOfBoundary, ReasonClass::Refused, "refused_out_of_boundary",
     "the request targets behaviour owned by an adjacent runtime, not this one"},

    {ReasonCode::RefusedDeviceIncarnationMismatch, ReasonClass::Refused, "refused_device_incarnation_mismatch",
     "the device incarnation does not match the incarnation bound to the instance"},
    {ReasonCode::RefusedStaleDeviceIncarnation, ReasonClass::Refused, "refused_stale_device_incarnation",
     "the request carries a device incarnation older than the recorded one"},
    {ReasonCode::RefusedStaleGeneration, ReasonClass::Refused, "refused_stale_generation",
     "evidence was produced under a generation older than the current one"},
    {ReasonCode::RefusedSupersededGeneration, ReasonClass::Refused, "refused_superseded_generation",
     "the request targets a generation that has since been superseded"},
    {ReasonCode::RefusedStaleEpoch, ReasonClass::Refused, "refused_stale_epoch",
     "the coordinator epoch is older than the live epoch"},
    {ReasonCode::RefusedStaleAuthority, ReasonClass::Refused, "refused_stale_authority",
     "the presented authority has been revoked or superseded"},
    {ReasonCode::RefusedStaleFencingToken, ReasonClass::Refused, "refused_stale_fencing_token",
     "the fencing token is lower than the durable high-water mark for this scope"},
    {ReasonCode::RefusedStaleEvidence, ReasonClass::Refused, "refused_stale_evidence",
     "evidence predates the generation, incarnation or epoch it would justify"},
    {ReasonCode::RefusedExpiredEvidence, ReasonClass::Refused, "refused_expired_evidence",
     "evidence aged out of its freshness window"},
    {ReasonCode::RefusedMissingEvidence, ReasonClass::Refused, "refused_missing_evidence",
     "required evidence is absent; absence is never coerced to zero, false or success"},
    {ReasonCode::RefusedConflictingEvidence, ReasonClass::Refused, "refused_conflicting_evidence",
     "two live evidence records disagree and no deterministic precedence applies"},
    {ReasonCode::RefusedEvidenceProvenanceUntrusted, ReasonClass::Refused, "refused_evidence_provenance_untrusted",
     "evidence arrived from a provenance this runtime does not accept for that claim"},
    {ReasonCode::RefusedEvidenceNotReverified, ReasonClass::Refused, "refused_evidence_not_reverified",
     "evidence survived a restart and has not been revalidated under the new epoch"},
    {ReasonCode::RefusedUnknownFreshness, ReasonClass::Refused, "refused_unknown_freshness",
     "freshness of the evidence is unknown and cannot justify a current decision"},

    {ReasonCode::RefusedIncompatibleCapabilityGeneration, ReasonClass::Refused, "refused_incompatible_capability_generation",
     "device capability generation is below the package requirement"},
    {ReasonCode::RefusedIncompatibleCompatibilityGeneration, ReasonClass::Refused, "refused_incompatible_compatibility_generation",
     "firmware/runtime compatibility generation is outside the package supported range"},
    {ReasonCode::RefusedIncompatibleFirmware, ReasonClass::Refused, "refused_incompatible_firmware",
     "observed firmware version is below the package minimum"},
    {ReasonCode::RefusedUnsupportedDeviceModel, ReasonClass::Refused, "refused_unsupported_device_model",
     "the package does not support the observed device model"},
    {ReasonCode::RefusedUnsupportedRequiredCapability, ReasonClass::Refused, "refused_unsupported_required_capability",
     "the device does not advertise a capability the package requires"},
    {ReasonCode::RefusedCapabilityEvidenceAbsent, ReasonClass::Refused, "refused_capability_evidence_absent",
     "no accepted capability evidence exists for the device incarnation"},

    {ReasonCode::RefusedLeaseHeldByOther, ReasonClass::Refused, "refused_lease_held_by_other",
     "an unexpired lease for this scope is held by a different holder"},
    {ReasonCode::RefusedLeaseExpired, ReasonClass::Refused, "refused_lease_expired",
     "the lease expired before the operation was authorised"},
    {ReasonCode::RefusedAuthorityRevoked, ReasonClass::Refused, "refused_authority_revoked",
     "authority for this scope was explicitly revoked"},
    {ReasonCode::RefusedNoAuthority, ReasonClass::Refused, "refused_no_authority",
     "the operation requires authority that was not presented"},
    {ReasonCode::RefusedScopeConflict, ReasonClass::Refused, "refused_scope_conflict",
     "another live instance exclusively owns the requested scope"},
    {ReasonCode::RefusedFencedByNewerToken, ReasonClass::Refused, "refused_fenced_by_newer_token",
     "a newer fencing token exists for this scope, so this incarnation is fenced"},
    {ReasonCode::RefusedLeaseScopeMismatch, ReasonClass::Refused, "refused_lease_scope_mismatch",
     "the lease does not cover the scope named by the request"},

    {ReasonCode::RefusedIllegalTransition, ReasonClass::Refused, "refused_illegal_transition",
     "the requested lifecycle transition is not legal from the current state"},
    {ReasonCode::RefusedInstanceBusy, ReasonClass::Refused, "refused_instance_busy",
     "an attempt is already in flight for this instance"},
    {ReasonCode::RefusedInstanceQuiesced, ReasonClass::Refused, "refused_instance_quiesced",
     "the instance is quiesced and cannot be mutated"},
    {ReasonCode::RefusedInstanceWithdrawn, ReasonClass::Refused, "refused_instance_withdrawn",
     "the instance is withdrawn and cannot be mutated"},
    {ReasonCode::RefusedPendingAttemptLimit, ReasonClass::Refused, "refused_pending_attempt_limit",
     "the bounded number of concurrent attempts for this instance is reached"},
    {ReasonCode::RefusedAmbiguousPendingReverification, ReasonClass::Refused, "refused_ambiguous_pending_reverification",
     "a crash boundary left this effect ambiguous; it stays ambiguous until reverified"},
    {ReasonCode::RefusedAttemptNotCurrent, ReasonClass::Refused, "refused_attempt_not_current",
     "the referenced attempt is not the current attempt for this instance"},
    {ReasonCode::RefusedAttemptAlreadySettled, ReasonClass::Refused, "refused_attempt_already_settled",
     "the attempt already reached a terminal phase and cannot be settled again"},

    {ReasonCode::RefusedJournalUnavailable, ReasonClass::Refused, "refused_journal_unavailable",
     "the durable journal could not be opened or written"},
    {ReasonCode::RefusedJournalCorrupt, ReasonClass::Refused, "refused_journal_corrupt",
     "the durable journal failed integrity validation outside a recoverable tail"},
    {ReasonCode::RefusedIncompatibleStoreVersion, ReasonClass::Refused, "refused_incompatible_store_version",
     "the store format version is not supported by this build"},
    {ReasonCode::RefusedSemanticsMismatch, ReasonClass::Refused, "refused_semantics_mismatch",
     "the stored records were written under different semantics and cannot be reinterpreted"},
    {ReasonCode::RefusedStoreFull, ReasonClass::Refused, "refused_store_full",
     "the store reached its configured growth bound"},
    {ReasonCode::RefusedCommitFailed, ReasonClass::Refused, "refused_commit_failed",
     "the durable commit did not complete; nothing is acknowledged as applied"},
    {ReasonCode::RefusedSnapshotCorrupt, ReasonClass::Refused, "refused_snapshot_corrupt",
     "the snapshot failed integrity validation"},
    {ReasonCode::RefusedSequenceGap, ReasonClass::Refused, "refused_sequence_gap",
     "the record sequence has a gap, so state cannot be reconstructed"},
    {ReasonCode::RefusedReplayedRecord, ReasonClass::Refused, "refused_replayed_record",
     "a record sequence number was replayed with different content"},

    {ReasonCode::RefusedShuttingDown, ReasonClass::Refused, "refused_shutting_down",
     "the runtime is shutting down and accepts no new work"},
    {ReasonCode::RefusedCancelled, ReasonClass::Refused, "refused_cancelled",
     "the operation was cancelled before it could publish a result"},
    {ReasonCode::RefusedOverloaded, ReasonClass::Refused, "refused_overloaded",
     "a bounded queue or worker bound is saturated"},
    {ReasonCode::RefusedPermissionDenied, ReasonClass::Refused, "refused_permission_denied",
     "the principal is not permitted to perform this operation"},
    {ReasonCode::RefusedMaintenanceHold, ReasonClass::Refused, "refused_maintenance_hold",
     "an operator hold prevents mutation of this scope"},
    {ReasonCode::RefusedEvidenceSourceUnavailable, ReasonClass::Refused, "refused_evidence_source_unavailable",
     "an adjacent runtime that must supply evidence did not respond"},
    {ReasonCode::RefusedReadOnlyStore, ReasonClass::Refused, "refused_read_only_store",
     "the store was opened read-only, so no durable change may be made"},
    {ReasonCode::RefusedReplayWindowExceeded, ReasonClass::Refused, "refused_replay_window_exceeded",
     "the command predates the retained idempotency window, so a replay cannot be distinguished from new work"},
}};

constexpr std::size_t kMaxCodeValue = 907;

const ReasonEntry* lookup(ReasonCode code) noexcept {
  const auto raw = static_cast<std::uint16_t>(code);
  if (raw > kMaxCodeValue) {
    return nullptr;
  }
  for (const auto& entry : kReasons) {
    if (entry.code == code) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

ReasonClass reason_class(ReasonCode code) noexcept {
  const ReasonEntry* entry = lookup(code);
  return entry != nullptr ? entry->klass : ReasonClass::Unknown;
}

bool is_acceptance(ReasonCode code) noexcept { return reason_class(code) == ReasonClass::Accepted; }

bool is_refusal(ReasonCode code) noexcept { return reason_class(code) == ReasonClass::Refused; }

std::string_view reason_name(ReasonCode code) noexcept {
  const ReasonEntry* entry = lookup(code);
  return entry != nullptr ? entry->name : std::string_view{"unknown"};
}

ReasonCode reason_from_name(std::string_view name) noexcept {
  for (const auto& entry : kReasons) {
    if (entry.name == name) {
      return entry.code;
    }
  }
  return ReasonCode::Unknown;
}

std::string_view reason_description(ReasonCode code) noexcept {
  const ReasonEntry* entry = lookup(code);
  return entry != nullptr ? entry->description : std::string_view{"no reason code recorded"};
}

std::string Status::to_string() const {
  std::string out;
  out.reserve(detail_.size() + 32U);
  out.append(reason_name(code_));
  if (!detail_.empty()) {
    out.append(": ");
    out.append(detail_);
  }
  return out;
}

}  // namespace sncf
