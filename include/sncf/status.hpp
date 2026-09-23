// Stable reason codes, status values and result plumbing.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_STATUS_HPP
#define SNCF_STATUS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace sncf {

/// Every accepted or refused decision in this runtime carries exactly one stable
/// numeric reason code. The numeric value is part of the persisted and exported
/// contract: it is never renumbered, only appended to. Human-readable text is
/// derived from the code, never the other way round.
enum class ReasonCode : std::uint16_t {
  // --- unknown / sentinel -------------------------------------------------
  Unknown = 0,

  // --- acceptance ---------------------------------------------------------
  Accepted = 100,
  AcceptedIdempotentReplay = 101,
  AcceptedNoChange = 102,
  AcceptedSupersededPrior = 103,
  AcceptedDryRun = 104,

  // --- input shape --------------------------------------------------------
  RefusedMalformedInput = 200,
  RefusedOversizedInput = 201,
  RefusedTruncatedInput = 202,
  RefusedMissingField = 203,
  RefusedUnknownField = 204,
  RefusedInvalidEnumValue = 205,
  RefusedArithmeticOverflow = 206,
  RefusedNilIdentity = 207,
  RefusedDuplicateIdentity = 208,
  RefusedCapacityExceeded = 209,
  RefusedUnsupportedVersion = 210,
  RefusedInvalidDigest = 211,
  RefusedInvalidRange = 212,
  RefusedInvalidLabel = 213,
  RefusedTrailingGarbage = 214,
  RefusedDepthExceeded = 215,
  RefusedDuplicateKey = 216,
  RefusedNonCanonicalNumber = 217,
  RefusedNonCanonicalOrder = 218,

  // --- identity and existence --------------------------------------------
  RefusedUnknownSmartNic = 300,
  RefusedUnknownDevice = 301,
  RefusedUnknownPackage = 302,
  RefusedUnknownInstance = 303,
  RefusedUnknownLease = 304,
  RefusedUnknownAttempt = 305,
  RefusedUnknownScope = 306,
  RefusedOutOfBoundary = 307,

  // --- generations, epochs and incarnations ------------------------------
  RefusedDeviceIncarnationMismatch = 400,
  RefusedStaleDeviceIncarnation = 401,
  RefusedStaleGeneration = 402,
  RefusedSupersededGeneration = 403,
  RefusedStaleEpoch = 404,
  RefusedStaleAuthority = 405,
  RefusedStaleFencingToken = 406,
  RefusedStaleEvidence = 407,
  RefusedExpiredEvidence = 408,
  RefusedMissingEvidence = 409,
  RefusedConflictingEvidence = 410,
  RefusedEvidenceProvenanceUntrusted = 411,
  RefusedEvidenceNotReverified = 412,
  RefusedUnknownFreshness = 413,

  // --- capability and compatibility --------------------------------------
  RefusedIncompatibleCapabilityGeneration = 500,
  RefusedIncompatibleCompatibilityGeneration = 501,
  RefusedIncompatibleFirmware = 502,
  RefusedUnsupportedDeviceModel = 503,
  RefusedUnsupportedRequiredCapability = 504,
  RefusedCapabilityEvidenceAbsent = 505,

  // --- authority and leases ----------------------------------------------
  RefusedLeaseHeldByOther = 600,
  RefusedLeaseExpired = 601,
  RefusedAuthorityRevoked = 602,
  RefusedNoAuthority = 603,
  RefusedScopeConflict = 604,
  RefusedFencedByNewerToken = 605,
  RefusedLeaseScopeMismatch = 606,

  // --- lifecycle ----------------------------------------------------------
  RefusedIllegalTransition = 700,
  RefusedInstanceBusy = 701,
  RefusedInstanceQuiesced = 702,
  RefusedInstanceWithdrawn = 703,
  RefusedPendingAttemptLimit = 704,
  RefusedAmbiguousPendingReverification = 705,
  RefusedAttemptNotCurrent = 706,
  RefusedAttemptAlreadySettled = 707,

  // --- durability ---------------------------------------------------------
  RefusedJournalUnavailable = 800,
  RefusedJournalCorrupt = 801,
  RefusedIncompatibleStoreVersion = 802,
  RefusedSemanticsMismatch = 803,
  RefusedStoreFull = 804,
  RefusedCommitFailed = 805,
  RefusedSnapshotCorrupt = 806,
  RefusedSequenceGap = 807,
  RefusedReplayedRecord = 808,

