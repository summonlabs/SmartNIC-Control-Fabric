// Copyright 2026 Summon Software Labs.
#include "sncf/persistence.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

#include "sncf/canonical.hpp"
#include "sncf/crash.hpp"
#include "sncf/digest.hpp"
#include "sncf/version.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace sncf {
namespace {

constexpr std::array<char, 8> kJournalMagic{'S', 'N', 'C', 'F', 'J', 'R', 'N', '1'};
constexpr std::array<char, 8> kSnapshotMagic{'S', 'N', 'C', 'F', 'S', 'N', 'P', '1'};
constexpr std::size_t kJournalHeaderBytes = 24U;
constexpr std::size_t kSnapshotHeaderBytes = 40U;
constexpr std::uint32_t kMaxFrameBytes = 16U * 1024U * 1024U;

void put_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFFU));
  out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<char>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

void put_u64(std::string& out, std::uint64_t value) {
  for (unsigned i = 0; i < 8U; ++i) {
    out.push_back(static_cast<char>((value >> (i * 8U)) & 0xFFU));
  }
}

std::uint32_t read_u32(const std::string& data, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1U])) << 8U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2U])) << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3U])) << 24U);
}

std::uint64_t read_u64(const std::string& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8U; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[offset + i])) << (i * 8U);
  }
  return value;
}

bool magic_matches(const std::string& data, std::size_t offset, const std::array<char, 8>& magic) {
  if (data.size() < offset + magic.size()) {
    return false;
  }
  return std::memcmp(data.data() + offset, magic.data(), magic.size()) == 0;
}

std::int64_t file_offset(std::FILE* file) noexcept {
#if defined(_WIN32)
  return static_cast<std::int64_t>(_ftelli64(file));
#else
  return static_cast<std::int64_t(::ftello(file));
#endif
}

bool seek_to(std::FILE* file, std::int64_t offset) noexcept {
#if defined(_WIN32)
  return _fseeki64(file, offset, SEEK_SET) == 0;
#else
  return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool truncate_to(std::FILE* file, std::int64_t offset) noexcept {
#if defined(_WIN32)
  return _chsize_s(_fileno(file), offset) == 0;
#else
  return ::ftruncate(::fileno(file), static_cast<off_t>(offset)) == 0;
#endif
}

Result<void> sync_file(std::FILE* file) noexcept {
  if (file == nullptr) {
    return Status(ReasonCode::RefusedJournalUnavailable, "no open file to synchronise");
  }
  if (std::fflush(file) != 0) {
    return Status(ReasonCode::RefusedCommitFailed, "buffered write could not be flushed");
  }
#if defined(_WIN32)
  if (_commit(_fileno(file)) != 0) {
    return Status(ReasonCode::RefusedCommitFailed, "commit to stable storage failed");
  }
#else
  if (::fsync(::fileno(file)) != 0) {
    return Status(ReasonCode::RefusedCommitFailed, "fsync to stable storage failed");
  }
#endif
  return {};
}

Result<std::string> read_whole_file(const std::filesystem::path& path, std::uint64_t limit) noexcept {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Status(ReasonCode::RefusedJournalUnavailable, "file could not be opened");
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size < 0) {
    return Status(ReasonCode::RefusedJournalUnavailable, "file size could not be determined");
  }
  if (static_cast<std::uint64_t>(size) > limit) {
    return Status(ReasonCode::RefusedOversizedInput, "file exceeds the configured bound");
  }
  std::string data;
  data.resize(static_cast<std::size_t>(size));
  stream.seekg(0, std::ios::beg);
  if (size > 0) {
    stream.read(data.data(), size);
    if (stream.gcount() != size) {
      return Status(ReasonCode::RefusedTruncatedInput, "file could not be read completely");
    }
  }
  return data;
}

