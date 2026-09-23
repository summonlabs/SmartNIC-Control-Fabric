// Strongly typed identities and checked arithmetic.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_STRONG_HPP
#define SNCF_STRONG_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace sncf {

/// A strongly typed scalar identity. Distinct tags produce distinct types that
/// cannot be compared, assigned or converted into one another by accident.
/// Value 0 is the reserved "nil" identity for every instantiation; no valid
/// entity is ever assigned nil by the runtime.
template <class Tag, class Rep = std::uint64_t>
class StrongId {
 public:
  using rep_type = Rep;

  constexpr StrongId() noexcept = default;

  static constexpr StrongId from_value(Rep value) noexcept {
    StrongId out;
    out.value_ = value;
    return out;
  }

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(StrongId a, StrongId b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(StrongId a, StrongId b) noexcept { return a.value_ != b.value_; }
  friend constexpr auto operator<=>(StrongId a, StrongId b) noexcept { return a.value_ <=> b.value_; }

 private:
  Rep value_ = 0;
};

/// Monotonic successor for identities that are allocated as a dense sequence.
/// Returns std::nullopt on wrap-around instead of silently reusing an identity.
template <class Id>
[[nodiscard]] constexpr std::optional<Id> next_id(Id current) noexcept {
  using rep = typename Id::rep_type;
  if (current.value() == (std::numeric_limits<rep>::max)()) {
    return std::nullopt;
  }
  return Id::from_value(static_cast<rep>(current.value() + 1));
}

/// Hash functor for StrongId. Users pass this explicitly; nothing is injected
/// into namespace std.
template <class Id>
struct IdHash {
  std::size_t operator()(Id id) const noexcept {
    std::uint64_t v = static_cast<std::uint64_t>(id.value());
    v ^= v >> 33U;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33U;
    return static_cast<std::size_t>(v);
  }
};

// ---------------------------------------------------------------------------
// Checked arithmetic. Every externally derived size, timestamp, counter or
// generation is funnelled through these helpers so that overflow is an explicit
// refusal rather than a silent wrap.
// ---------------------------------------------------------------------------

template <class T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr bool add_checked(T a, T b, T& out) noexcept {
  if (b > static_cast<T>((std::numeric_limits<T>::max)() - a)) {
    return false;
  }
  out = static_cast<T>(a + b);
  return true;
}

template <class T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr bool sub_checked(T a, T b, T& out) noexcept {
  if (b > a) {
    return false;
  }
  out = static_cast<T>(a - b);
  return true;
}

template <class T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr bool mul_checked(T a, T b, T& out) noexcept {
  if (a != 0 && b > static_cast<T>((std::numeric_limits<T>::max)() / a)) {
    return false;
  }
  out = static_cast<T>(a * b);
  return true;
}

/// Narrowing conversion that reports failure instead of truncating.
template <class To, class From>
  requires std::is_integral_v<To> && std::is_integral_v<From>
[[nodiscard]] constexpr std::optional<To> narrow_checked(From value) noexcept {
  if constexpr (std::is_signed_v<From> == std::is_signed_v<To> && sizeof(To) >= sizeof(From)) {
    return static_cast<To>(value);
  } else if constexpr (std::is_signed_v<From> && !std::is_signed_v<To>) {
    if (value < 0) {
      return std::nullopt;
    }
    using ufrom = std::make_unsigned_t<From>;
    const auto uvalue = static_cast<ufrom>(value);
    if (uvalue > static_cast<ufrom>((std::numeric_limits<To>::max)())) {
      return std::nullopt;
    }
    return static_cast<To>(uvalue);
  } else if constexpr (!std::is_signed_v<From> && std::is_signed_v<To>) {
    if (value > static_cast<std::make_unsigned_t<To>>((std::numeric_limits<To>::max)())) {
      return std::nullopt;
    }
    return static_cast<To>(value);
  } else {
    if (value > static_cast<From>((std::numeric_limits<To>::max)()) ||
        value < static_cast<From>((std::numeric_limits<To>::min)())) {
      return std::nullopt;
    }
    return static_cast<To>(value);
  }
}

/// Bytes required to hold count elements of item_size bytes each, with overflow
/// refused. Used to bound every allocation driven by external input.
[[nodiscard]] constexpr std::optional<std::size_t> byte_size_checked(std::uint64_t count,
                                                                     std::uint64_t item_size) noexcept {
  std::uint64_t total = 0;
  if (!mul_checked<std::uint64_t>(count, item_size, total)) {
    return std::nullopt;
  }
  return narrow_checked<std::size_t>(total);
}

}  // namespace sncf

#endif  // SNCF_STRONG_HPP
