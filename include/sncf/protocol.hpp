// Framed control-fabric protocol.
//
// Frame layout (all integers little-endian):
//   u32 magic | u16 version | u16 kind | u16 flags | u16 reserved |
//   u32 payload length | u32 payload CRC-32C | u64 request id | payload
//
// Payloads are canonical JSON documents. A frame whose length, magic, version or
// checksum does not match is refused before any payload byte is interpreted.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_PROTOCOL_HPP
#define SNCF_PROTOCOL_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "sncf/canonical.hpp"
#include "sncf/status.hpp"

namespace sncf {

inline constexpr std::uint32_t kFrameMagic = 0x534E4346U;  // "SNCF"
inline constexpr std::size_t kFrameHeaderBytes = 28U;
inline constexpr std::size_t kDefaultMaxFrameBytes = 1U * 1024U * 1024U;

enum class FrameKind : std::uint16_t {
  Request = 1,
  Response = 2,
  Goodbye = 3,
  Error = 4,
};

[[nodiscard]] std::string_view frame_kind_name(FrameKind kind) noexcept;
[[nodiscard]] FrameKind frame_kind_from_name(std::string_view name) noexcept;

struct Frame {
  FrameKind kind = FrameKind::Request;
  std::uint16_t flags = 0;
  std::uint64_t request_id = 0;
  std::string payload;

  [[nodiscard]] std::string encode() const;
};

/// Decodes one frame from a byte range. The out parameter receives the number of
/// bytes the frame occupied; trailing bytes belonging to the next frame are left
/// for the caller. Returns RefusedTruncatedInput when the buffer holds only part
/// of a frame, which lets a reader distinguish "need more bytes" from "corrupt".
[[nodiscard]] Result<Frame> decode_frame(std::string_view bytes, std::size_t max_payload,
                                         std::size_t& consumed) noexcept;

/// Errors carried in an Error frame payload.
[[nodiscard]] Value protocol_error_value(Status status);
[[nodiscard]] Status protocol_error_status(const Value& value) noexcept;

}  // namespace sncf

#endif  // SNCF_PROTOCOL_HPP
