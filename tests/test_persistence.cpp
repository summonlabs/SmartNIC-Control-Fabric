// Copyright 2026 Summon Software Labs.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "sncf/persistence.hpp"
#include "sncf/version.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace sncf;
using sncf_test::TempDir;

Value make_record(std::uint64_t sequence, std::uint64_t smartnic_id = 1) {
  SmartNicRecord record;
  record.id = SmartNicId::from_value(smartnic_id);
  record.label = "nic-a";
  record.port_count = 4;
  record.registered_at = TimestampNs::from_value(1000U + sequence);
  JournalRecord journal;
  journal.sequence = RecordSequence::from_value(sequence);
  journal.kind = RecordKind::SmartNicRegistered;
  journal.command = CommandId::from_value(sequence);
  journal.principal = PrincipalId::from_value(1);
  journal.epoch = CoordinatorEpoch::from_value(1);
  journal.at = TimestampNs::from_value(1000U + sequence);
  journal.outcome = ReasonCode::Accepted;
  journal.subject = "smartnic:" + std::to_string(smartnic_id);
  journal.body = Value::object({{"smartnic", smartnic_to_value(record)}});
  return journal.to_value();
}

std::unique_ptr<Journal> open_journal(const std::filesystem::path& directory,
                                      std::size_t max_records = 4096) {
  Journal::Options options;
  options.directory = directory;
  options.max_records_between_compactions = max_records;
  options.max_journal_bytes = 8U * 1024U * 1024U;
  options.sync_on_commit = false;
  auto journal = Journal::open(options);
  SNCF_REQUIRE(journal.ok());
  return std::move(journal.value());
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  return data;
}

void write_file(const std::filesystem::path& path, const std::string& data) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(data.data(), static_cast<std::streamsize>(data.size()));
}

SNCF_TEST(persistence_fresh_open_and_clean_reopen) {
  TempDir directory;
  {
    auto journal = open_journal(directory.path());
    SNCF_CHECK(journal->report().disposition == RecoveryDisposition::FreshStore);
    SNCF_CHECK(journal->recovered().empty());
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
      auto appended = journal->append(make_record(sequence));
      SNCF_REQUIRE(appended.ok());
    }
    SNCF_CHECK_EQ(journal->record_count(), 3U);
    SNCF_CHECK_EQ(journal->next_sequence().value(), 4U);
  }
  {
    auto journal = open_journal(directory.path());
    SNCF_CHECK(journal->report().disposition == RecoveryDisposition::CleanReopen);
    SNCF_CHECK_EQ(journal->recovered().size(), 3U);
    SNCF_CHECK_EQ(journal->recovered()[0].sequence.value(), 1U);
    SNCF_CHECK_EQ(journal->recovered()[2].sequence.value(), 3U);
    SNCF_CHECK_EQ(journal->recovered()[0].kind, RecordKind::SmartNicRegistered);
    SNCF_CHECK_EQ(journal->recovered()[0].subject, std::string("smartnic:1"));
  }
}

SNCF_TEST(persistence_recovers_a_torn_tail) {
  TempDir directory;
  {
    auto journal = open_journal(directory.path());
    SNCF_REQUIRE(journal->append(make_record(1)).ok());
    SNCF_REQUIRE(journal->append(make_record(2)).ok());
    SNCF_REQUIRE(journal->append(make_record(3)).ok());
  }
  const auto path = journal_path(directory.path());
  const auto size = std::filesystem::file_size(path);
  std::filesystem::resize_file(path, size - 5U);
  auto journal = open_journal(directory.path());
  SNCF_CHECK(journal->report().disposition == RecoveryDisposition::TornTailRecovered);
  SNCF_CHECK_EQ(journal->recovered().size(), 2U);
  SNCF_CHECK(journal->report().bytes_dropped > 0U);
  SNCF_CHECK(std::filesystem::file_size(path) < size - 5U);
  // The truncated tail must not come back on the next open.
  journal->close();
  auto reopened = open_journal(directory.path());
  SNCF_CHECK(reopened->report().disposition == RecoveryDisposition::CleanReopen);
  SNCF_CHECK_EQ(reopened->recovered().size(), 2U);
}

