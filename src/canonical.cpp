// Copyright 2026 Summon Software Labs.
#include "sncf/canonical.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace sncf {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

bool is_control(char c) noexcept { return static_cast<unsigned char>(c) < 0x20U; }

void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7FU) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FFU) {
    out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else if (code_point <= 0xFFFFU) {
    out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  }
}

int hex_digit_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

void escape_into(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (byte < 0x20U) {
          out.append("\\u00");
          out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
          out.push_back(kHexDigits[byte & 0x0FU]);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
}

class Parser {
 public:
  Parser(std::string_view text, const ParseLimits& limits) noexcept : text_(text), limits_(limits) {}

  Result<Value> run() {
    if (text_.size() > limits_.max_bytes) {
      return refuse(ReasonCode::RefusedOversizedInput, "input exceeds max_bytes");
    }
    Value value;
    if (!parse_value(0U, value)) {
      return failure_;
    }
    if (pos_ != text_.size()) {
      return refuse(ReasonCode::RefusedTrailingGarbage, "bytes remain after the value");
    }
    return value;
  }

 private:
  Status refuse(ReasonCode code, std::string detail) {
    failure_ = Status(code, std::move(detail));
    return failure_;
  }

  bool at_end() const noexcept { return pos_ >= text_.size(); }
  char peek() const noexcept { return text_[pos_]; }

  bool parse_value(std::size_t depth, Value& out) {
    if (depth > limits_.max_depth) {
      refuse(ReasonCode::RefusedDepthExceeded, "nesting depth bound exceeded");
      return false;
    }
    ++nodes_;
    if (nodes_ > max_nodes()) {
      refuse(ReasonCode::RefusedOversizedInput, "node count bound exceeded");
      return false;
    }
    if (at_end()) {
      refuse(ReasonCode::RefusedTruncatedInput, "value expected but input ended");
      return false;
    }
    switch (peek()) {
      case '{':
        return parse_object(depth, out);
      case '[':
        return parse_array(depth, out);
      case '"': {
        std::string text;
        if (!parse_string(text)) {
          return false;
        }
        out = Value::string(std::move(text));
        return true;
      }
      case 't':
        if (text_.substr(pos_, 4U) == "true") {
          pos_ += 4U;
          out = Value::boolean(true);
          return true;
        }
        refuse(ReasonCode::RefusedMalformedInput, "invalid literal");
        return false;
      case 'f':
        if (text_.substr(pos_, 5U) == "false") {
          pos_ += 5U;
          out = Value::boolean(false);
          return true;
        }
        refuse(ReasonCode::RefusedMalformedInput, "invalid literal");
        return false;
      case 'n':
        if (text_.substr(pos_, 4U) == "null") {
          pos_ += 4U;
          out = Value::null();
          return true;
        }
        refuse(ReasonCode::RefusedMalformedInput, "invalid literal");
        return false;
      default:
        return parse_number(out);
    }
  }

  [[nodiscard]] std::size_t max_nodes() const noexcept {
    return limits_.max_nodes == 0U ? 1U : limits_.max_nodes;
  }

  bool parse_number(Value& out) {
    const std::size_t start = pos_;
    const bool negative = !at_end() && peek() == '-';
    if (negative) {
      ++pos_;
    }
    if (at_end() || peek() < '0' || peek() > '9') {
      refuse(ReasonCode::RefusedMalformedInput, "number has no digits");
      return false;
    }
    if (peek() == '0' && (pos_ + 1U) < text_.size()) {
      const char next = text_[pos_ + 1U];
      if (next >= '0' && next <= '9') {
        refuse(ReasonCode::RefusedNonCanonicalNumber, "leading zero");
        return false;
      }
    }
    std::uint64_t magnitude = 0;
    while (!at_end() && peek() >= '0' && peek() <= '9') {
      const std::uint64_t digit = static_cast<std::uint64_t>(peek() - '0');
      std::uint64_t scaled = 0;
      if (!mul_checked<std::uint64_t>(magnitude, 10U, scaled)) {
        refuse(ReasonCode::RefusedArithmeticOverflow, "integer literal overflows");
        return false;
      }
      std::uint64_t summed = 0;
      if (!add_checked<std::uint64_t>(scaled, digit, summed)) {
        refuse(ReasonCode::RefusedArithmeticOverflow, "integer literal overflows");
        return false;
      }
      magnitude = summed;
      ++pos_;
    }
    if (!at_end() && (peek() == '.' || peek() == 'e' || peek() == 'E')) {
      refuse(ReasonCode::RefusedNonCanonicalNumber, "fractional or exponent form is not canonical");
      return false;
    }
    if (negative) {
      if (magnitude == 0U) {
        refuse(ReasonCode::RefusedNonCanonicalNumber, "negative zero is not canonical");
        return false;
      }
      if (magnitude > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1U) {
        refuse(ReasonCode::RefusedArithmeticOverflow, "negative literal overflows int64");
        return false;
      }
      if (magnitude == static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1U) {
        out = Value::int_value((std::numeric_limits<std::int64_t>::min)());
        return true;
      }
      out = Value::int_value(-static_cast<std::int64_t>(magnitude));
      return true;
    }
    out = Value::uint_value(magnitude);
    (void)start;
    return true;
  }

  bool parse_string(std::string& out) {
    if (at_end() || peek() != '"') {
      refuse(ReasonCode::RefusedMalformedInput, "string expected");
      return false;
    }
    ++pos_;
    out.clear();
    while (true) {
      if (at_end()) {
        refuse(ReasonCode::RefusedTruncatedInput, "string not terminated");
        return false;
      }
      const char c = peek();
      if (c == '"') {
        ++pos_;
        break;
      }
      if (static_cast<unsigned char>(c) < 0x20U) {
        refuse(ReasonCode::RefusedMalformedInput, "raw control character in string");
        return false;
      }
      if (c != '\\') {
        out.push_back(c);
        ++pos_;
      } else {
        ++pos_;
        if (at_end()) {
          refuse(ReasonCode::RefusedTruncatedInput, "escape not terminated");
          return false;
        }
        const char esc = peek();
        ++pos_;
        switch (esc) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            std::uint32_t code_point = 0;
            if (!parse_hex4(code_point)) {
              return false;
            }
            if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
              if ((pos_ + 1U) >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1U] != 'u') {
                refuse(ReasonCode::RefusedMalformedInput, "lone high surrogate");
                return false;
              }
              pos_ += 2U;
              std::uint32_t low = 0;
              if (!parse_hex4(low)) {
                return false;
              }
              if (low < 0xDC00U || low > 0xDFFFU) {
                refuse(ReasonCode::RefusedMalformedInput, "invalid low surrogate");
                return false;
              }
              code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
            } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
              refuse(ReasonCode::RefusedMalformedInput, "lone low surrogate");
              return false;
            }
            append_utf8(out, code_point);
            break;
          }
          default:
            refuse(ReasonCode::RefusedMalformedInput, "unknown escape sequence");
            return false;
        }
      }
      if (out.size() > limits_.max_string_bytes) {
        refuse(ReasonCode::RefusedOversizedInput, "string exceeds max_string_bytes");
        return false;
      }
    }
    if (!is_valid_utf8(out)) {
      refuse(ReasonCode::RefusedMalformedInput, "string is not valid UTF-8");
      return false;
    }
    return true;
  }

  bool parse_hex4(std::uint32_t& out) {
    if (pos_ + 4U > text_.size()) {
      refuse(ReasonCode::RefusedTruncatedInput, "unicode escape truncated");
      return false;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const int digit = hex_digit_value(text_[pos_ + static_cast<std::size_t>(i)]);
      if (digit < 0) {
        refuse(ReasonCode::RefusedMalformedInput, "unicode escape has a non-hex digit");
        return false;
      }
      value = (value << 4U) | static_cast<std::uint32_t>(digit);
    }
    pos_ += 4U;
    out = value;
    return true;
  }

  bool parse_array(std::size_t depth, Value& out) {
    ++pos_;  // consume '['
    Value::array_type items;
    if (!at_end() && peek() == ']') {
      ++pos_;
      out = Value::array(std::move(items));
      return true;
    }
    while (true) {
      if (items.size() >= limits_.max_array_items) {
        refuse(ReasonCode::RefusedOversizedInput, "array exceeds max_array_items");
        return false;
      }
      Value item;
      if (!parse_value(depth + 1U, item)) {
        return false;
      }
      items.push_back(std::move(item));
      if (at_end()) {
        refuse(ReasonCode::RefusedTruncatedInput, "array not terminated");
        return false;
      }
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        break;
      }
      refuse(ReasonCode::RefusedMalformedInput, "array element separator expected");
      return false;
    }
    out = Value::array(std::move(items));
    return true;
  }

  bool parse_object(std::size_t depth, Value& out) {
    ++pos_;  // consume '{'
    Value::object_type fields;
    if (!at_end() && peek() == '}') {
      ++pos_;
      out = Value::object(std::move(fields));
      return true;
    }
    while (true) {
      if (fields.size() >= limits_.max_object_fields) {
        refuse(ReasonCode::RefusedOversizedInput, "object exceeds max_object_fields");
        return false;
      }
      if (at_end() || peek() != '"') {
        refuse(ReasonCode::RefusedMalformedInput, "object key must be a string");
        return false;
      }
      std::string key;
      if (!parse_string(key)) {
        return false;
      }
      if (key.size() > limits_.max_key_bytes) {
        refuse(ReasonCode::RefusedOversizedInput, "object key exceeds max_key_bytes");
        return false;
      }
      if (!fields.empty()) {
        const int order = key.compare(fields.back().first);
        if (order == 0) {
          refuse(ReasonCode::RefusedDuplicateKey, "duplicate object key");
          return false;
        }
        if (order < 0) {
          refuse(ReasonCode::RefusedNonCanonicalOrder, "object keys are not in canonical order");
          return false;
        }
      }
      if (at_end() || peek() != ':') {
        refuse(ReasonCode::RefusedMalformedInput, "object key separator expected");
        return false;
      }
      ++pos_;
      Value item;
      if (!parse_value(depth + 1U, item)) {
        return false;
      }
      fields.emplace_back(std::move(key), std::move(item));
      if (at_end()) {
        refuse(ReasonCode::RefusedTruncatedInput, "object not terminated");
        return false;
      }
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        break;
      }
      refuse(ReasonCode::RefusedMalformedInput, "object field separator expected");
      return false;
    }
    out = Value::object(std::move(fields));
    return true;
  }

  std::string_view text_;
  const ParseLimits& limits_;
  std::size_t pos_ = 0;
  std::size_t nodes_ = 0;
  Status failure_{ReasonCode::RefusedMalformedInput};
};

