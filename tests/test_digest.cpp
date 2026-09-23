// Copyright 2026 Summon Software Labs.
#include "sncf/digest.hpp"
#include "sncf/ids.hpp"
#include "test_framework.hpp"

namespace {

using sncf::Crc32cAccumulator;
using sncf::crc32c;
using sncf::Digest256;
using sncf::Sha256;

SNCF_TEST(digest_sha256_known_vectors) {
  SNCF_CHECK_EQ(Sha256::hash("").to_hex(),
                std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  SNCF_CHECK_EQ(Sha256::hash("abc").to_hex(),
                std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  SNCF_CHECK_EQ(
      Sha256::hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").to_hex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  std::string million(1000000U, 'a');
  SNCF_CHECK_EQ(Sha256::hash(million).to_hex(),
                std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

SNCF_TEST(digest_sha256_streaming_matches_one_shot) {
  std::string payload;
  for (int i = 0; i < 5000; ++i) {
    payload.push_back(static_cast<char>('a' + (i % 26)));
  }
  Sha256 hasher;
  std::size_t offset = 0;
  std::size_t step = 1;
  while (offset < payload.size()) {
    const std::size_t take = (offset + step <= payload.size()) ? step : payload.size() - offset;
    hasher.update(payload.data() + offset, take);
    offset += take;
    step = (step * 3U) % 977U + 1U;
  }
  SNCF_CHECK(hasher.finish() == Sha256::hash(payload));
}

SNCF_TEST(digest_crc32c_known_vectors) {
  SNCF_CHECK_EQ(crc32c("", 0), 0U);
  SNCF_CHECK_EQ(crc32c("123456789"), 0xE3069283U);
  SNCF_CHECK_EQ(crc32c("abc"), 0x364B3FB7U);
  Crc32cAccumulator accumulator;
  accumulator.update("12345", 5);
  accumulator.update("6789", 4);
  SNCF_CHECK_EQ(accumulator.value(), 0xE3069283U);
  accumulator.reset();
  SNCF_CHECK_EQ(accumulator.value(), 0U);
}

SNCF_TEST(digest_hex_parsing_is_strict) {
  const auto valid = Digest256::from_hex(std::string(64, 'a'));
  SNCF_CHECK(valid.has_value());
  SNCF_CHECK_EQ(valid->to_hex(), std::string(64, 'a'));
  SNCF_CHECK(!Digest256::from_hex(std::string(63, 'a')).has_value());
  SNCF_CHECK(!Digest256::from_hex(std::string(65, 'a')).has_value());
  SNCF_CHECK(!Digest256::from_hex(std::string(64, 'A')).has_value());
  SNCF_CHECK(!Digest256::from_hex(std::string(64, 'z')).has_value());
  SNCF_CHECK(Digest256{}.is_zero());
  SNCF_CHECK(!valid->is_zero());
}

SNCF_TEST(digest_capability_mask_round_trip) {
  sncf::CapabilityMask mask;
  mask.insert(sncf::CapabilityCode::PacketParsing);
  mask.insert(sncf::CapabilityCode::QueueSteering);
  const std::string hex = mask.to_hex();
  const auto parsed = sncf::CapabilityMask::from_hex(hex);
  SNCF_REQUIRE(parsed.has_value());
  SNCF_CHECK(*parsed == mask);
  SNCF_CHECK(mask.contains(sncf::CapabilityCode::PacketParsing));
  SNCF_CHECK(!mask.contains(sncf::CapabilityCode::RateLimiting));
  SNCF_CHECK(sncf::CapabilityMask{}.to_hex() == "0");
  SNCF_CHECK(!sncf::CapabilityMask::from_hex("zz").has_value());
}

}  // namespace