SNCF_TEST(persistence_discards_a_corrupt_tail_record) {
  TempDir directory;
  {
    auto journal = open_journal(directory.path());
    SNCF_REQUIRE(journal->append(make_record(1)).ok());
    SNCF_REQUIRE(journal->append(make_record(2)).ok());
  }
  const auto path = journal_path(directory.path());
  std::string data = read_file(path);
  SNCF_REQUIRE(data.size() > 10U);
  data[data.size() - 2U] = static_cast<char>(data[data.size() - 2U] ^ 0x5A);
  write_file(path, data);
  auto journal = open_journal(directory.path());
  SNCF_CHECK(journal->report().disposition == RecoveryDisposition::CorruptTailDiscarded);
  SNCF_CHECK_EQ(journal->recovered().size(), 1U);
}

SNCF_TEST(persistence_refuses_corruption_in_the_middle) {
  TempDir directory;
  {
    auto journal = open_journal(directory.path());
    SNCF_REQUIRE(journal->append(make_record(1)).ok());
    SNCF_REQUIRE(journal->append(make_record(2)).ok());
    SNCF_REQUIRE(journal->append(make_record(3)).ok());
  }
  const auto path = journal_path(directory.path());
  std::string data = read_file(path);
  constexpr std::size_t kHeaderBytes = 24U;  // journal file header length
  data[kHeaderBytes + 40U] = static_cast<char>(data[kHeaderBytes + 40U] ^ 0xFF);
  write_file(path, data);
  auto journal = open_journal(directory.path());
  SNCF_CHECK(journal->report().disposition == RecoveryDisposition::CorruptRefused);
  SNCF_CHECK(!recovery_is_usable(journal->report().disposition));
}

SNCF_TEST(persistence_refuses_wrong_version_and_semantics) {
  TempDir directory;
  {
    auto journal = open_journal(directory.path());
    SNCF_REQUIRE(journal->append(make_record(1)).ok());
  }
  const auto path = journal_path(directory.path());
  const std::string original = read_file(path);

  std::string wrong_version = original;
  wrong_version[8] = static_cast<char>(9);
  write_file(path, wrong_version);
  auto version_journal = open_journal(directory.path());
  SNCF_CHECK(version_journal->report().disposition == RecoveryDisposition::IncompatibleVersionRefused);

  std::string wrong_semantics = original;
  wrong_semantics[12] = static_cast<char>(9);
  write_file(path, wrong_semantics);
  auto semantics_journal = open_journal(directory.path());
  SNCF_CHECK(semantics_journal->report().disposition == RecoveryDisposition::SemanticsMismatchRefused);

  std::string wrong_magic = original;
  wrong_magic[0] = 'X';
  write_file(path, wrong_magic);
  auto magic_journal = open_journal(directory.path());
  SNCF_CHECK(magic_journal->report().disposition == RecoveryDisposition::CorruptRefused);

  write_file(path, original.substr(0, 10));
  auto short_journal = open_journal(directory.path());
  SNCF_CHECK(short_journal->report().disposition == RecoveryDisposition::TruncatedRefused);

  write_file(path, original);
  auto good = open_journal(directory.path());
  SNCF_CHECK(good->report().disposition == RecoveryDisposition::CleanReopen);
}

SNCF_TEST(persistence_refuses_sequence_gaps_and_replays) {
  TempDir directory;
  {
    auto journal = open_journal(directory.path());
    SNCF_REQUIRE(journal->append(make_record(1)).ok());
    SNCF_REQUIRE(journal->append(make_record(3)).ok());
  }
  auto gap = open_journal(directory.path());
  SNCF_CHECK(gap->report().disposition == RecoveryDisposition::SequenceGapRefused);
  SNCF_CHECK(gap->report().reason == ReasonCode::RefusedSequenceGap);
  SNCF_CHECK(!recovery_is_usable(gap->report().disposition));

  TempDir replay_directory;
  {
    auto journal = open_journal(replay_directory.path());
    SNCF_REQUIRE(journal->append(make_record(1)).ok());
    SNCF_REQUIRE(journal->append(make_record(1)).ok());
  }
  auto replayed = open_journal(replay_directory.path());
  SNCF_CHECK(replayed->report().disposition == RecoveryDisposition::SequenceGapRefused);
  SNCF_CHECK(replayed->report().reason == ReasonCode::RefusedReplayedRecord);
}

