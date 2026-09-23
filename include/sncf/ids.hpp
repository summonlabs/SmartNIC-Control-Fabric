// Typed identities, generations, epochs, incarnations and bounded scalar types.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_IDS_HPP
#define SNCF_IDS_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sncf/canonical.hpp"
#include "sncf/status.hpp"
#include "sncf/strong.hpp"

namespace sncf {

// --- logical and physical identities ---------------------------------------
using SmartNicId = StrongId<struct SmartNicIdTag, std::uint64_t>;
using DeviceId = StrongId<struct DeviceIdTag, std::uint64_t>;
using DeviceIncarnation = StrongId<struct DeviceIncarnationTag, std::uint32_t>;
using DeviceModelId = StrongId<struct DeviceModelIdTag, std::uint16_t>;
using FunctionPackageId = StrongId<struct FunctionPackageIdTag, std::uint64_t>;
using FunctionInstanceId = StrongId<struct FunctionInstanceIdTag, std::uint64_t>;
using PortId = StrongId<struct PortIdTag, std::uint16_t>;
using QueueId = StrongId<struct QueueIdTag, std::uint32_t>;
using PrincipalId = StrongId<struct PrincipalIdTag, std::uint64_t>;

// --- generations, epochs and incarnations ----------------------------------
using CapabilityGeneration = StrongId<struct CapabilityGenerationTag, std::uint64_t>;
using CompatibilityGeneration = StrongId<struct CompatibilityGenerationTag, std::uint64_t>;
using DeploymentGeneration = StrongId<struct DeploymentGenerationTag, std::uint64_t>;
using PolicyGeneration = StrongId<struct PolicyGenerationTag, std::uint64_t>;
using CoordinatorEpoch = StrongId<struct CoordinatorEpochTag, std::uint64_t>;
using BootId = StrongId<struct BootIdTag, std::uint64_t>;
using LayoutRevision = StrongId<struct LayoutRevisionTag, std::uint64_t>;

// --- operation bookkeeping --------------------------------------------------
using AttemptId = StrongId<struct AttemptIdTag, std::uint32_t>;
using LeaseId = StrongId<struct LeaseIdTag, std::uint64_t>;
using FencingToken = StrongId<struct FencingTokenTag, std::uint64_t>;
using CommandId = StrongId<struct CommandIdTag, std::uint64_t>;
using EvidenceId = StrongId<struct EvidenceIdTag, std::uint64_t>;
using RecordSequence = StrongId<struct RecordSequenceTag, std::uint64_t>;
using EventSequence = StrongId<struct EventSequenceTag, std::uint64_t>;

// --- time -------------------------------------------------------------------
/// Nanoseconds since the Unix epoch, supplied by the runtime clock. Externally
/// derived, so every arithmetic operation on it is checked.
using TimestampNs = StrongId<struct TimestampTag, std::uint64_t>;
/// A non-negative elapsed duration in nanoseconds.
using DurationNs = StrongId<struct DurationTag, std::uint64_t>;

[[nodiscard]] std::optional<TimestampNs> timestamp_add(TimestampNs base, DurationNs delta) noexcept;
[[nodiscard]] std::optional<DurationNs> timestamp_difference(TimestampNs later, TimestampNs earlier) noexcept;
[[nodiscard]] bool timestamp_less(TimestampNs a, TimestampNs b) noexcept;

// --- semantic version -------------------------------------------------------
struct SemVer {
  std::uint16_t major = 0;
  std::uint16_t minor = 0;
  std::uint16_t patch = 0;