Result<void> write_file_atomic_prepare(const std::filesystem::path& path, const std::string& bytes) noexcept {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return Status(ReasonCode::RefusedJournalUnavailable, "temporary file could not be created");
  }
  const std::size_t written = bytes.empty() ? 0U : std::fwrite(bytes.data(), 1U, bytes.size(), file);
  const bool ok = written == bytes.size();
  const Status sync_status = sync_file(file).status();
  std::fclose(file);
  if (!ok) {
    return Status(ReasonCode::RefusedCommitFailed, "temporary file write was short");
  }
  if (!sync_status.ok()) {
    return sync_status;
  }
  return {};
}

Result<void> replace_file(const std::filesystem::path& from, const std::filesystem::path& to) noexcept {
  // The rename replaces the destination itself. Removing the destination first
  // would open a window in which neither copy exists.
  std::error_code error;
  std::filesystem::rename(from, to, error);
  if (error) {
    return Status(ReasonCode::RefusedJournalUnavailable, "atomic replace failed: " + error.message());
  }
  return {};
}

}  // namespace

std::filesystem::path journal_path(const std::filesystem::path& directory) {
  return directory / "journal.sncf";
}

std::filesystem::path snapshot_path(const std::filesystem::path& directory) {
  return directory / "snapshot.sncf";
}

std::string_view record_kind_name(RecordKind kind) noexcept {
  switch (kind) {
    case RecordKind::Unknown:
      return "unknown";
    case RecordKind::EpochAdvanced:
      return "epoch_advanced";
    case RecordKind::SmartNicRegistered:
      return "smartnic_registered";
    case RecordKind::DeviceRegistered:
      return "device_registered";
    case RecordKind::PackageRegistered:
      return "package_registered";
    case RecordKind::CapabilityEvidenceAccepted:
      return "capability_evidence_accepted";
    case RecordKind::ObservationAccepted:
      return "observation_accepted";
    case RecordKind::EvidenceReverified:
      return "evidence_reverified";
    case RecordKind::AuthorityGranted:
      return "authority_granted";
    case RecordKind::AuthorityRevoked:
      return "authority_revoked";
    case RecordKind::AttemptDispatched:
      return "attempt_dispatched";
    case RecordKind::AttemptSettled:
      return "attempt_settled";
    case RecordKind::InstanceCreated:
      return "instance_created";
    case RecordKind::InstanceReplaced:
      return "instance_replaced";
    case RecordKind::IntentRecorded:
      return "intent_recorded";
    case RecordKind::LifecycleChanged:
      return "lifecycle_changed";
    case RecordKind::FencingAdvanced:
      return "fencing_advanced";
    case RecordKind::RecoverySupersededAuthority:
      return "recovery_superseded_authority";
    case RecordKind::RecoveryAmbiguousAttempt:
      return "recovery_ambiguous_attempt";
    case RecordKind::RecoveryEvidencePendingReverification:
      return "recovery_evidence_pending_reverification";
    case RecordKind::RecoveryObservationInvalidated:
      return "recovery_observation_invalidated";
  }
  return "unknown";
}

RecordKind record_kind_from_name(std::string_view name) noexcept {
  for (std::uint16_t raw = 0; raw <= 100U; ++raw) {
    const auto kind = static_cast<RecordKind>(raw);
    if (record_kind_name(kind) == name && kind != RecordKind::Unknown) {
      return kind;
    }
  }
  return RecordKind::Unknown;
}

std::string_view recovery_disposition_name(RecoveryDisposition disposition) noexcept {
  switch (disposition) {
    case RecoveryDisposition::FreshStore:
      return "fresh_store";
    case RecoveryDisposition::CleanReopen:
      return "clean_reopen";
    case RecoveryDisposition::TornTailRecovered:
      return "torn_tail_recovered";
    case RecoveryDisposition::CorruptTailDiscarded:
      return "corrupt_tail_discarded";
    case RecoveryDisposition::SnapshotRejected:
      return "snapshot_rejected";
    case RecoveryDisposition::IncompatibleVersionRefused:
      return "incompatible_version_refused";
    case RecoveryDisposition::SemanticsMismatchRefused:
      return "semantics_mismatch_refused";
    case RecoveryDisposition::CorruptRefused:
      return "corrupt_refused";
    case RecoveryDisposition::SequenceGapRefused:
      return "sequence_gap_refused";
    case RecoveryDisposition::TruncatedRefused:
      return "truncated_refused";
    case RecoveryDisposition::HistoryIncompleteRefused:
      return "history_incomplete_refused";
    case RecoveryDisposition::AppendRollbackFailed:
      return "append_rollback_failed";
  }
  return "unknown";
}

