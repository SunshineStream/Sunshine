#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <src/crypto.h>
#include <src/stream.h>

namespace audio_transport_test {

struct packet_writer {
  explicit packet_writer(const std::size_t capacity):
    destination(capacity, 0xA5),
    key(16, 0),
    iv(16, 0),
    cipher(key, true) {}

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

}  // namespace audio_transport_test
