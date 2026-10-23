/**
 * @file tests/unit/test_crypto.cpp
 * @brief Test src/crypto.*.
 */
// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
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

TEST(CryptoTest, Fingerprint256HashesTheDerCertificate) {
  const auto creds = crypto::gen_creds("Sunshine Fingerprint Client", 2048);
  const auto cert = crypto::x509(creds.x509);
  ASSERT_NE(cert.get(), nullptr);

  unsigned char *der = nullptr;
  const auto der_size = i2d_X509(cert.get(), &der);
  ASSERT_GT(der_size, 0);
  const std::string der_bytes {reinterpret_cast<const char *>(der), static_cast<std::size_t>(der_size)};
  OPENSSL_free(der);

  const auto fingerprint = crypto::fingerprint256(creds.x509);
  EXPECT_EQ(fingerprint.size(), 64);
  EXPECT_EQ(fingerprint, util::hex_vec(crypto::hash(der_bytes), true));
  EXPECT_NE(fingerprint, crypto::fingerprint256(crypto::gen_creds("Sunshine Other Client", 2048).x509));
}

TEST(CryptoTest, Fingerprint256OfInvalidCertificateIsEmpty) {
  EXPECT_TRUE(crypto::fingerprint256("").empty());
  EXPECT_TRUE(crypto::fingerprint256("-----BEGIN CERTIFICATE-----\nnot a certificate\n-----END CERTIFICATE-----\n").empty());
}
