// Injected time source. The runtime never sleeps and never uses a timeout:
// all expiry decisions compare logical timestamps supplied by the clock.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_TIME_HPP
#define SNCF_TIME_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "sncf/ids.hpp"

namespace sncf {

/// Abstract clock.
///
/// Implementations must be safe to call from multiple threads, must be
/// non-blocking, and must never call back into the Fabric: the clock is read
/// while the coordinator lock is held, so a re-entrant clock would deadlock.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;
  [[nodiscard]] virtual TimestampNs now() const noexcept = 0;
};

/// Wall-clock source used in production. Reads the system clock and converts to
/// nanoseconds since the Unix epoch with checked narrowing.
class SystemClock final : public Clock {
 public:
  [[nodiscard]] TimestampNs now() const noexcept override;
};

/// Deterministic clock for tests, simulations and replay. Time only moves when
/// the caller moves it, which is what makes expiry decisions reproducible.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(TimestampNs start = TimestampNs::from_value(1'700'000'000'000'000'000ULL)) noexcept
      : current_(start.value()) {}

  [[nodiscard]] TimestampNs now() const noexcept override {
    return TimestampNs::from_value(current_.load(std::memory_order_relaxed));
  }

  void set(TimestampNs value) noexcept { current_.store(value.value(), std::memory_order_relaxed); }

  /// Advance by an exact duration, refusing on overflow rather than wrapping.
  [[nodiscard]] bool advance(DurationNs delta) noexcept {
    std::uint64_t current = current_.load(std::memory_order_relaxed);
    std::uint64_t next = 0;
    if (!add_checked<std::uint64_t>(current, delta.value(), next)) {
      return false;
    }
    current_.store(next, std::memory_order_relaxed);
    return true;
  }

 private:
  std::atomic<std::uint64_t> current_;
};

[[nodiscard]] inline DurationNs millis(std::uint64_t value) noexcept {
  std::uint64_t ns = 0;
  if (!mul_checked<std::uint64_t>(value, 1'000'000ULL, ns)) {
    ns = (std::numeric_limits<std::uint64_t>::max)();
  }
  return DurationNs::from_value(ns);
}

[[nodiscard]] inline DurationNs seconds(std::uint64_t value) noexcept {
  std::uint64_t ns = 0;
  if (!mul_checked<std::uint64_t>(value, 1'000'000'000ULL, ns)) {
    ns = (std::numeric_limits<std::uint64_t>::max)();
  }
  return DurationNs::from_value(ns);
}

}  // namespace sncf

#endif  // SNCF_TIME_HPP