  [[nodiscard]] std::string to_string() const;
  friend bool operator==(const SemVer& a, const SemVer& b) noexcept {
    return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
  }
  friend bool operator!=(const SemVer& a, const SemVer& b) noexcept { return !(a == b); }
  friend bool operator<(const SemVer& a, const SemVer& b) noexcept {
    if (a.major != b.major) {
      return a.major < b.major;
    }
    if (a.minor != b.minor) {
      return a.minor < b.minor;
    }
    return a.patch < b.patch;
  }
  friend bool operator>(const SemVer& a, const SemVer& b) noexcept { return b < a; }
  friend bool operator<=(const SemVer& a, const SemVer& b) noexcept { return !(b < a); }
  friend bool operator>=(const SemVer& a, const SemVer& b) noexcept { return !(a < b); }
};

/// Strict parse of "major.minor.patch"; no prefixes, suffixes or whitespace.
[[nodiscard]] std::optional<SemVer> parse_semver(std::string_view text) noexcept;
[[nodiscard]] Value semver_to_value(const SemVer& version);
[[nodiscard]] Result<SemVer> semver_from_value(const Value& value) noexcept;

// --- capability set ---------------------------------------------------------
/// Vendor-neutral programmable-function capability codes. The numeric value is
/// the persisted and exported identity of the capability and is never reused.
enum class CapabilityCode : std::uint16_t {
  Unknown = 0,
  PacketParsing = 1,
  PacketModification = 2,
  ConnectionTracking = 3,
  StatefulFiltering = 4,
  TransportOffload = 5,
  EncapsulationOffload = 6,
  QueueSteering = 7,
  RateLimiting = 8,
  Timestamping = 9,
  Replication = 10,
  LoadDistribution = 11,
  AtomicCounters = 12,
  OnCardEventTracing = 13,
  HostMemoryDma = 14,
  ProgrammablePipeline = 15,
};

inline constexpr std::uint16_t kCapabilityCodeMax = 15;
inline constexpr std::size_t kCapabilityWordBits = 64;
inline constexpr std::size_t kCapabilityWords = 2;

[[nodiscard]] std::string_view capability_name(CapabilityCode code) noexcept;
[[nodiscard]] CapabilityCode capability_from_name(std::string_view name) noexcept;

/// A fixed-width bit set over CapabilityCode. Subset tests are exact; there is
/// no "unknown capability implies support" path.
class CapabilityMask {
 public:
  constexpr CapabilityMask() noexcept = default;

  [[nodiscard]] static CapabilityMask empty() noexcept { return CapabilityMask{}; }
  [[nodiscard]] static CapabilityMask of(CapabilityCode code) noexcept;
  [[nodiscard]] static std::optional<CapabilityMask> from_codes(const std::vector<CapabilityCode>& codes) noexcept;

  void insert(CapabilityCode code) noexcept;
  [[nodiscard]] bool contains(CapabilityCode code) const noexcept;
  [[nodiscard]] bool contains_all(const CapabilityMask& other) const noexcept;
  [[nodiscard]] bool is_empty() const noexcept;
  [[nodiscard]] CapabilityMask union_with(const CapabilityMask& other) const noexcept;
  [[nodiscard]] CapabilityMask without(const CapabilityMask& other) const noexcept;

  /// Canonical lowercase hex, most significant word first.
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] static std::optional<CapabilityMask> from_hex(std::string_view text) noexcept;
  /// Sorted list of contained codes, ascending.
  [[nodiscard]] std::vector<CapabilityCode> codes() const;

  friend bool operator==(const CapabilityMask& a, const CapabilityMask& b) noexcept {
    return a.words_ == b.words_;
  }
  friend bool operator!=(const CapabilityMask& a, const CapabilityMask& b) noexcept { return !(a == b); }

 private:
  std::array<std::uint64_t, kCapabilityWords> words_{};
};

/// A closed range of compatibility generations, inclusive on both ends.
struct CompatibilityRange {
  CompatibilityGeneration min;
  CompatibilityGeneration max;

  [[nodiscard]] bool contains(CompatibilityGeneration generation) const noexcept {
    return generation.value() >= min.value() && generation.value() <= max.value();
  }
  [[nodiscard]] bool valid() const noexcept { return min.value() <= max.value(); }
  friend bool operator==(const CompatibilityRange& a, const CompatibilityRange& b) noexcept {
    return a.min == b.min && a.max == b.max;
  }
};

// --- label validation -------------------------------------------------------
inline constexpr std::size_t kMaxLabelBytes = 64;

/// Labels are canonical lowercase tokens: [a-z0-9] first, then [a-z0-9._-].
/// Anything else is refused instead of being normalised.
[[nodiscard]] bool is_valid_label(std::string_view label) noexcept;

/// Returns the label when valid, otherwise a refusal with a stable code.
[[nodiscard]] Result<std::string> validate_label(std::string label) noexcept;

}  // namespace sncf

#endif  // SNCF_IDS_HPP
