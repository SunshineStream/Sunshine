/**
 * @file tests/unit/test_crypto.cpp
 * @brief Test src/crypto.*.
 */
// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <array>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <openssl/x509.h>

// local includes
#include <src/crypto.h>

TEST(CryptoTest, GeneratedCredentialsExposeSubjectAndVerifySignatures) {
  constexpr std::string_view common_name = "Sunshine Test Host";
  constexpr std::string_view payload = "payload";

  auto creds = crypto::gen_creds(common_name, 2048);
  ASSERT_FALSE(creds.x509.empty());
  ASSERT_FALSE(creds.pkey.empty());

  auto cert = crypto::x509(creds.x509);
  auto pkey = crypto::pkey(creds.pkey);
  ASSERT_NE(cert.get(), nullptr);
  ASSERT_NE(pkey.get(), nullptr);

  const auto subject = X509_get_subject_name(cert.get());
  ASSERT_NE(subject, nullptr);

  const auto common_name_index = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
  ASSERT_GE(common_name_index, 0);
  const auto common_name_entry = X509_NAME_get_entry(subject, common_name_index);
  ASSERT_NE(common_name_entry, nullptr);
  const auto common_name_data = X509_NAME_ENTRY_get_data(common_name_entry);
  ASSERT_NE(common_name_data, nullptr);

  unsigned char *common_name_utf8 = nullptr;
  const auto free_common_name = util::fail_guard([&common_name_utf8] {
    OPENSSL_free(common_name_utf8);
  });
  const auto common_name_length = ASN1_STRING_to_UTF8(&common_name_utf8, common_name_data);
  ASSERT_GE(common_name_length, 0);
  ASSERT_NE(common_name_utf8, nullptr);
  const std::string_view parsed_common_name {
    reinterpret_cast<const char *>(common_name_utf8),
    static_cast<std::size_t>(common_name_length)
  };
  ASSERT_EQ(parsed_common_name, common_name);

  ASSERT_FALSE(crypto::signature(cert).empty());

  const auto signature = crypto::sign256(pkey, payload);
  ASSERT_FALSE(signature.empty());
  ASSERT_TRUE(crypto::verify256(cert, payload, {reinterpret_cast<const char *>(signature.data()), signature.size()}));
}

TEST(CryptoTest, CertificateSignaturePreservesBinaryBytes) {
  constexpr std::uint32_t key_bits = 2048;
  const auto creds = crypto::gen_creds("Sunshine Signature Test", key_bits);
  const auto cert = crypto::x509(creds.x509);
  ASSERT_NE(cert.get(), nullptr);

  const auto der_size = i2d_X509(cert.get(), nullptr);
  ASSERT_GT(der_size, 0);
  std::vector<unsigned char> der(static_cast<std::size_t>(der_size));
  auto *write_cursor = der.data();
  ASSERT_EQ(i2d_X509(cert.get(), &write_cursor), der_size);

  // The final DER field contains the RSA signature. Include every byte value,
  // including embedded NULs, to verify that extraction preserves binary data.
  constexpr std::size_t signature_size = key_bits / 8;
  ASSERT_GT(der.size(), signature_size);
  for (std::size_t i = 0; i < signature_size; ++i) {
    der[der.size() - signature_size + i] = static_cast<unsigned char>(i);
  }

  const auto *read_cursor = der.data();
  const crypto::x509_t binary_cert {d2i_X509(nullptr, &read_cursor, der_size)};
  ASSERT_NE(binary_cert.get(), nullptr);
  const std::string_view expected_signature {
    reinterpret_cast<const char *>(der.data() + der.size() - signature_size),
    signature_size
  };
  EXPECT_EQ(crypto::signature(binary_cert), expected_signature);
}

TEST(CryptoTest, CertificateChainRequiresExactPairedIdentity) {
  const auto issuer_credentials = test_utils::certificates::generate_ca_credentials();
  const auto derived_credentials = test_utils::certificates::generate_derived_leaf(issuer_credentials);
  auto paired_certificate = crypto::x509(issuer_credentials.x509);
  auto derived_certificate = crypto::x509(derived_credentials.x509);

  crypto::cert_chain_t certificate_chain;
  certificate_chain.add(crypto::x509(issuer_credentials.x509));

  EXPECT_EQ(certificate_chain.verify(paired_certificate.get()), nullptr);
  EXPECT_NE(certificate_chain.verify(derived_certificate.get()), nullptr);
}

/**
 * @brief GCM rejects incomplete tags and invalid key or IV inputs without plaintext.
 */
TEST(CryptoTest, GcmRejectsMalformedInputsBeforeReadingCiphertext) {
  crypto::cipher::gcm_t cipher {crypto::aes_t(16, 0), false};
  crypto::aes_t iv(12, 0);
  std::vector<std::uint8_t> plaintext {1, 2, 3};
  for (std::size_t size = 0; size < crypto::cipher::tag_size; ++size) {
    const std::string truncated(size, '\0');
    EXPECT_EQ(cipher.decrypt(truncated, plaintext, &iv), -1);
    EXPECT_TRUE(plaintext.empty());
  }
  const std::string tag(crypto::cipher::tag_size, '\0');
  EXPECT_EQ(cipher.decrypt(tag, plaintext, nullptr), -1);
  crypto::aes_t empty_iv;
  EXPECT_EQ(cipher.decrypt(tag, plaintext, &empty_iv), -1);
  for (const auto size : {0U, 15U, 17U}) {
    crypto::cipher::gcm_t invalid_key {crypto::aes_t(size, 0), false};
    EXPECT_EQ(invalid_key.decrypt(tag, plaintext, &iv), -1);
    EXPECT_TRUE(plaintext.empty());
  }
}

/**
 * @brief Authentication failure clears output and does not poison later valid messages.
 */
TEST(CryptoTest, GcmAuthenticatesEmptyAndNonemptyMessagesAndRecoversAfterFailure) {
  crypto::aes_t key(16, 0x42);
  crypto::aes_t iv(12, 0x24);
  crypto::cipher::gcm_t sender {key, false};
  crypto::cipher::gcm_t receiver {key, false};
  for (const std::string &message : {std::string {}, std::string {"control message"}}) {
    std::vector<std::uint8_t> ciphertext(crypto::cipher::tag_size + message.size() + 16);
    ASSERT_EQ(sender.encrypt(message, ciphertext.data(), &iv), message.size());
    ciphertext.resize(crypto::cipher::tag_size + message.size());
    const std::string valid(ciphertext.begin(), ciphertext.end());
    std::vector<std::uint8_t> plaintext;
    ASSERT_EQ(receiver.decrypt(valid, plaintext, &iv), 0);
    EXPECT_EQ(std::string(plaintext.begin(), plaintext.end()), message);
    auto forged = valid;
    forged[0] = valid[0] == 'x' ? 'y' : 'x';
    EXPECT_EQ(receiver.decrypt(forged, plaintext, &iv), -1);
    EXPECT_TRUE(plaintext.empty());
    ASSERT_EQ(receiver.decrypt(valid, plaintext, &iv), 0);
    EXPECT_EQ(std::string(plaintext.begin(), plaintext.end()), message);
  }
}