bool recovery_is_usable(RecoveryDisposition disposition) noexcept {
  switch (disposition) {
    case RecoveryDisposition::FreshStore:
    case RecoveryDisposition::CleanReopen:
    case RecoveryDisposition::TornTailRecovered:
    case RecoveryDisposition::CorruptTailDiscarded:
    case RecoveryDisposition::SnapshotRejected:
      return true;
    case RecoveryDisposition::IncompatibleVersionRefused:
    case RecoveryDisposition::SemanticsMismatchRefused:
    case RecoveryDisposition::CorruptRefused:
    case RecoveryDisposition::SequenceGapRefused:
    case RecoveryDisposition::TruncatedRefused:
    case RecoveryDisposition::HistoryIncompleteRefused:
    case RecoveryDisposition::AppendRollbackFailed:
      return false;
  }
  return false;
}

Value RecoveryReport::to_value() const {
  return Value::object({
      {"bytes_dropped", Value::uint_value(bytes_dropped)},
      {"detail", Value::string(detail)},
      {"disposition", Value::string(std::string(recovery_disposition_name(disposition)))},
      {"reason", Value::string(std::string(reason_name(reason)))},
      {"records_dropped", Value::uint_value(records_dropped)},
      {"records_recovered", Value::uint_value(records_recovered)},
      {"resumed_from_snapshot", Value::boolean(resumed_from_snapshot)},
      {"snapshot_sequence", Value::uint_value(snapshot_sequence)},
      {"usable", Value::boolean(recovery_is_usable(disposition))},
  });
}

Value JournalRecord::to_value() const {
  return Value::object({
      {"at", Value::uint_value(at.value())},
      {"body", body},
      {"command", Value::uint_value(command.value())},
      {"epoch", Value::uint_value(epoch.value())},
      {"kind", Value::string(std::string(record_kind_name(kind)))},
      {"outcome", Value::string(std::string(reason_name(outcome)))},
      {"principal", Value::uint_value(principal.value())},
      {"seq", Value::uint_value(sequence.value())},
      {"subject", Value::string(subject)},
  });
}

Result<JournalRecord> JournalRecord::from_value(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "journal record must be an object");
  }
  JournalRecord record;
  auto sequence = field_uint(value, "seq");
  SNCF_TRY(sequence);
  if (sequence.value() == 0U) {
    return Status(ReasonCode::RefusedNilIdentity, "journal sequence must be non-zero");
  }
  record.sequence = RecordSequence::from_value(sequence.value());
  auto kind = field_string(value, "kind");
  SNCF_TRY(kind);
  record.kind = record_kind_from_name(kind.value());
  if (record.kind == RecordKind::Unknown) {
    return Status(ReasonCode::RefusedInvalidEnumValue, "journal record kind is not known");
  }
  auto command = field_uint(value, "command");
  SNCF_TRY(command);
  record.command = CommandId::from_value(command.value());
  auto principal = field_uint(value, "principal");
  SNCF_TRY(principal);
  record.principal = PrincipalId::from_value(principal.value());
  auto epoch = field_uint(value, "epoch");
  SNCF_TRY(epoch);
  record.epoch = CoordinatorEpoch::from_value(epoch.value());
  auto at = field_uint(value, "at");
  SNCF_TRY(at);
  record.at = TimestampNs::from_value(at.value());
  auto outcome = field_string(value, "outcome");
  SNCF_TRY(outcome);
  record.outcome = reason_from_name(outcome.value());
  if (record.outcome == ReasonCode::Unknown && outcome.value() != "unknown") {
    return Status(ReasonCode::RefusedInvalidEnumValue, "journal record outcome is not known");
  }
  auto subject = field_string(value, "subject");
  SNCF_TRY(subject);
  record.subject = subject.value();
  auto body = field_value(value, "body");
  SNCF_TRY(body);
  if (!body.value()->is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "journal record body must be an object");
  }
  record.body = *body.value();
  return record;
}

