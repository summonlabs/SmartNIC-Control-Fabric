// Copyright 2026 Summon Software Labs.
#include "sncf/protocol.hpp"

#include "sncf/digest.hpp"
#include "sncf/version.hpp"

namespace sncf {
namespace {

void put_u16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFFU));
  out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void put_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFFU));
  out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<char>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

void put_u64(std::string& out, std::uint64_t value) {
  for (unsigned i = 0; i < 8U; ++i) {
    out.push_back(static_cast<char>((value >> (i * 8U)) & 0xFFU));
  }
}

std::uint16_t read_u16(std::string_view bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset]) |
                                    (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 1U])) << 8U));
}

std::uint32_t read_u32(std::string_view bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1U])) << 8U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2U])) << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3U])) << 24U);
}

std::uint64_t read_u64(std::string_view bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8U; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8U);
  }
  return value;
}

}  // namespace

std::string_view frame_kind_name(FrameKind kind) noexcept {
  switch (kind) {
    case FrameKind::Request:
      return "request";
    case FrameKind::Response:
      return "response";
    case FrameKind::Goodbye:
      return "goodbye";
    case FrameKind::Error:
      return "error";
  }
  return "unknown";
}

FrameKind frame_kind_from_name(std::string_view name) noexcept {
  for (std::uint16_t raw = 1; raw <= 4U; ++raw) {
    const auto kind = static_cast<FrameKind>(raw);
    if (frame_kind_name(kind) == name) {
      return kind;
    }
  }
  return FrameKind::Error;
}

std::string Frame::encode() const {
  std::string out;
  out.reserve(kFrameHeaderBytes + payload.size());
  put_u32(out, kFrameMagic);
  put_u16(out, static_cast<std::uint16_t>(kProtocolVersion));
  put_u16(out, static_cast<std::uint16_t>(kind));
  put_u16(out, flags);
  put_u16(out, 0U);
  put_u32(out, static_cast<std::uint32_t>(payload.size()));
  put_u32(out, crc32c(payload.data(), payload.size()));
  put_u64(out, request_id);
  out.append(payload);
  return out;
}

Result<Frame> decode_frame(std::string_view bytes, std::size_t max_payload, std::size_t& consumed) noexcept {
  consumed = 0;
  if (bytes.size() < kFrameHeaderBytes) {
    return Status(ReasonCode::RefusedTruncatedInput, "frame header is incomplete");
  }
  if (read_u32(bytes, 0U) != kFrameMagic) {
    return Status(ReasonCode::RefusedMalformedInput, "frame magic does not match");
  }
  const std::uint16_t version = read_u16(bytes, 4U);
  if (version != static_cast<std::uint16_t>(kProtocolVersion)) {
    return Status(ReasonCode::RefusedUnsupportedVersion, "frame protocol version is not supported");
  }
  const std::uint16_t kind_raw = read_u16(bytes, 6U);
  if (kind_raw < 1U || kind_raw > 4U) {
    return Status(ReasonCode::RefusedInvalidEnumValue, "frame kind is not known");
  }
  const std::uint32_t length = read_u32(bytes, 12U);
  if (length > max_payload) {
    return Status(ReasonCode::RefusedOversizedInput, "frame payload exceeds the configured bound");
  }
  if (bytes.size() < kFrameHeaderBytes + length) {
    return Status(ReasonCode::RefusedTruncatedInput, "frame payload is incomplete");
  }
  const std::uint32_t crc = read_u32(bytes, 16U);
  const std::string_view payload = bytes.substr(kFrameHeaderBytes, length);
  if (crc32c(payload.data(), payload.size()) != crc) {
    return Status(ReasonCode::RefusedJournalCorrupt, "frame payload failed its checksum");
  }
  Frame frame;
  frame.kind = static_cast<FrameKind>(kind_raw);
  frame.flags = read_u16(bytes, 8U);
  frame.request_id = read_u64(bytes, 20U);
  frame.payload.assign(payload);
  consumed = kFrameHeaderBytes + length;
  return frame;
}

Value protocol_error_value(Status status) {
  return Value::object({
      {"detail", Value::string(status.detail())},
      {"ok", Value::boolean(false)},
      {"reason", Value::string(std::string(reason_name(status.code())))},
  });
}

Status protocol_error_status(const Value& value) noexcept {
  if (!value.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "error document must be an object");
  }
  auto reason = field_string(value, "reason");
  if (!reason.ok()) {
    return Status(ReasonCode::RefusedMalformedInput, "error document is missing its reason code");
  }
  const ReasonCode code = reason_from_name(reason.value());
  if (code == ReasonCode::Unknown) {
    return Status(ReasonCode::RefusedInvalidEnumValue, "error document carries an unknown reason code");
  }
  const Value* detail = value.find("detail");
  return Status(code, detail != nullptr && detail->is_string() ? detail->as_string() : std::string{});
}

}  // namespace sncf