Status type_refusal(const char* expected) {
  return Status(ReasonCode::RefusedInvalidEnumValue, std::string("expected ") + expected);
}

}  // namespace

Value Value::boolean(bool value) noexcept {
  Value out;
  out.storage_ = value;
  return out;
}

Value Value::uint_value(std::uint64_t value) noexcept {
  Value out;
  out.storage_ = value;
  return out;
}

Value Value::int_value(std::int64_t value) noexcept {
  Value out;
  out.storage_ = value;
  return out;
}

Value Value::string(std::string text) {
  Value out;
  out.storage_ = std::move(text);
  return out;
}

Value Value::array(array_type items) {
  Value out;
  out.storage_ = std::move(items);
  return out;
}

Value Value::object(object_type fields) {
  std::stable_sort(fields.begin(), fields.end(),
                   [](const field_type& a, const field_type& b) { return a.first < b.first; });
  fields.erase(std::unique(fields.begin(), fields.end(),
                           [](const field_type& a, const field_type& b) { return a.first == b.first; }),
               fields.end());
  Value out;
  out.storage_ = std::move(fields);
  return out;
}

const Value* Value::find(std::string_view key) const noexcept {
  if (!is_object()) {
    return nullptr;
  }
  const auto& fields = as_object();
  std::size_t low = 0;
  std::size_t high = fields.size();
  while (low < high) {
    const std::size_t mid = low + ((high - low) / 2U);
    const int order = fields[mid].first.compare(key);
    if (order == 0) {
      return &fields[mid].second;
    }
    if (order < 0) {
      low = mid + 1U;
    } else {
      high = mid;
    }
  }
  return nullptr;
}