Journal::~Journal() { close(); }

Result<std::unique_ptr<Journal>> Journal::open(Options options) noexcept {
  if (options.directory.empty()) {
    return Status(ReasonCode::RefusedJournalUnavailable, "store directory must be named");
  }
  if (options.max_record_bytes == 0U || options.max_record_bytes > kMaxFrameBytes) {
    return Status(ReasonCode::RefusedInvalidRange, "max_record_bytes is outside the supported range");
  }
  auto journal = std::unique_ptr<Journal>(new Journal());
  journal->options_ = std::move(options);

  std::error_code error;
  std::filesystem::create_directories(journal->options_.directory, error);
  if (error) {
    return Status(ReasonCode::RefusedJournalUnavailable, "store directory could not be created");
  }
  const auto journal_file = journal_path(journal->options_.directory);
  const auto snapshot_file = snapshot_path(journal->options_.directory);
  const auto journal_temp = journal->options_.directory / "journal.tmp";
  const auto snapshot_temp = journal->options_.directory / "snapshot.tmp";
  std::filesystem::remove(journal_temp, error);
  error.clear();
  std::filesystem::remove(snapshot_temp, error);
  error.clear();

  RecordSequence first_sequence = RecordSequence::from_value(1U);
  bool have_snapshot = std::filesystem::exists(snapshot_file, error) && !error;
  error.clear();
  std::uint64_t snapshot_sequence_value = 0;
  if (have_snapshot) {
    auto bytes = read_whole_file(snapshot_file, 64U * 1024U * 1024U);
    if (bytes.ok() && bytes.value().size() >= kSnapshotHeaderBytes &&
        magic_matches(bytes.value(), 0, kSnapshotMagic)) {
      snapshot_sequence_value = read_u64(bytes.value(), 16U);
      first_sequence = RecordSequence::from_value(snapshot_sequence_value + 1U);
    }
  }

  const bool have_journal = std::filesystem::exists(journal_file, error) && !error;
  error.clear();

  if (!have_journal) {
    journal->report_.disposition = have_snapshot ? RecoveryDisposition::CleanReopen : RecoveryDisposition::FreshStore;
    journal->report_.snapshot_sequence = snapshot_sequence_value;
    journal->next_sequence_ = first_sequence;
    journal->first_sequence_ = first_sequence;
    journal->report_.detail = have_snapshot ? "journal absent; state comes from the snapshot"
                                            : "store created";
  } else {
    auto bytes = read_whole_file(journal_file, journal->options_.max_journal_bytes + kMaxFrameBytes + 1024U);
    if (!bytes.ok()) {
      journal->report_.disposition = RecoveryDisposition::CorruptRefused;
      journal->report_.reason = bytes.status().code();
      journal->report_.detail = bytes.status().detail();
      return journal;
    }
    const std::string& data = bytes.value();
    if (data.size() < kJournalHeaderBytes) {
      journal->report_.disposition = RecoveryDisposition::TruncatedRefused;
      journal->report_.reason = ReasonCode::RefusedTruncatedInput;
      journal->report_.detail = "journal header is incomplete";
      return journal;
    }
    if (!magic_matches(data, 0, kJournalMagic)) {
      journal->report_.disposition = RecoveryDisposition::CorruptRefused;
      journal->report_.reason = ReasonCode::RefusedJournalCorrupt;
      journal->report_.detail = "journal magic does not match";
      return journal;
    }
    const std::uint32_t format_version = read_u32(data, 8U);
    const std::uint32_t semantics = read_u32(data, 12U);
    if (format_version != kStoreFormatVersion) {
      journal->report_.disposition = RecoveryDisposition::IncompatibleVersionRefused;
      journal->report_.reason = ReasonCode::RefusedIncompatibleStoreVersion;
      journal->report_.detail = "journal format version is not supported";
      return journal;
    }
    if (semantics != kSemanticsRevision) {
      journal->report_.disposition = RecoveryDisposition::SemanticsMismatchRefused;
      journal->report_.reason = ReasonCode::RefusedSemanticsMismatch;
      journal->report_.detail = "journal was written under different semantics";
      return journal;
    }
    const std::uint64_t header_first = read_u64(data, 16U);
    journal->first_sequence_ = RecordSequence::from_value(header_first);
    if (header_first == 0U) {
      journal->report_.disposition = RecoveryDisposition::CorruptRefused;
      journal->report_.reason = ReasonCode::RefusedJournalCorrupt;
      journal->report_.detail = "journal first sequence is zero";
      return journal;
    }
    // The journal either starts at the beginning of history, or continues
    // exactly where a snapshot that is present on disk ends. Anything else means
    // a prefix of history is missing, and replaying the remainder would silently
    // serve a partial state.
    const bool starts_history = header_first == 1U;
    const bool continues_snapshot =
        snapshot_sequence_value > 0U && header_first == snapshot_sequence_value + 1U;
    if (!starts_history && !continues_snapshot) {
      journal->report_.disposition = RecoveryDisposition::HistoryIncompleteRefused;
      journal->report_.reason = ReasonCode::RefusedSequenceGap;
      journal->report_.detail =
          "the journal does not start at the beginning of history and no snapshot covers the missing prefix";
      return journal;
    }

    std::size_t offset = kJournalHeaderBytes;
    std::size_t valid_end = offset;
    RecordSequence expected = RecordSequence::from_value(header_first);
    bool torn = false;
    bool corrupt_tail = false;
    Status failure{ReasonCode::Accepted};

    while (offset < data.size()) {
      if (data.size() - offset < 8U) {
        torn = true;
        break;
      }
      const std::uint32_t length = read_u32(data, offset);
      const std::uint32_t crc = read_u32(data, offset + 4U);
      const bool last_frame = (offset + 8U + length) == data.size();
      if (length == 0U || length > journal->options_.max_record_bytes) {
        if (last_frame) {
          torn = true;
        } else {
          failure = Status(ReasonCode::RefusedJournalCorrupt, "record length is outside the supported range");
        }
        break;
      }
      if (data.size() - offset - 8U < length) {
        torn = true;
        break;
      }
      const std::string_view payload(data.data() + offset + 8U, length);
      if (crc32c(payload.data(), payload.size()) != crc) {
        if (last_frame) {
          corrupt_tail = true;
        } else {
          failure = Status(ReasonCode::RefusedJournalCorrupt, "record checksum mismatch mid-journal");
        }
        break;
      }
      auto parsed_json = parse_canonical(payload, ParseLimits{});
      if (!parsed_json.ok()) {
        failure = Status(ReasonCode::RefusedJournalCorrupt, "record payload is not canonical: " +
                                                                std::string(parsed_json.status().detail()));
        break;
      }
      auto record = JournalRecord::from_value(parsed_json.value());
      if (!record.ok()) {
        failure = Status(ReasonCode::RefusedJournalCorrupt,
                         "record payload is not a journal record: " + std::string(record.status().detail()));
        break;
      }
      if (record.value().sequence.value() != expected.value()) {
        if (record.value().sequence.value() < expected.value()) {
          failure = Status(ReasonCode::RefusedReplayedRecord, "journal sequence was replayed");
        } else {
          failure = Status(ReasonCode::RefusedSequenceGap, "journal sequence has a gap");
        }
        break;
      }
      expected = RecordSequence::from_value(expected.value() + 1U);
      journal->recovered_.push_back(std::move(record.value()));
      offset += 8U + length;
      valid_end = offset;
    }

    if (!failure.ok()) {
      journal->report_.disposition = failure.code() == ReasonCode::RefusedSequenceGap ||
                                             failure.code() == ReasonCode::RefusedReplayedRecord
                                         ? RecoveryDisposition::SequenceGapRefused
                                         : RecoveryDisposition::CorruptRefused;
      journal->report_.reason = failure.code();
      journal->report_.detail = failure.detail();
      return journal;
    }

    journal->report_.records_recovered = journal->recovered_.size();
    // Count the whole frames that were discarded with the tail, so a dropped
    // record is never invisible.
    std::uint64_t dropped = 0;
    std::size_t scan = valid_end;
    while (data.size() - scan >= 8U) {
      const std::uint32_t length = read_u32(data, scan);
      if (length == 0U || length > journal->options_.max_record_bytes) {
        break;
      }
      if (data.size() - scan - 8U < length) {
        break;
      }
      ++dropped;
      scan += 8U + length;
    }
    journal->report_.records_dropped = dropped;
    journal->report_.bytes_dropped = data.size() - valid_end;
    if (torn) {
      journal->report_.disposition = RecoveryDisposition::TornTailRecovered;
      journal->report_.detail = "a partial trailing record was discarded";
    } else if (corrupt_tail) {
      journal->report_.disposition = RecoveryDisposition::CorruptTailDiscarded;
      journal->report_.detail = "a trailing record failed its checksum and was discarded";
    } else {
      journal->report_.disposition = RecoveryDisposition::CleanReopen;
      journal->report_.detail = "store reopened cleanly";
    }
    if (journal->report_.bytes_dropped > 0U) {
      std::error_code resize_error;
      std::filesystem::resize_file(journal_file, valid_end, resize_error);
      if (resize_error) {
        journal->report_.disposition = RecoveryDisposition::CorruptRefused;
        journal->report_.reason = ReasonCode::RefusedJournalCorrupt;
        journal->report_.detail = "recoverable tail could not be truncated";
        return journal;
      }
    }
    journal->record_count_ = journal->recovered_.size();
    journal->bytes_ = valid_end - kJournalHeaderBytes;
    journal->next_sequence_ = expected;
    journal->report_.snapshot_sequence = snapshot_sequence_value;
  }

  journal->header_bytes_ = kJournalHeaderBytes;
  auto stream = journal->open_append_stream();
  if (!stream.ok()) {
    journal->report_.disposition = RecoveryDisposition::CorruptRefused;
    journal->report_.reason = stream.code();
    journal->report_.detail = stream.status().detail();
    return journal;
  }
  // Discard records the snapshot already contains. Compaction is allowed to
  // leave them behind when a crash interrupts the journal rewrite.
  if (snapshot_sequence_value > 0U) {
    const auto first_kept = std::remove_if(
        journal->recovered_.begin(), journal->recovered_.end(),
        [snapshot_sequence_value](const JournalRecord& record) {
          return record.sequence.value() <= snapshot_sequence_value;
        });
    journal->recovered_.erase(first_kept, journal->recovered_.end());
    journal->report_.records_recovered = journal->recovered_.size();
  }
  return journal;
}

