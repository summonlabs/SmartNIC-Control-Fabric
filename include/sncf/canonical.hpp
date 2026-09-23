// Canonical, deterministic JSON value model, writer and strict parser.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_CANONICAL_HPP
#define SNCF_CANONICAL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "sncf/digest.hpp"
#include "sncf/status.hpp"
#include "sncf/strong.hpp"

namespace sncf {

/// Bounds applied to every decode path so that hostile or corrupt input cannot
/// drive unbounded allocation, recursion or work.
struct ParseLimits {
  std::size_t max_bytes = 8U * 1024U * 1024U;
  std::size_t max_string_bytes = 64U * 1024U;
  std::size_t max_depth = 32U;
  std::size_t max_array_items = 1U << 20U;
  std::size_t max_object_fields = 1U << 16U;
  std::size_t max_key_bytes = 256U;
  std::size_t max_nodes = 1U << 18U;
};

/// A JSON value restricted to the canonical subset this runtime emits: objects
/// with unique keys kept sorted byte-wise, arrays, strings (valid UTF-8),
/// booleans, null, and integers only. Floating point is deliberately absent:
/// every quantity in this runtime is an exact integer with explicit units.
class Value {
 public:
  enum class Type : std::uint8_t { Null = 0, Bool = 1, UInt = 2, Int = 3, String = 4, Array = 5, Object = 6 };

  using array_type = std::vector<Value>;
  using field_type = std::pair<std::string, Value>;
  using object_type = std::vector<field_type>;

  Value() noexcept = default;

  static Value null() noexcept { return Value(); }
  static Value boolean(bool value) noexcept;
  static Value uint_value(std::uint64_t value) noexcept;
  static Value int_value(std::int64_t value) noexcept;
  static Value string(std::string text);
  static Value string(std::string_view text) { return string(std::string(text)); }
  static Value string(const char* text) { return string(std::string(text)); }
  static Value array(array_type items);
  static Value object(object_type fields);

  [[nodiscard]] Type type() const noexcept { return static_cast<Type>(storage_.index()); }
  [[nodiscard]] bool is_null() const noexcept { return type() == Type::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return type() == Type::Bool; }
  [[nodiscard]] bool is_uint() const noexcept { return type() == Type::UInt; }
  [[nodiscard]] bool is_int() const noexcept { return type() == Type::Int; }
  [[nodiscard]] bool is_string() const noexcept { return type() == Type::String; }
  [[nodiscard]] bool is_array() const noexcept { return type() == Type::Array; }
  [[nodiscard]] bool is_object() const noexcept { return type() == Type::Object; }

  [[nodiscard]] bool as_bool() const noexcept { return std::get<1>(storage_); }
  [[nodiscard]] std::uint64_t as_uint() const noexcept { return std::get<2>(storage_); }
  [[nodiscard]] std::int64_t as_int() const noexcept { return std::get<3>(storage_); }
  [[nodiscard]] const std::string& as_string() const noexcept { return std::get<4>(storage_); }
  [[nodiscard]] const array_type& as_array() const noexcept { return std::get<5>(storage_); }
  [[nodiscard]] const object_type& as_object() const noexcept { return std::get<6>(storage_); }
  [[nodiscard]] array_type& as_array() noexcept { return std::get<5>(storage_); }
  [[nodiscard]] object_type& as_object() noexcept { return std::get<6>(storage_); }

  /// Binary search over the sorted field vector. Returns nullptr when absent.
  [[nodiscard]] const Value* find(std::string_view key) const noexcept;

  /// Deterministic textual encoding. Two equal values always encode to identical
  /// bytes, and the encoding round-trips through parse.
  [[nodiscard]] std::string to_canonical() const;
  void append_canonical(std::string& out) const;

  [[nodiscard]] Digest256 digest() const;

  friend bool operator==(const Value& a, const Value& b) noexcept { return a.storage_ == b.storage_; }
  friend bool operator!=(const Value& a, const Value& b) noexcept { return !(a == b); }

 private:
  using storage_type =
      std::variant<std::monostate, bool, std::uint64_t, std::int64_t, std::string, array_type, object_type>;
  storage_type storage_;
};

/// Strict canonical JSON decode. Rejects trailing bytes, duplicate keys,
/// non-canonical numbers, over-deep nesting, invalid UTF-8 and oversized input,
/// each with a distinct stable reason code.
[[nodiscard]] Result<Value> parse_canonical(std::string_view text, const ParseLimits& limits = {}) noexcept;

/// True when the byte sequence is well-formed UTF-8.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

// --- typed extraction helpers ---------------------------------------------
// Each helper distinguishes absent, wrong-typed and out-of-range, and never
// substitutes a default for missing evidence.

[[nodiscard]] Result<std::uint64_t> field_uint(const Value& object, std::string_view key) noexcept;
[[nodiscard]] Result<std::int64_t> field_int(const Value& object, std::string_view key) noexcept;
[[nodiscard]] Result<bool> field_bool(const Value& object, std::string_view key) noexcept;
[[nodiscard]] Result<std::string> field_string(const Value& object, std::string_view key) noexcept;
[[nodiscard]] Result<const Value*> field_value(const Value& object, std::string_view key) noexcept;
[[nodiscard]] Result<const Value::array_type*> field_array(const Value& object, std::string_view key) noexcept;
[[nodiscard]] Result<const Value::object_type*> field_object(const Value& object, std::string_view key) noexcept;

/// Encode/decode an unsigned integer as a lowercase hex string for digests and
/// tokens in the export surface.
[[nodiscard]] std::string to_hex_uint(std::uint64_t value, unsigned width);

}  // namespace sncf

#endif  // SNCF_CANONICAL_HPP