void Value::append_canonical(std::string& out) const {
  switch (type()) {
    case Type::Null:
      out.append("null");
      return;
    case Type::Bool:
      out.append(as_bool() ? "true" : "false");
      return;
    case Type::UInt:
      out.append(std::to_string(as_uint()));
      return;
    case Type::Int:
      out.append(std::to_string(as_int()));
      return;
    case Type::String:
      escape_into(out, as_string());
      return;
    case Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const auto& item : as_array()) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        item.append_canonical(out);
      }
      out.push_back(']');
      return;
    }
    case Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& field : as_object()) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        escape_into(out, field.first);
        out.push_back(':');
        field.second.append_canonical(out);
      }
      out.push_back('}');
      return;
    }
  }
  out.append("null");
}

std::string Value::to_canonical() const {
  std::string out;
  append_canonical(out);
  return out;
}

Digest256 Value::digest() const {
  Sha256 hasher;
  std::string buffer;
  append_canonical(buffer);
  hasher.update(buffer);
  return hasher.finish();
}

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t i = 0;
  const std::size_t size = text.size();
  while (i < size) {
    const auto byte = static_cast<unsigned char>(text[i]);
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if (byte < 0x80U) {
      ++i;
      continue;
    } else if ((byte & 0xE0U) == 0xC0U) {
      extra = 1;
      code_point = byte & 0x1FU;
    } else if ((byte & 0xF0U) == 0xE0U) {
      extra = 2;
      code_point = byte & 0x0FU;
    } else if ((byte & 0xF8U) == 0xF0U) {
      extra = 3;
      code_point = byte & 0x07U;
    } else {
      return false;
    }
    if (i + extra >= size) {
      return false;
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto cont = static_cast<unsigned char>(text[i + k]);
      if ((cont & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (cont & 0x3FU);
    }
    if (extra == 1 && code_point < 0x80U) {
      return false;
    }
    if (extra == 2 && code_point < 0x800U) {
      return false;
    }
    if (extra == 3 && code_point < 0x10000U) {
      return false;
    }
    if (code_point > 0x10FFFFU) {
      return false;
    }
    if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
      return false;
    }
    i += extra + 1U;
  }
  return true;
}