Result<void> Journal::open_append_stream() noexcept {
  if (options_.read_only) {
    return {};
  }
  const auto path = journal_path(options_.directory);
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error) && !error;
  error.clear();
  if (!exists) {
    std::string header;
    header.append(kJournalMagic.data(), kJournalMagic.size());
    put_u32(header, kStoreFormatVersion);
    put_u32(header, kSemanticsRevision);
    put_u64(header, next_sequence_.value());
    auto written = write_file_atomic_prepare(path, header);
    if (!written.ok()) {
      return written.status();
    }
    bytes_ = 0;
  }
  file_ = std::fopen(path.string().c_str(), "r+b");
  if (file_ == nullptr) {
    return Status(ReasonCode::RefusedJournalUnavailable, "journal could not be opened for append");
  }
  if (std::fseek(file_, 0, SEEK_END) != 0) {
    std::fclose(file_);
    file_ = nullptr;
    return Status(ReasonCode::RefusedJournalUnavailable, "journal could not be positioned for append");
  }
  return {};
}

Result<void> Journal::append(const Value& payload) noexcept {
  if (options_.read_only) {
    return Status(ReasonCode::RefusedReadOnlyStore, "the store was opened read-only");
  }
  if (file_ == nullptr) {
    return Status(ReasonCode::RefusedJournalUnavailable, "journal is not open");
  }
  if (unusable_) {
    return Status(ReasonCode::RefusedJournalUnavailable,
                  "the journal was left unusable by an earlier failed append");
  }
  std::string body = payload.to_canonical();
  if (body.size() > options_.max_record_bytes) {
    return Status(ReasonCode::RefusedOversizedInput, "record exceeds max_record_bytes");
  }
  if (needs_compaction()) {
    return Status(ReasonCode::RefusedStoreFull, "journal reached its configured bound before compaction");
  }
  std::string frame;
  frame.reserve(body.size() + 8U);
  put_u32(frame, static_cast<std::uint32_t>(body.size()));
  put_u32(frame, crc32c(body.data(), body.size()));
  frame.append(body);

  // Remember exactly where the record starts. Every failure below this point
  // must leave the file exactly as it was, otherwise a later record reusing the
  // same sequence would be read as a replayed sequence, or a torn frame would
  // strand later records behind mid-file damage.
  const std::int64_t start_offset = current_offset();
  if (start_offset < 0) {
    unusable_ = true;
    return Status(ReasonCode::RefusedJournalUnavailable, "the journal position could not be read");
  }
  crash_if_requested(CrashPoint::JournalBeforeWrite);
  if (consume_append_failure()) {
    // Deterministic QA injection: a partial write followed by a failure, which
    // is exactly the case the rollback below exists to handle.
    const std::size_t partial = frame.size() / 2U;
    (void)std::fwrite(frame.data(), 1U, partial, file_);
    (void)std::fflush(file_);
    (void)rollback_to(start_offset);
    return Status(ReasonCode::RefusedCommitFailed, "injected append failure");
  }
  if (std::fwrite(frame.data(), 1U, frame.size(), file_) != frame.size()) {
    (void)rollback_to(start_offset);
    return Status(ReasonCode::RefusedCommitFailed, "journal write was short");
  }
  // Push the frame out of the C runtime buffer so that this boundary is a real
  // durability boundary: the bytes reached the operating system but are not yet
  // guaranteed to survive a machine failure.
  if (std::fflush(file_) != 0) {
    (void)rollback_to(start_offset);
    return Status(ReasonCode::RefusedCommitFailed, "journal flush failed");
  }
  crash_if_requested(CrashPoint::JournalAfterWriteBeforeSync);
  if (options_.sync_on_commit) {
    auto synced = sync();
    if (!synced.ok()) {
      (void)rollback_to(start_offset);
      return synced.status();
    }
  }
  crash_if_requested(CrashPoint::JournalAfterSyncBeforeAck);

  bytes_ += frame.size();
  ++record_count_;
  next_sequence_ = RecordSequence::from_value(next_sequence_.value() + 1U);
  return {};
}

