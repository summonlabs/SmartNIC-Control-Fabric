// Copyright 2026 Summon Software Labs.
#include <string>

#include "sncf/canonical.hpp"
#include "test_framework.hpp"

namespace {

sncf::Value sample() {
  return sncf::Value::object({
      {"alpha", sncf::Value::uint_value(1)},
      {"beta", sncf::Value::array({sncf::Value::string("x"), sncf::Value::boolean(true)})},
      {"gamma", sncf::Value::object({{"z", sncf::Value::int_value(-5)}, {"a", sncf::Value::null()}})},
  });
}

SNCF_TEST(canonical_encoding_is_sorted_and_deterministic) {
  const std::string first = sample().to_canonical();
  const std::string second = sample().to_canonical();
  SNCF_CHECK_EQ(first, second);
  SNCF_CHECK_EQ(first,
                std::string("{\"alpha\":1,\"beta\":[\"x\",true],\"gamma\":{\"a\":null,\"z\":-5}}"));
}

SNCF_TEST(canonical_round_trip_preserves_every_field) {
  const sncf::Value original = sample();
  const std::string text = original.to_canonical();
  auto parsed = sncf::parse_canonical(text);
  SNCF_REQUIRE(parsed.ok());
  SNCF_CHECK(parsed.value() == original);
  SNCF_CHECK_EQ(parsed.value().to_canonical(), text);
}

SNCF_TEST(canonical_rejects_duplicate_and_unordered_keys) {
  auto duplicate = sncf::parse_canonical("{\"a\":1,\"a\":2}");
  SNCF_CHECK(!duplicate.ok());
  SNCF_CHECK(duplicate.code() == sncf::ReasonCode::RefusedDuplicateKey);

  auto unordered = sncf::parse_canonical("{\"b\":1,\"a\":2}");
  SNCF_CHECK(!unordered.ok());
  SNCF_CHECK(unordered.code() == sncf::ReasonCode::RefusedNonCanonicalOrder);
}

SNCF_TEST(canonical_rejects_malformed_numbers_and_trailing_bytes) {
  for (const char* text : {"01", "-0", "1.5", "1e3", "+1", "-", "0x10"}) {
    auto parsed = sncf::parse_canonical(text);
    SNCF_CHECK(!parsed.ok());
  }
  auto trailing = sncf::parse_canonical("1 2");
  SNCF_CHECK(!trailing.ok());
  SNCF_CHECK(trailing.code() == sncf::ReasonCode::RefusedTrailingGarbage);
  auto whitespace = sncf::parse_canonical(" 1");
  SNCF_CHECK(!whitespace.ok());
}

SNCF_TEST(canonical_rejects_truncated_and_oversized_input) {
  for (const char* text : {"{\"a\":", "[1,", "\"abc", "{\"a\"", "[1,2", "tru"}) {
    auto parsed = sncf::parse_canonical(text);
    SNCF_CHECK(!parsed.ok());
  }
  sncf::ParseLimits limits;
  limits.max_bytes = 4;
  auto oversized = sncf::parse_canonical("[1,2,3,4,5,6,7,8]", limits);
  SNCF_CHECK(!oversized.ok());
  SNCF_CHECK(oversized.code() == sncf::ReasonCode::RefusedOversizedInput);

  std::string deep;
  for (int i = 0; i < 64; ++i) {
    deep.push_back('[');
  }
  auto too_deep = sncf::parse_canonical(deep);
  SNCF_CHECK(!too_deep.ok());
  SNCF_CHECK(too_deep.code() == sncf::ReasonCode::RefusedDepthExceeded);
}

SNCF_TEST(canonical_string_escapes_and_unicode) {
  const sncf::Value value = sncf::Value::string("line\nbreak\t\"quoted\" \\ slash \u00e9");
  const std::string text = value.to_canonical();
  auto parsed = sncf::parse_canonical(text);
  SNCF_REQUIRE(parsed.ok());
  SNCF_CHECK(parsed.value() == value);

  auto surrogate = sncf::parse_canonical("\"\\ud83d\\ude00\"");
  SNCF_REQUIRE(surrogate.ok());
  SNCF_CHECK_EQ(surrogate.value().as_string(), std::string("\xf0\x9f\x98\x80"));

  auto lone = sncf::parse_canonical("\"\\ud83d\"");
  SNCF_CHECK(!lone.ok());
  auto control = sncf::parse_canonical("\"a\tb\"");
  SNCF_CHECK(!control.ok());
  auto bad_escape = sncf::parse_canonical("\"\\q\"");
  SNCF_CHECK(!bad_escape.ok());
}

SNCF_TEST(canonical_field_helpers_distinguish_absence_from_zero) {
  const sncf::Value object = sncf::Value::object({{"present_zero", sncf::Value::uint_value(0)}});
  auto present = sncf::field_uint(object, "present_zero");
  SNCF_REQUIRE(present.ok());
  SNCF_CHECK_EQ(present.value(), 0U);
  auto absent = sncf::field_uint(object, "missing");
  SNCF_CHECK(!absent.ok());
  SNCF_CHECK(absent.code() == sncf::ReasonCode::RefusedMissingField);
  auto wrong_type = sncf::field_uint(object, "present_zero");
  SNCF_CHECK(wrong_type.ok());
  const sncf::Value text = sncf::Value::object({{"k", sncf::Value::string("v")}});
  auto as_uint = sncf::field_uint(text, "k");
  SNCF_CHECK(!as_uint.ok());
  SNCF_CHECK(as_uint.code() == sncf::ReasonCode::RefusedInvalidEnumValue);
}

SNCF_TEST(canonical_utf8_validation) {
  SNCF_CHECK(sncf::is_valid_utf8("plain"));
  SNCF_CHECK(sncf::is_valid_utf8("\xc3\xa9"));
  SNCF_CHECK(!sncf::is_valid_utf8("\xff"));
  SNCF_CHECK(!sncf::is_valid_utf8("\xc3"));
  SNCF_CHECK(!sncf::is_valid_utf8("\xed\xa0\x80"));
}

SNCF_TEST(canonical_digest_is_stable_across_insertion_order) {
  const sncf::Value a = sncf::Value::object({{"x", sncf::Value::uint_value(1)}, {"y", sncf::Value::uint_value(2)}});
  const sncf::Value b = sncf::Value::object({{"y", sncf::Value::uint_value(2)}, {"x", sncf::Value::uint_value(1)}});
  SNCF_CHECK(a.digest() == b.digest());
}

}  // namespace
