#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <src/audio.h>
#include <src/crypto.h>
#include <src/stream.h>

namespace audio_transport_test {

struct packet_writer {
  explicit packet_writer(const std::size_t capacity):
    destination(capacity, 0xA5),
    key(16, 0),
    iv(16, 0),
    cipher(key, true) {}

  bool is_filled_with(const std::uint8_t value) const {
    return std::all_of(destination.begin(), destination.end(), [value](const std::uint8_t byte) {
      return byte == value;
    });
  }

  int encode(const bool encrypted, const audio::buffer_t &plaintext, const std::size_t capacity) {
    return stream::encode_audio(
      encrypted,
      plaintext,
      destination.data(),
      capacity,
      iv,
      cipher
    );
  }

  std::vector<std::uint8_t> destination;
  crypto::aes_t key;
  crypto::aes_t iv;
  crypto::cipher::cbc_t cipher;
};

template<typename Test>
void for_each_maximum_packet_case(Test &&test) {
  constexpr std::size_t plaintext_size = MAX_DOLBY_PACKET_SIZE;
  constexpr auto encrypted_size = crypto::cipher::round_to_pkcs7_padded(plaintext_size);
  const audio::buffer_t plaintext(plaintext_size, 0x5A);

  for (const bool encrypted : {false, true}) {
    const auto required_capacity = encrypted ? encrypted_size : plaintext_size;
    packet_writer writer(required_capacity);
    test(encrypted, required_capacity, plaintext, writer);
  }
}

}  // namespace audio_transport_test
