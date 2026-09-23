// Copyright 2026 Summon Software Labs.
#include "sncf/time.hpp"

namespace sncf {

TimestampNs SystemClock::now() const noexcept {
  const auto ticks = std::chrono::system_clock::now().time_since_epoch();
  const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(ticks).count();
  if (nanoseconds <= 0) {
    return TimestampNs{};
  }
  const auto unsigned_value = static_cast<std::uint64_t>(nanoseconds);
  return TimestampNs::from_value(unsigned_value);
}

}  // namespace sncf