Result<void> Journal::sync() noexcept { return sync_file(file_); }

std::int64_t Journal::current_offset() noexcept {
  if (file_ == nullptr) {
    return -1;
  }
  return file_offset(file_);
}

bool Journal::rollback_to(std::int64_t offset) noexcept {
  if (file_ == nullptr || offset < 0) {
    unusable_ = true;
    return false;
  }
  // Discard buffered output first: the bytes must not be flushed later, after
  // the file has been shortened.
  if (std::fflush(file_) != 0) {
    unusable_ = true;
    return false;
  }
  if (!seek_to(file_, offset) || !truncate_to(file_, offset)) {
    unusable_ = true;
    return false;
  }
  if (!seek_to(file_, offset)) {
    unusable_ = true;
    return false;
  }
  return true;
}

bool Journal::needs_compaction() const noexcept {
  return record_count_ >= options_.max_records_between_compactions || bytes_ >= options_.max_journal_bytes;
}

Result<void> Journal::rewrite(RecordSequence first_sequence) noexcept {
  if (options_.read_only) {
    return Status(ReasonCode::RefusedReadOnlyStore, "the store was opened read-only");
  }
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
  std::string header;
  header.append(kJournalMagic.data(), kJournalMagic.size());
  put_u32(header, kStoreFormatVersion);
  put_u32(header, kSemanticsRevision);
  put_u64(header, first_sequence.value());

  const auto journal_file = journal_path(options_.directory);
  const auto journal_temp = options_.directory / "journal.tmp";
  auto prepared = write_file_atomic_prepare(journal_temp, header);
  if (!prepared.ok()) {
    return prepared.status();
  }
  auto replaced = replace_file(journal_temp, journal_file);
  if (!replaced.ok()) {
    return replaced.status();
  }
  next_sequence_ = first_sequence;
  first_sequence_ = first_sequence;
  record_count_ = 0;
  bytes_ = 0;
  return open_append_stream();
}

