// Copyright 2026 Summon Software Labs.
#include "sncf/ids.hpp"

#include <limits>

namespace sncf {
namespace {

constexpr std::array<std::string_view, 16> kCapabilityNames{
    "unknown",         "packet_parsing",     "packet_modification", "connection_tracking",
    "stateful_filtering", "transport_offload", "encapsulation_offload", "queue_steering",
    "rate_limiting",   "timestamping",        "replication",         "load_distribution",
    "atomic_counters", "on_card_event_tracing", "host_memory_dma",   "programmable_pipeline"};

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  return -1;
}

}  // namespace

std::optional<TimestampNs> timestamp_add(TimestampNs base, DurationNs delta) noexcept {
  std::uint64_t sum = 0;
  if (!add_checked<std::uint64_t>(base.value(), delta.value(), sum)) {
    return std::nullopt;
  }
  return TimestampNs::from_value(sum);
}

std::optional<DurationNs> timestamp_difference(TimestampNs later, TimestampNs earlier) noexcept {
  std::uint64_t delta = 0;
  if (!sub_checked<std::uint64_t>(later.value(), earlier.value(), delta)) {
    return std::nullopt;
  }
  return DurationNs::from_value(delta);
}

bool timestamp_less(TimestampNs a, TimestampNs b) noexcept { return a.value() < b.value(); }

std::string SemVer::to_string() const {
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

std::optional<SemVer> parse_semver(std::string_view text) noexcept {
  if (text.empty() || text.size() > 32U) {
    return std::nullopt;
  }
  std::array<std::uint32_t, 3> parts{};
  std::size_t part = 0;
  std::size_t digits = 0;
  std::uint32_t accumulated = 0;
  for (const char c : text) {
    if (c == '.') {
      if (digits == 0U || part >= 2U) {
        return std::nullopt;
      }
      parts[part++] = accumulated;
      accumulated = 0;
      digits = 0;
      continue;
    }
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    if (digits == 1U && accumulated == 0U) {
      return std::nullopt;  // leading zero
    }
    if (digits >= 5U) {
      return std::nullopt;
    }
    accumulated = (accumulated * 10U) + static_cast<std::uint32_t>(c - '0');
    ++digits;
    if (accumulated > 65535U) {
      return std::nullopt;
    }
  }
  if (digits == 0U || part != 2U) {
    return std::nullopt;
  }
  parts[2] = accumulated;
  SemVer out;
  out.major = static_cast<std::uint16_t>(parts[0]);
  out.minor = static_cast<std::uint16_t>(parts[1]);
  out.patch = static_cast<std::uint16_t>(parts[2]);
  return out;
}

Value semver_to_value(const SemVer& version) {
  return Value::object({{"major", Value::uint_value(version.major)},
                        {"minor", Value::uint_value(version.minor)},
                        {"patch", Value::uint_value(version.patch)}});
}

Result<SemVer> semver_from_value(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "semver must be an object");
  }
  auto major = field_uint(value, "major");
  SNCF_TRY(major);
  auto minor = field_uint(value, "minor");
  SNCF_TRY(minor);
  auto patch = field_uint(value, "patch");
  SNCF_TRY(patch);
  const auto major16 = narrow_checked<std::uint16_t>(major.value());
  const auto minor16 = narrow_checked<std::uint16_t>(minor.value());
  const auto patch16 = narrow_checked<std::uint16_t>(patch.value());
  if (!major16.has_value() || !minor16.has_value() || !patch16.has_value()) {
    return Status(ReasonCode::RefusedArithmeticOverflow, "semver component exceeds 65535");
  }
  SemVer out;
  out.major = *major16;
  out.minor = *minor16;
  out.patch = *patch16;
  return out;
}

std::string_view capability_name(CapabilityCode code) noexcept {
  const auto index = static_cast<std::size_t>(code);
  if (index >= kCapabilityNames.size()) {
    return "unknown";
  }
  return kCapabilityNames[index];
}

CapabilityCode capability_from_name(std::string_view name) noexcept {
  for (std::size_t i = 0; i < kCapabilityNames.size(); ++i) {
    if (kCapabilityNames[i] == name) {
      return static_cast<CapabilityCode>(i);
    }
  }
  return CapabilityCode::Unknown;
}

CapabilityMask CapabilityMask::of(CapabilityCode code) noexcept {
  CapabilityMask mask;
  mask.insert(code);
  return mask;
}

std::optional<CapabilityMask> CapabilityMask::from_codes(const std::vector<CapabilityCode>& codes) noexcept {
  CapabilityMask mask;
  for (const auto code : codes) {
    if (code == CapabilityCode::Unknown ||
        static_cast<std::uint16_t>(code) > kCapabilityCodeMax) {
      return std::nullopt;
    }
    mask.insert(code);
  }
  return mask;
}

