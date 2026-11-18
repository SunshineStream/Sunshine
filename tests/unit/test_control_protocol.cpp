/**
 * @file tests/unit/test_control_protocol.cpp
 * @brief Regression tests for bounds checked control-channel envelopes.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// local includes
#include <src/control_protocol.h>

/**
 * @brief Short encrypted envelopes and inconsistent advertised lengths are rejected.
 */
TEST(ControlEnvelopeTest, RejectsTruncatedEncryptedHeadersAndCiphertext) {
  std::string message(26, '\0');
  message[0] = 24;
  for (std::size_t size = 0; size < message.size(); ++size) {
    SCOPED_TRACE(size);
    EXPECT_FALSE(stream::control::parse_encrypted(std::string_view(message).substr(0, size)));
  }
  message.push_back('\0');
  EXPECT_FALSE(stream::control::parse_encrypted(message));
  message[0] = static_cast<char>(0xff);
  message[1] = static_cast<char>(0xff);
  EXPECT_FALSE(stream::control::parse_encrypted(message));
  message.resize(26);
  message[0] = 23;
  message[1] = 0;
  EXPECT_FALSE(stream::control::parse_encrypted(message));
}

/**
 * @brief Valid encrypted envelopes preserve their sequence and exact ciphertext bounds.
 */
TEST(ControlEnvelopeTest, AcceptsEncryptedEnvelopeWithUnalignedStorage) {
  std::string storage(27, '\0');
  storage[1] = 24;
  storage[3] = 0x12;
  storage[4] = 0x34;
  storage[5] = 0x56;
  storage[6] = 0x78;
  const auto message = stream::control::parse_encrypted(std::string_view(storage).substr(1));
  ASSERT_TRUE(message);
  EXPECT_EQ(message->sequence, 0x78563412U);
  EXPECT_EQ(message->tagged_cipher.data(), storage.data() + 7);
  EXPECT_EQ(message->tagged_cipher.size(), 20);
}

/**
 * @brief The largest wire-representable encrypted message remains valid.
 */
TEST(ControlEnvelopeTest, AcceptsMaximumEncryptedWireLength) {
  std::string message(65537, '\0');
  message[0] = static_cast<char>(0xff);
  message[1] = static_cast<char>(0xff);
  const auto parsed = stream::control::parse_encrypted(message);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->tagged_cipher.size(), 65531);
}

/**
 * @brief Legacy prefixes must describe the complete available tag and ciphertext.
 */
TEST(ControlEnvelopeTest, RejectsInvalidLegacyLengths) {
  std::string message(20, '\0');
  message[3] = 16;
  for (std::size_t size = 0; size < message.size(); ++size) {
    SCOPED_TRACE(size);
    EXPECT_FALSE(stream::control::parse_legacy_input(std::string_view(message).substr(0, size)));
  }
  for (const auto length : {0U, 15U, 17U, 0x7fffffffU, 0x80000000U, 0xffffffffU}) {
    SCOPED_TRACE(length);
    for (unsigned index = 0; index < 4; ++index) {
      message[index] = static_cast<char>(length >> (24 - 8 * index));
    }
    EXPECT_FALSE(stream::control::parse_legacy_input(message));
  }
}

/**
 * @brief A bounded legacy message may contain only a tag or nonempty ciphertext.
 */
TEST(ControlEnvelopeTest, PreservesValidLegacyCiphertext) {
  for (const auto size : {16U, 32U, 256U}) {
    std::string message(size + 4, '\0');
    message[2] = static_cast<char>(size >> 8);
    message[3] = static_cast<char>(size);
    const auto parsed = stream::control::parse_legacy_input(message);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->size(), size);
    EXPECT_EQ(parsed->data(), message.data() + 4);
  }
}

/**
 * @brief Truncated decrypted secondary headers are rejected before reading their type.
 */
TEST(ControlEnvelopeTest, RejectsMalformedSecondaryHeaders) {
  const std::string message {"\x06\x02\x01\0x", 5};
  for (std::size_t size = 0; size < 4; ++size) {
    EXPECT_FALSE(stream::control::parse_plaintext(std::string_view(message).substr(0, size)));
  }
}

/**
 * @brief Valid empty and nonempty inner messages retain their type and payload.
 */
TEST(ControlEnvelopeTest, AcceptsValidSecondaryHeaders) {
  const std::string empty {"\x02\x03\0\0", 4};
  const auto parsed_empty = stream::control::parse_plaintext(empty);
  ASSERT_TRUE(parsed_empty);
  EXPECT_EQ(parsed_empty->first, 0x0302);
  EXPECT_TRUE(parsed_empty->second.empty());
  const std::string nonempty {"\x06\x02\x01\0x", 5};
  const auto parsed = stream::control::parse_plaintext(nonempty);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->first, 0x0206);
  EXPECT_EQ(parsed->second, "x");
}

/**
 * @brief Advisory secondary lengths cannot extend or truncate the authenticated payload.
 */
TEST(ControlEnvelopeTest, BoundsSecondaryPayloadByAuthenticatedBytes) {
  std::string message {"\x06\x02\0\0x", 5};
  for (const auto length : {0U, 1U, 2U, 65535U}) {
    message[2] = static_cast<char>(length);
    message[3] = static_cast<char>(length >> 8);
    const auto parsed = stream::control::parse_plaintext(message);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->first, 0x0206);
    EXPECT_EQ(parsed->second, "x");
  }
}