Result<void> Journal::write_snapshot(const Value& document, RecordSequence sequence) noexcept {
  if (options_.read_only) {
    return Status(ReasonCode::RefusedReadOnlyStore, "the store was opened read-only");
  }
  std::string payload = document.to_canonical();
  std::string bytes;
  bytes.reserve(payload.size() + kSnapshotHeaderBytes);
  bytes.append(kSnapshotMagic.data(), kSnapshotMagic.size());
  put_u32(bytes, kStoreFormatVersion);
  put_u32(bytes, kSemanticsRevision);
  put_u64(bytes, sequence.value());
  put_u64(bytes, payload.size());
  put_u32(bytes, crc32c(payload.data(), payload.size()));
  put_u32(bytes, 0U);
  bytes.append(payload);

  const auto snapshot_file = snapshot_path(options_.directory);
  const auto snapshot_temp = options_.directory / "snapshot.tmp";
  auto prepared = write_file_atomic_prepare(snapshot_temp, bytes);
  if (!prepared.ok()) {
    return prepared.status();
  }
  crash_if_requested(CrashPoint::SnapshotBeforeRename);
  auto replaced = replace_file(snapshot_temp, snapshot_file);
  if (!replaced.ok()) {
    return replaced.status();
  }
  crash_if_requested(CrashPoint::SnapshotAfterRenameBeforeJournalRewrite);
  auto rewritten = rewrite(RecordSequence::from_value(sequence.value() + 1U));
  if (!rewritten.ok()) {
    return rewritten.status();
  }
  return {};
}

