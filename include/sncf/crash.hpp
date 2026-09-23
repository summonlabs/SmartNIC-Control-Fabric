// Crash-boundary injection used by the durability test suite.
//
// When the environment variable SNCF_CRASH_AT names a crash point, the process
// terminates immediately at that boundary without unwinding, flushing streams or
// running destructors. This is how the tests kill the runtime at real durable
// boundaries and then re-open the store in a fresh process.
//
// With the variable unset the hook costs one relaxed atomic load per boundary
// and never fires.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_CRASH_HPP
#define SNCF_CRASH_HPP

#include <cstdint>
#include <string_view>

namespace sncf {

enum class CrashPoint : std::uint8_t {
  None = 0,
  JournalBeforeWrite = 1,
  JournalAfterWriteBeforeSync = 2,
  JournalAfterSyncBeforeAck = 3,
  SnapshotBeforeRename = 4,
  SnapshotAfterRenameBeforeJournalRewrite = 5,
  ShutdownBeforeClose = 6,
};

[[nodiscard]] std::string_view crash_point_name(CrashPoint point) noexcept;

/// Terminates the process immediately when the configured crash point matches.
/// Never returns for a matching point.
void crash_if_requested(CrashPoint point) noexcept;

/// Reads and caches SNCF_CRASH_AT. Exposed so tests and tools can report the
/// armed boundary without triggering it.
[[nodiscard]] CrashPoint armed_crash_point() noexcept;

/// Arms a boundary programmatically, overriding SNCF_CRASH_AT. Used by the
/// durability harness so a dedicated child process can be terminated at an exact
/// durable boundary after it has finished its setup work.
void arm_crash_point(CrashPoint point) noexcept;

/// QA fault injection used by the durability tests: the next journal append
/// performs a partial write and then fails. This exercises the rollback path
/// that keeps a failed append from stranding bytes in the journal. It is a
/// no-op in production because nothing calls it.
void fail_next_append_once() noexcept;

/// Consumes the injection flag. Returns true exactly once after
/// fail_next_append_once() was called.
[[nodiscard]] bool consume_append_failure() noexcept;

}  // namespace sncf

#endif  // SNCF_CRASH_HPP
