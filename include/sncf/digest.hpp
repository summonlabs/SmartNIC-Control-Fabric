// Cryptographic-free integrity primitives: SHA-256 and CRC-32C.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_DIGEST_HPP
#define SNCF_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sncf {

/// 256-bit content digest. Used for evidence binding, package identity and
/// canonical export fingerprints. Never used as an authority token: authority is
/// carried by explicit fencing tokens and epochs.
class Digest256 {
 public:
  using bytes_type = std::array<std::uint8_t, 32>;

  constexpr Digest256() noexcept = default;
  explicit constexpr Digest256(bytes_type bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] const bytes_type& bytes() const noexcept { return bytes_; }
  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] std::string to_hex() const;

  /// Strict parse: exactly 64 lowercase hex characters. Uppercase, short or long
  /// input is refused rather than normalised.
  [[nodiscard]] static std::optional<Digest256> from_hex(std::string_view hex) noexcept;

  friend bool operator==(const Digest256& a, const Digest256& b) noexcept {
    return a.bytes_ == b.bytes_;
  }
  friend bool operator!=(const Digest256& a, const Digest256& b) noexcept {
    return !(a == b);
  }

 private:
  bytes_type bytes_{};
};

/// Streaming SHA-256 (FIPS 180-4).
class Sha256 {
 public:
  Sha256() noexcept { reset(); }

  void reset() noexcept;
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept {
    update(text.data(), text.size());
  }
  [[nodiscard]] Digest256 finish() noexcept;

  [[nodiscard]] static Digest256 hash(std::string_view text) noexcept;

 private:
  void compress(const std::uint8_t block[64]) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t bit_length_ = 0;
  std::size_t buffered_ = 0;
};

/// CRC-32C (Castagnoli), reflected, used for frame and journal-record integrity.
[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t size) noexcept;
[[nodiscard]] inline std::uint32_t crc32c(std::string_view text) noexcept {
  return crc32c(text.data(), text.size());
}

/// Incremental CRC-32C for streaming readers.
class Crc32cAccumulator {
 public:
  void update(const void* data, std::size_t size) noexcept;
  [[nodiscard]] std::uint32_t value() const noexcept { return value_; }
  void reset() noexcept { value_ = 0; }

 private:
  std::uint32_t value_ = 0;
};

}  // namespace sncf

#endif  // SNCF_DIGEST_HPP