Result<Value> parse_canonical(std::string_view text, const ParseLimits& limits) noexcept {
  Parser parser(text, limits);
  return parser.run();
}

Result<std::uint64_t> field_uint(const Value& object, std::string_view key) noexcept {
  auto located = field_value(object, key);
  SNCF_TRY(located);
  const Value* value = located.value();
  if (value->is_uint()) {
    return value->as_uint();
  }
  if (value->is_int()) {
    return Status(ReasonCode::RefusedInvalidRange, std::string(key) + " must be non-negative");
  }
  return type_refusal("unsigned integer");
}

Result<std::int64_t> field_int(const Value& object, std::string_view key) noexcept {
  auto located = field_value(object, key);
  SNCF_TRY(located);
  const Value* value = located.value();
  if (value->is_int()) {
    return value->as_int();
  }
  if (value->is_uint()) {
    const auto narrowed = narrow_checked<std::int64_t>(value->as_uint());
    if (!narrowed.has_value()) {
      return Status(ReasonCode::RefusedArithmeticOverflow, std::string(key) + " exceeds int64");
    }
    return *narrowed;
  }
  return type_refusal("integer");
}

Result<bool> field_bool(const Value& object, std::string_view key) noexcept {
  auto located = field_value(object, key);
  SNCF_TRY(located);
  const Value* value = located.value();
  if (value->is_bool()) {
    return value->as_bool();
  }
  return type_refusal("boolean");
}

Result<std::string> field_string(const Value& object, std::string_view key) noexcept {
  auto located = field_value(object, key);
  SNCF_TRY(located);
  const Value* value = located.value();
  if (value->is_string()) {
    return value->as_string();
  }
  return type_refusal("string");
}

Result<const Value*> field_value(const Value& object, std::string_view key) noexcept {
  if (!object.is_object()) {
    return Status(ReasonCode::RefusedMalformedInput, "object expected");
  }
  const Value* found = object.find(key);
  if (found == nullptr) {
    return Status(ReasonCode::RefusedMissingField, std::string(key) + " is required");
  }
  return found;
}

Result<const Value::array_type*> field_array(const Value& object, std::string_view key) noexcept {
  auto located = field_value(object, key);
  SNCF_TRY(located);
  const Value* value = located.value();
  if (value->is_array()) {
    return &value->as_array();
  }
  return type_refusal("array");
}

Result<const Value::object_type*> field_object(const Value& object, std::string_view key) noexcept {
  auto located = field_value(object, key);
  SNCF_TRY(located);
  const Value* value = located.value();
  if (value->is_object()) {
    return &value->as_object();
  }
  return type_refusal("object");
}

std::string to_hex_uint(std::uint64_t value, unsigned width) {
  std::string out;
  out.resize(width);
  for (unsigned i = 0; i < width; ++i) {
    const unsigned shift = (width - 1U - i) * 4U;
    out[i] = kHexDigits[(value >> shift) & 0xFU];
  }
  return out;
}

}  // namespace sncf
