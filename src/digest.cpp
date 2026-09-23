// Copyright 2026 Summon Software Labs.
#include "sncf/digest.hpp"

#include <cstring>

namespace sncf {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256K{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
  return static_cast<std::uint32_t>((value >> count) | (value << (32U - count)));
}

constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  constexpr std::uint32_t kPoly = 0x82f63b78U;
  for (std::uint32_t i = 0; i < 256U; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? static_cast<std::uint32_t>((crc >> 1U) ^ kPoly)
                             : static_cast<std::uint32_t>(crc >> 1U);
    }
    table[i] = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

constexpr char hex_digit(unsigned value) noexcept {
  return static_cast<char>(value < 10U ? ('0' + value) : ('a' + (value - 10U)));
}

constexpr int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  return -1;
}

}  // namespace

bool Digest256::is_zero() const noexcept {
  for (const auto byte : bytes_) {
    if (byte != 0U) {
      return false;
    }
  }
  return true;
}

std::string Digest256::to_hex() const {
  std::string out;
  out.resize(64U);
  for (std::size_t i = 0; i < bytes_.size(); ++i) {
    out[i * 2U] = hex_digit(static_cast<unsigned>(bytes_[i] >> 4U));
    out[(i * 2U) + 1U] = hex_digit(static_cast<unsigned>(bytes_[i] & 0x0FU));
  }
  return out;
}

std::optional<Digest256> Digest256::from_hex(std::string_view hex) noexcept {
  if (hex.size() != 64U) {
    return std::nullopt;
  }
  bytes_type bytes{};
  for (std::size_t i = 0; i < 32U; ++i) {
    const int high = hex_value(hex[i * 2U]);
    const int low = hex_value(hex[(i * 2U) + 1U]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    bytes[i] = static_cast<std::uint8_t>((static_cast<unsigned>(high) << 4U) | static_cast<unsigned>(low));
  }
  return Digest256{bytes};
}

void Sha256::reset() noexcept {
  state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  buffer_.fill(0U);
  bit_length_ = 0;
  buffered_ = 0;
}

void Sha256::compress(const std::uint8_t block[64]) noexcept {
  std::array<std::uint32_t, 64> w{};
  for (std::size_t i = 0; i < 16U; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4U]) << 24U) |
           (static_cast<std::uint32_t>(block[(i * 4U) + 1U]) << 16U) |
           (static_cast<std::uint32_t>(block[(i * 4U) + 2U]) << 8U) |
           static_cast<std::uint32_t>(block[(i * 4U) + 3U]);
  }
  for (std::size_t i = 16U; i < 64U; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15U], 7U) ^ rotr(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
    const std::uint32_t s1 = rotr(w[i - 2U], 17U) ^ rotr(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
    w[i] = static_cast<std::uint32_t>(w[i - 16U] + s0 + w[i - 7U] + s1);
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64U; ++i) {
    const std::uint32_t s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = static_cast<std::uint32_t>(h + s1 + ch + kSha256K[i] + w[i]);
    const std::uint32_t s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = static_cast<std::uint32_t>(s0 + maj);
    h = g;
    g = f;
    f = e;
    e = static_cast<std::uint32_t>(d + temp1);
    d = c;
    c = b;
    b = a;
    a = static_cast<std::uint32_t>(temp1 + temp2);
  }

  state_[0] = static_cast<std::uint32_t>(state_[0] + a);
  state_[1] = static_cast<std::uint32_t>(state_[1] + b);
  state_[2] = static_cast<std::uint32_t>(state_[2] + c);
  state_[3] = static_cast<std::uint32_t>(state_[3] + d);
  state_[4] = static_cast<std::uint32_t>(state_[4] + e);
  state_[5] = static_cast<std::uint32_t>(state_[5] + f);
  state_[6] = static_cast<std::uint32_t>(state_[6] + g);
  state_[7] = static_cast<std::uint32_t>(state_[7] + h);
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  bit_length_ += static_cast<std::uint64_t>(size) * 8U;
  std::size_t offset = 0;
  if (buffered_ != 0U) {
    const std::size_t need = 64U - buffered_;
    const std::size_t take = size < need ? size : need;
    std::memcpy(buffer_.data() + buffered_, bytes, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == 64U) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (offset + 64U <= size) {
    compress(bytes + offset);
    offset += 64U;
  }
  if (offset < size) {
    const std::size_t remaining = size - offset;
    std::memcpy(buffer_.data(), bytes + offset, remaining);
    buffered_ = remaining;
  }
}

Digest256 Sha256::finish() noexcept {
  const std::uint64_t total_bits = bit_length_;
  const std::uint8_t pad = 0x80U;
  update(&pad, 1U);
  const std::uint8_t zero = 0x00U;
  while (buffered_ != 56U) {
    update(&zero, 1U);
  }
  std::uint8_t length_bytes[8]{};
  for (std::size_t i = 0; i < 8U; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>((total_bits >> ((7U - i) * 8U)) & 0xFFU);
  }
  update(length_bytes, 8U);

  Digest256::bytes_type out{};
  for (std::size_t i = 0; i < 8U; ++i) {
    out[i * 4U] = static_cast<std::uint8_t>((state_[i] >> 24U) & 0xFFU);
    out[(i * 4U) + 1U] = static_cast<std::uint8_t>((state_[i] >> 16U) & 0xFFU);
    out[(i * 4U) + 2U] = static_cast<std::uint8_t>((state_[i] >> 8U) & 0xFFU);
    out[(i * 4U) + 3U] = static_cast<std::uint8_t>(state_[i] & 0xFFU);
  }
  return Digest256{out};
}

Digest256 Sha256::hash(std::string_view text) noexcept {
  Sha256 hasher;
  hasher.update(text);
  return hasher.finish();
}

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    crc = kCrc32cTable[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8U);
  }
  return crc ^ 0xFFFFFFFFU;
}

void Crc32cAccumulator::update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = value_ ^ 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    crc = kCrc32cTable[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8U);
  }
  value_ = crc ^ 0xFFFFFFFFU;
}

}  // namespace sncf