  // --- operational --------------------------------------------------------
  RefusedShuttingDown = 900,
  RefusedCancelled = 901,
  RefusedOverloaded = 902,
  RefusedPermissionDenied = 903,
  RefusedMaintenanceHold = 904,
  RefusedEvidenceSourceUnavailable = 905,
  RefusedReadOnlyStore = 906,
  RefusedReplayWindowExceeded = 907,
};

/// Classification of a reason code. Used by explanations and counters; it is
/// derived, never stored, so it can never disagree with the code itself.
enum class ReasonClass : std::uint8_t {
  Unknown = 0,
  Accepted = 1,
  Refused = 2,
};

[[nodiscard]] ReasonClass reason_class(ReasonCode code) noexcept;
[[nodiscard]] bool is_acceptance(ReasonCode code) noexcept;
[[nodiscard]] bool is_refusal(ReasonCode code) noexcept;

/// Canonical lowercase snake_case name. Stable across releases.
[[nodiscard]] std::string_view reason_name(ReasonCode code) noexcept;

/// Parses a canonical name; returns ReasonCode::Unknown when unrecognised.
[[nodiscard]] ReasonCode reason_from_name(std::string_view name) noexcept;

/// One-line description of the code, for the explanation surface.
[[nodiscard]] std::string_view reason_description(ReasonCode code) noexcept;

/// Highest defined reason code value; used to size counter tables.
inline constexpr std::uint16_t kMaxReasonCodeValue = 907;

/// A status is a reason code plus optional human-readable detail. The detail is
/// never parsed to recover semantics.
class Status {
 public:
  Status() noexcept = default;
  explicit Status(ReasonCode code) noexcept : code_(code) {}
  Status(ReasonCode code, std::string detail) noexcept
      : code_(code), detail_(std::move(detail)) {}

  [[nodiscard]] ReasonCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
  [[nodiscard]] bool ok() const noexcept { return is_acceptance(code_); }

  [[nodiscard]] std::string to_string() const;

 private:
  ReasonCode code_ = ReasonCode::Unknown;
  std::string detail_;
};

[[nodiscard]] inline Status accept(ReasonCode code = ReasonCode::Accepted) noexcept {
  return Status(code);
}

[[nodiscard]] inline Status refuse(ReasonCode code, std::string detail = {}) noexcept {
  return Status(code, std::move(detail));
}

/// Result carries either a value or a refusal. There is no third state and no
/// default-constructed "empty but successful" outcome.
template <class T>
class Result {
 public:
  Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
      : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Status status) noexcept : storage_(std::in_place_index<1>, std::move(status)) {}

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const T& value() const& noexcept { return std::get<0>(storage_); }
  [[nodiscard]] T& value() & noexcept { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && noexcept { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] const Status& status() const noexcept {
    return storage_.index() == 1 ? std::get<1>(storage_) : kOkStatus;
  }
  [[nodiscard]] ReasonCode code() const noexcept { return status().code(); }

  [[nodiscard]] const T* operator->() const noexcept { return &value(); }
  [[nodiscard]] T* operator->() noexcept { return &value(); }
  [[nodiscard]] const T& operator*() const noexcept { return value(); }
  [[nodiscard]] T& operator*() noexcept { return value(); }

 private:
  static inline const Status kOkStatus{ReasonCode::Accepted};
  std::variant<T, Status> storage_;
};

/// Result of an operation with no payload.
template <>
class Result<void> {
 public:
  Result() noexcept : status_(ReasonCode::Accepted) {}
  Result(Status status) noexcept : status_(std::move(status)) {}

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] ReasonCode code() const noexcept { return status_.code(); }

 private:
  Status status_;
};

/// Propagate a refusal out of a function returning Result<U>.
#define SNCF_TRY(expr)                                  \
  do {                                                  \
    auto&& sncf_try_result = (expr);                    \
    if (!sncf_try_result.ok()) {                        \
      return sncf_try_result.status();                  \
    }                                                   \
  } while (false)

}  // namespace sncf

#endif  // SNCF_STATUS_HPP