SNCF_TEST(persistence_snapshot_round_trip_and_compaction) {
  TempDir directory;
  const Value document = Value::object({{"marker", Value::string("state")}});
  {
    auto journal = open_journal(directory.path());
    SNCF_REQUIRE(journal->append(make_record(1)).ok());
    SNCF_REQUIRE(journal->append(make_record(2)).ok());
    auto written = journal->write_snapshot(document, RecordSequence::from_value(2));
    SNCF_REQUIRE(written.ok());
    SNCF_CHECK_EQ(journal->next_sequence().value(), 3U);
    SNCF_CHECK_EQ(journal->record_count(), 0U);
    SNCF_REQUIRE(journal->append(make_record(3)).ok());
  }
  auto journal = open_journal(directory.path());
  SNCF_CHECK(journal->report().snapshot_sequence == 2U);
  auto loaded = journal->load_snapshot();
  SNCF_REQUIRE(loaded.ok());
  SNCF_CHECK(loaded.value() == document);
  SNCF_CHECK_EQ(journal->recovered().size(), 1U);
  SNCF_CHECK_EQ(journal->recovered()[0].sequence.value(), 3U);

  // Damage the snapshot payload: it must be refused, and because the journal no
  // longer covers the full history the caller must not silently continue.
  const auto snapshot = snapshot_path(directory.path());
  std::string bytes = read_file(snapshot);
  bytes[bytes.size() - 2U] = static_cast<char>(bytes[bytes.size() - 2U] ^ 0x11);
  write_file(snapshot, bytes);
  auto damaged = open_journal(directory.path());
  auto refused = damaged->load_snapshot();
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedSnapshotCorrupt);
  SNCF_CHECK(damaged->first_sequence().value() > 1U);
}

SNCF_TEST(persistence_bounds_growth_and_refuses_when_full) {
  TempDir directory;
  auto journal = open_journal(directory.path(), 4U);
  for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
    SNCF_REQUIRE(journal->append(make_record(sequence)).ok());
  }
  SNCF_CHECK(journal->needs_compaction());
  auto refused = journal->append(make_record(5));
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedStoreFull);
  auto written = journal->write_snapshot(Value::object({{"k", Value::uint_value(1)}}),
                                         RecordSequence::from_value(4));
  SNCF_REQUIRE(written.ok());
  SNCF_CHECK(!journal->needs_compaction());
  SNCF_REQUIRE(journal->append(make_record(5)).ok());
}

SNCF_TEST(persistence_rejects_oversized_records) {
  TempDir directory;
  Journal::Options options;
  options.directory = directory.path();
  options.max_record_bytes = 256U;
  options.sync_on_commit = false;
  auto journal = Journal::open(options);
  SNCF_REQUIRE(journal.ok());
  Value large = Value::object({{"blob", Value::string(std::string(1024U, 'x'))}});
  auto refused = journal.value()->append(large);
  SNCF_CHECK(!refused.ok());
  SNCF_CHECK(refused.code() == ReasonCode::RefusedOversizedInput);
}

SNCF_TEST(persistence_record_document_round_trip) {
  const Value document = make_record(7, 42);
  auto parsed = JournalRecord::from_value(document);
  SNCF_REQUIRE(parsed.ok());
  SNCF_CHECK_EQ(parsed.value().sequence.value(), 7U);
  SNCF_CHECK_EQ(parsed.value().subject, std::string("smartnic:42"));
  SNCF_CHECK(parsed.value().to_value() == document);

  auto missing = JournalRecord::from_value(Value::object({{"seq", Value::uint_value(1)}}));
  SNCF_CHECK(!missing.ok());
  SNCF_CHECK(missing.code() == ReasonCode::RefusedMissingField);
  auto unknown_kind = JournalRecord::from_value(Value::object({
      {"at", Value::uint_value(1)},
      {"body", Value::object({})},
      {"command", Value::uint_value(1)},
      {"epoch", Value::uint_value(1)},
      {"kind", Value::string("not-a-kind")},
      {"outcome", Value::string("accepted")},
      {"principal", Value::uint_value(1)},
      {"seq", Value::uint_value(1)},
      {"subject", Value::string("x")},
  }));
  SNCF_CHECK(!unknown_kind.ok());
  SNCF_CHECK(unknown_kind.code() == ReasonCode::RefusedInvalidEnumValue);
}

}  // namespace