void CapabilityMask::insert(CapabilityCode code) noexcept {
  const auto raw = static_cast<std::uint16_t>(code);
  if (raw == 0U || raw > kCapabilityCodeMax) {
    return;
  }
  const std::size_t word = static_cast<std::size_t>(raw - 1U) / kCapabilityWordBits;
  const std::size_t bit = static_cast<std::size_t>(raw - 1U) % kCapabilityWordBits;
  words_[word] |= (1ULL << bit);
}

bool CapabilityMask::contains(CapabilityCode code) const noexcept {
  const auto raw = static_cast<std::uint16_t>(code);
  if (raw == 0U || raw > kCapabilityCodeMax) {
    return false;
  }
  const std::size_t word = static_cast<std::size_t>(raw - 1U) / kCapabilityWordBits;
  const std::size_t bit = static_cast<std::size_t>(raw - 1U) % kCapabilityWordBits;
  return (words_[word] & (1ULL << bit)) != 0ULL;
}

bool CapabilityMask::contains_all(const CapabilityMask& other) const noexcept {
  for (std::size_t i = 0; i < kCapabilityWords; ++i) {
    if ((words_[i] & other.words_[i]) != other.words_[i]) {
      return false;
    }
  }
  return true;
}

bool CapabilityMask::is_empty() const noexcept {
  for (const auto word : words_) {
    if (word != 0ULL) {
      return false;
    }
  }
  return true;
}

CapabilityMask CapabilityMask::union_with(const CapabilityMask& other) const noexcept {
  CapabilityMask out;
  for (std::size_t i = 0; i < kCapabilityWords; ++i) {
    out.words_[i] = words_[i] | other.words_[i];
  }
  return out;
}

CapabilityMask CapabilityMask::without(const CapabilityMask& other) const noexcept {
  CapabilityMask out;
  for (std::size_t i = 0; i < kCapabilityWords; ++i) {
    out.words_[i] = words_[i] & ~other.words_[i];
  }
  return out;
}

std::string CapabilityMask::to_hex() const {
  std::string out;
  out.reserve(kCapabilityWords * 16U);
  bool leading = true;
  for (std::size_t i = kCapabilityWords; i-- > 0;) {
    const std::string word = to_hex_uint(words_[i], 16U);
    if (leading) {
      const auto first = word.find_first_not_of('0');
      if (first == std::string::npos) {
        continue;
      }
      leading = false;
      out.append(word.substr(first));
      continue;
    }
    out.append(word);
  }
  if (out.empty()) {
    out.push_back('0');
  }
  return out;
}

std::optional<CapabilityMask> CapabilityMask::from_hex(std::string_view text) noexcept {
  if (text.empty() || text.size() > kCapabilityWords * 16U) {
    return std::nullopt;
  }
  CapabilityMask mask;
  std::size_t index = 0;
  for (auto it = text.rbegin(); it != text.rend(); ++it, ++index) {
    const int digit = hex_value(*it);
    if (digit < 0) {
      return std::nullopt;
    }
    const std::size_t word = index / 16U;
    const std::size_t nibble = index % 16U;
    if (word >= kCapabilityWords) {
      if (digit != 0) {
        return std::nullopt;
      }
      continue;
    }
    mask.words_[word] |= static_cast<std::uint64_t>(digit) << (nibble * 4U);
  }
  return mask;
}

std::vector<CapabilityCode> CapabilityMask::codes() const {
  std::vector<CapabilityCode> out;
  for (std::uint16_t raw = 1; raw <= kCapabilityCodeMax; ++raw) {
    if (contains(static_cast<CapabilityCode>(raw))) {
      out.push_back(static_cast<CapabilityCode>(raw));
    }
  }
  return out;
}

bool is_valid_label(std::string_view label) noexcept {
  if (label.empty() || label.size() > kMaxLabelBytes) {
    return false;
  }
  const char first = label.front();
  const bool first_ok = (first >= 'a' && first <= 'z') || (first >= '0' && first <= '9');
  if (!first_ok) {
    return false;
  }
  for (const char c : label) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

Result<std::string> validate_label(std::string label) noexcept {
  if (label.empty()) {
    return Status(ReasonCode::RefusedMissingField, "label is empty");
  }
  if (label.size() > kMaxLabelBytes) {
    return Status(ReasonCode::RefusedOversizedInput, "label exceeds 64 bytes");
  }
  if (!is_valid_label(label)) {
    return Status(ReasonCode::RefusedInvalidLabel,
                  "label must match [a-z0-9][a-z0-9._-]* and be at most 64 bytes");
  }
  return label;
}

}  // namespace sncf
