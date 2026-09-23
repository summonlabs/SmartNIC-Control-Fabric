// Versioned, integrity-checked durable store: framed journal plus snapshots.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_PERSISTENCE_HPP
#define SNCF_PERSISTENCE_HPP

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sncf/ids.hpp"
#include "sncf/status.hpp"

namespace sncf {

/// The kinds of accepted fact that can appear in the journal. A record always
/// carries a complete replacement for the entity it names, so replay is a pure
/// sequence of idempotent upserts and can never diverge from the live path.
enum class RecordKind : std::uint16_t {
  Unknown = 0,
  EpochAdvanced = 1,
  SmartNicRegistered = 10,
  DeviceRegistered = 11,
  PackageRegistered = 12,
  CapabilityEvidenceAccepted = 20,
  ObservationAccepted = 21,
  EvidenceReverified = 22,
  AuthorityGranted = 30,
  AuthorityRevoked = 31,
  AttemptDispatched = 40,
  AttemptSettled = 42,
  InstanceCreated = 50,
  InstanceReplaced = 51,
  IntentRecorded = 52,
  LifecycleChanged = 53,
  FencingAdvanced = 60,
  RecoverySupersededAuthority = 70,
  RecoveryAmbiguousAttempt = 71,
  RecoveryEvidencePendingReverification = 72,
  RecoveryObservationInvalidated = 73,
};

[[nodiscard]] std::string_view record_kind_name(RecordKind kind) noexcept;
[[nodiscard]] RecordKind record_kind_from_name(std::string_view name) noexcept;

struct JournalRecord {
  RecordSequence sequence;
  RecordKind kind = RecordKind::Unknown;
  CommandId command;
  PrincipalId principal;
  CoordinatorEpoch epoch;
  TimestampNs at;
  /// The reason the command was accepted. Replayed so that a redelivered
  /// command returns the same answer it originally received.
  ReasonCode outcome = ReasonCode::Accepted;
  /// Canonical subject key of the affected entity, e.g. "instance:7".
  std::string subject;
  /// Kind-specific body. Already canonical when decoded.
  Value body;

  [[nodiscard]] Value to_value() const;
  [[nodiscard]] static Result<JournalRecord> from_value(const Value& value) noexcept;
};

/// What recovery found when it opened the store. Every disposition is explicit;
/// nothing is inferred from silence.
enum class RecoveryDisposition : std::uint8_t {
  FreshStore = 0,
  CleanReopen = 1,
  TornTailRecovered = 2,
  CorruptTailDiscarded = 3,
  SnapshotRejected = 4,
  IncompatibleVersionRefused = 5,
  SemanticsMismatchRefused = 6,
  CorruptRefused = 7,
  SequenceGapRefused = 8,
  TruncatedRefused = 9,
  HistoryIncompleteRefused = 10,
  AppendRollbackFailed = 11,
};

[[nodiscard]] std::string_view recovery_disposition_name(RecoveryDisposition disposition) noexcept;
[[nodiscard]] bool recovery_is_usable(RecoveryDisposition disposition) noexcept;

struct RecoveryReport {
  RecoveryDisposition disposition = RecoveryDisposition::FreshStore;
  ReasonCode reason = ReasonCode::Accepted;
  std::string detail;
  std::uint64_t records_recovered = 0;
  std::uint64_t records_dropped = 0;
  std::uint64_t bytes_dropped = 0;
  std::uint64_t snapshot_sequence = 0;
  bool resumed_from_snapshot = false;

  [[nodiscard]] Value to_value() const;
};

/// Append-only framed journal. Frame layout:
///   u32 little-endian payload length | u32 little-endian CRC-32C | payload
/// File header:
///   "SNCFJRN1" | u32 format version | u32 semantics revision | u64 first sequence
class Journal {
 public:
  struct Options {
    std::filesystem::path directory;
    std::size_t max_record_bytes = 256U * 1024U;
    std::size_t max_records_between_compactions = 4096;
    std::uint64_t max_journal_bytes = 8U * 1024U * 1024U;
    bool sync_on_commit = true;
    /// Opens the store for reading only: no append stream is created and every
    /// write is refused, so an inspection can never alter what it inspects.
    bool read_only = false;
  };

  /// Opens the store, validating every frame. A recoverable tail is truncated;
  /// damage anywhere else is refused and the caller must not use the store.
  [[nodiscard]] static Result<std::unique_ptr<Journal>> open(Options options) noexcept;

  ~Journal();
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  /// Appends one record. The payload is framed, written and (by default)
  /// flushed to stable storage before the call reports success.
  [[nodiscard]] Result<void> append(const Value& payload) noexcept;

  /// Replaces the journal with an empty one whose first sequence is
  /// first_sequence. Used by compaction after the snapshot is durable.
  [[nodiscard]] Result<void> rewrite(RecordSequence first_sequence) noexcept;

  /// Writes a snapshot document and rewrites the journal in one operation.
  [[nodiscard]] Result<void> write_snapshot(const Value& document, RecordSequence sequence) noexcept;

  [[nodiscard]] const std::vector<JournalRecord>& recovered() const noexcept { return recovered_; }
  [[nodiscard]] const RecoveryReport& report() const noexcept { return report_; }
  [[nodiscard]] std::uint64_t record_count() const noexcept { return record_count_; }
  [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] RecordSequence next_sequence() const noexcept { return next_sequence_; }
  [[nodiscard]] RecordSequence first_sequence() const noexcept { return first_sequence_; }
  [[nodiscard]] bool needs_compaction() const noexcept;
  [[nodiscard]] Result<Value> load_snapshot() const noexcept;

  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }

 private:
  Journal() = default;

  [[nodiscard]] Result<void> open_append_stream() noexcept;
  [[nodiscard]] Result<void> sync() noexcept;

  /// Byte offset of the append position, or -1 when it cannot be read.
  [[nodiscard]] std::int64_t current_offset() noexcept;

  /// Restores the file to a pre-write length after a failed append. When the
  /// rollback itself fails the journal is marked unusable, so no later append
  /// can extend a file whose contents are no longer trustworthy.
  [[nodiscard]] bool rollback_to(std::int64_t offset) noexcept;

  Options options_{};
  std::FILE* file_ = nullptr;
  RecordSequence next_sequence_;
  RecordSequence first_sequence_;
  std::uint64_t record_count_ = 0;
  std::uint64_t bytes_ = 0;
  std::uint64_t header_bytes_ = 0;
  bool unusable_ = false;
  std::vector<JournalRecord> recovered_;
  RecoveryReport report_;
};

[[nodiscard]] std::filesystem::path journal_path(const std::filesystem::path& directory);
[[nodiscard]] std::filesystem::path snapshot_path(const std::filesystem::path& directory);

}  // namespace sncf

#endif  // SNCF_PERSISTENCE_HPP
