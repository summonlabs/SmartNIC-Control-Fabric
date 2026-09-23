// Copyright 2026 Summon Software Labs.
#include "sncf/crash.hpp"

#include <array>
#include <atomic>
#include <cstdlib>

namespace sncf {
namespace {

constexpr std::array<std::string_view, 7> kCrashNames{"none",
                                                      "journal_before_write",
                                                      "journal_after_write_before_sync",
                                                      "journal_after_sync_before_ack",
                                                      "snapshot_before_rename",
                                                      "snapshot_after_rename_before_journal_rewrite",
                                                      "shutdown_before_close"};

std::atomic<CrashPoint> g_armed{CrashPoint::None};
std::atomic<bool> g_loaded{false};
std::atomic<bool> g_fail_next_append{false};

void load_once() noexcept {
  if (g_loaded.load(std::memory_order_acquire)) {
    return;
  }
  const char* raw = std::getenv("SNCF_CRASH_AT");
  CrashPoint point = CrashPoint::None;
  if (raw != nullptr) {
    const std::string_view requested{raw};
    for (std::size_t i = 0; i < kCrashNames.size(); ++i) {
      if (kCrashNames[i] == requested) {
        point = static_cast<CrashPoint>(i);
        break;
      }
    }
  }
  g_armed.store(point, std::memory_order_release);
  g_loaded.store(true, std::memory_order_release);
}

}  // namespace

std::string_view crash_point_name(CrashPoint point) noexcept {
  const auto index = static_cast<std::size_t>(point);
  return index < kCrashNames.size() ? kCrashNames[index] : std::string_view{"none"};
}

CrashPoint armed_crash_point() noexcept {
  load_once();
  return g_armed.load(std::memory_order_acquire);
}

void fail_next_append_once() noexcept { g_fail_next_append.store(true, std::memory_order_release); }

bool consume_append_failure() noexcept {
  return g_fail_next_append.exchange(false, std::memory_order_acq_rel);
}

void arm_crash_point(CrashPoint point) noexcept {
  g_loaded.store(true, std::memory_order_release);
  g_armed.store(point, std::memory_order_release);
}

void crash_if_requested(CrashPoint point) noexcept {
  if (point == CrashPoint::None) {
    return;
  }
  load_once();
  if (g_armed.load(std::memory_order_acquire) == point) {
    // Hard exit: no stack unwinding, no stream flush, no destructors.
    std::_Exit(70 + static_cast<int>(point));
  }
}

}  // namespace sncf
