/**
 * @file src/control_protocol.h
 * @brief Bounded parsing of GameStream control envelopes before decryption.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

// lib includes
#include <boost/endian/conversion.hpp>

namespace stream::control {
  constexpr std::size_t TAG_SIZE = 16;  ///< AES-GCM tag size on the control wire protocol.

  /**
   * @brief Validated encrypted envelope excluding the outer message type.
   */
  struct encrypted_message_t {
    std::uint32_t sequence;  ///< Sequence used to construct the message IV.
    std::string_view tagged_cipher;  ///< Bounded view of the authentication tag and ciphertext.
  };

  /**
   * @brief Validate an encrypted envelope before reading its sequence or ciphertext.
   * @param payload Wire bytes after the two-byte outer message type.
   * @return Parsed envelope, or no value for an incomplete or inconsistent length.
   */
  inline std::optional<encrypted_message_t> parse_encrypted(std::string_view payload) {
    constexpr std::size_t header_size = sizeof(std::uint16_t) + sizeof(std::uint32_t);
    constexpr std::size_t minimum_length = sizeof(std::uint32_t) + TAG_SIZE + 4;
    if (payload.size() < header_size) {
      return std::nullopt;
    }
    const auto *bytes = reinterpret_cast<const unsigned char *>(payload.data());
    const auto length = boost::endian::endian_load<std::uint16_t, 2, boost::endian::order::little>(bytes);
    if (length < minimum_length || length != payload.size() - sizeof(length)) {
      return std::nullopt;
    }
    const auto sequence = boost::endian::endian_load<std::uint32_t, 4, boost::endian::order::little>(bytes + sizeof(length));
    return encrypted_message_t {sequence, payload.substr(header_size)};
  }

  /**
   * @brief Validate the big-endian length prefix of legacy encrypted input.
   * @param payload Wire bytes after the outer message type.
   * @return Bounded tag and ciphertext, or no value for an invalid length.
   */
  inline std::optional<std::string_view> parse_legacy_input(std::string_view payload) {
    constexpr auto header_size = sizeof(std::uint32_t);
    if (payload.size() < header_size) {
      return std::nullopt;
    }
    const auto length = boost::endian::endian_load<std::uint32_t, 4, boost::endian::order::big>(reinterpret_cast<const unsigned char *>(payload.data()));
    if (length < TAG_SIZE || length != payload.size() - header_size) {
      return std::nullopt;
    }
    return payload.substr(header_size);
  }

  /**
   * @brief Read a complete decrypted secondary control header.
   * @param plaintext Complete authenticated plaintext including its secondary header.
   * @return Message type and actual authenticated payload, or no value for a short header.
   */
  inline std::optional<std::pair<std::uint16_t, std::string_view>> parse_plaintext(std::string_view plaintext) {
    constexpr std::size_t header_size = 4;
    if (plaintext.size() < header_size) {
      return std::nullopt;
    }
    const auto *bytes = reinterpret_cast<const unsigned char *>(plaintext.data());
    const auto type = boost::endian::endian_load<std::uint16_t, 2, boost::endian::order::little>(bytes);
    // Preserve legacy header semantics: dispatch is bounded by authenticated bytes,
    // not by the advisory secondary payloadLength field.
    return std::pair {type, plaintext.substr(header_size)};
  }
}  // namespace stream::control