Result<Value> Journal::load_snapshot() const noexcept {
  const auto snapshot_file = snapshot_path(options_.directory);
  std::error_code error;
  if (!std::filesystem::exists(snapshot_file, error) || error) {
    return Status(ReasonCode::RefusedSnapshotCorrupt, "snapshot is absent");
  }
  auto bytes = read_whole_file(snapshot_file, 64U * 1024U * 1024U);
  if (!bytes.ok()) {
    return bytes.status();
  }
  const std::string& data = bytes.value();
  if (data.size() < kSnapshotHeaderBytes) {
    return Status(ReasonCode::RefusedTruncatedInput, "snapshot header is incomplete");
  }
  if (!magic_matches(data, 0, kSnapshotMagic)) {
    return Status(ReasonCode::RefusedSnapshotCorrupt, "snapshot magic does not match");
  }
  if (read_u32(data, 8U) != kStoreFormatVersion) {
    return Status(ReasonCode::RefusedIncompatibleStoreVersion, "snapshot format version is not supported");
  }
  if (read_u32(data, 12U) != kSemanticsRevision) {
    return Status(ReasonCode::RefusedSemanticsMismatch, "snapshot was written under different semantics");
  }
  const std::uint64_t length = read_u64(data, 24U);
  const std::uint32_t crc = read_u32(data, 32U);
  if (length != data.size() - kSnapshotHeaderBytes) {
    return Status(ReasonCode::RefusedTruncatedInput, "snapshot payload length does not match the file size");
  }
  const std::string_view payload(data.data() + kSnapshotHeaderBytes, static_cast<std::size_t>(length));
  if (crc32c(payload.data(), payload.size()) != crc) {
    return Status(ReasonCode::RefusedSnapshotCorrupt, "snapshot payload failed its checksum");
  }
  return parse_canonical(payload, ParseLimits{});
}

void Journal::close() noexcept {
  if (file_ != nullptr) {
    crash_if_requested(CrashPoint::ShutdownBeforeClose);
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
}

}  // namespace sncf
