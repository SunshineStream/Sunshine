/**
 * @file src/server/crypto.cpp
 * @brief Definitions for cryptography functions.
 */
#include <openssl/pem.h>

#include "src/server/crypto.h"

namespace crypto {
  using asn1_string_t = util::safe_ptr<ASN1_STRING, ASN1_STRING_free>;

  /**
   * @memberof crypto
   */
  cert_chain_t::
    cert_chain_t():
      _certs {},
      _cert_ctx { X509_STORE_CTX_new() } {}

  void
  cert_chain_t::add(x509_t &&cert) {
    x509_store_t x509_store { X509_STORE_new() };

    X509_STORE_add_cert(x509_store.get(), cert.get());
    _certs.emplace_back(std::make_pair(std::move(cert), std::move(x509_store)));
  }

  void
  cert_chain_t::clear() {
    _certs.clear();
  }

  static int
  openssl_verify_cb(int ok, X509_STORE_CTX *ctx) {
    int err_code = X509_STORE_CTX_get_error(ctx);

    switch (err_code) {
      // Expired or not-yet-valid certificates are fine. Sometimes Moonlight is running on embedded devices
      // that don't have accurate clocks (or haven't yet synchronized by the time Moonlight first runs).
      // This behavior also matches what GeForce Experience does.
      // TODO: Checking for X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY is a temporary workaround to get moonlight-embedded to work on the raspberry pi
      case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
      case X509_V_ERR_CERT_NOT_YET_VALID:
      case X509_V_ERR_CERT_HAS_EXPIRED:
        return 1;

      default:
        return ok;
    }
  }

  /**
   * @brief Verify the certificate chain.
   * When certificates from two or more instances of Moonlight have been added to x509_store_t,
   * only one of them will be verified by X509_verify_cert, resulting in only a single instance of
   * Moonlight to be able to use Sunshine
   *
   * To circumvent this, x509_store_t instance will be created for each instance of the certificates.
   * @param cert The certificate to verify.
   * @return nullptr if the certificate is valid, otherwise an error string.
   */
  const char *
  cert_chain_t::verify(x509_t::element_type *cert) {
    int err_code = 0;
    for (auto &[_, x509_store] : _certs) {
      auto fg = util::fail_guard([this]() {
        X509_STORE_CTX_cleanup(_cert_ctx.get());
      });

      X509_STORE_CTX_init(_cert_ctx.get(), x509_store.get(), cert, nullptr);
      X509_STORE_CTX_set_verify_cb(_cert_ctx.get(), openssl_verify_cb);

      // We don't care to validate the entire chain for the purposes of client auth.
      // Some versions of clients forked from Moonlight Embedded produce client certs
      // that OpenSSL doesn't detect as self-signed due to some X509v3 extensions.
      X509_STORE_CTX_set_flags(_cert_ctx.get(), X509_V_FLAG_PARTIAL_CHAIN);

      auto err = X509_verify_cert(_cert_ctx.get());

      if (err == 1) {
        return nullptr;
      }

      err_code = X509_STORE_CTX_get_error(_cert_ctx.get());

      if (err_code != X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT && err_code != X509_V_ERR_INVALID_CA) {
        return X509_verify_cert_error_string(err_code);
      }
    }

    return X509_verify_cert_error_string(err_code);
  }

  namespace cipher {

    static int
    init_decrypt_gcm(cipher_ctx_t &ctx, aes_t *key, aes_t *iv, bool padding) {
      ctx.reset(EVP_CIPHER_CTX_new());

      if (!ctx) {
        return -1;
      }

      if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1) {
        return -1;
      }

      if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, iv->size(), nullptr) != 1) {
        return -1;
      }

      if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key->data(), iv->data()) != 1) {
        return -1;
      }
      EVP_CIPHER_CTX_set_padding(ctx.get(), padding);

      return 0;
    }

    static int
    init_encrypt_gcm(cipher_ctx_t &ctx, aes_t *key, aes_t *iv, bool padding) {
      ctx.reset(EVP_CIPHER_CTX_new());

      // Gen 7 servers use 128-bit AES ECB
      if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1) {
        return -1;
      }

      if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, iv->size(), nullptr) != 1) {
        return -1;
      }

      if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key->data(), iv->data()) != 1) {
        return -1;
      }
      EVP_CIPHER_CTX_set_padding(ctx.get(), padding);

      return 0;
    }

    static int
    init_encrypt_cbc(cipher_ctx_t &ctx, aes_t *key, aes_t *iv, bool padding) {
      ctx.reset(EVP_CIPHER_CTX_new());

      // Gen 7 servers use 128-bit AES ECB
      if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, key->data(), iv->data()) != 1) {
        return -1;
      }

      EVP_CIPHER_CTX_set_padding(ctx.get(), padding);

      return 0;
    }

    int
    gcm_t::decrypt(const std::string_view &tagged_cipher, std::vector<std::uint8_t> &plaintext, aes_t *iv) {
      if (!decrypt_ctx && init_decrypt_gcm(decrypt_ctx, &key, iv, padding)) {
        return -1;
      }

      // Calling with cipher == nullptr results in a parameter change
      // without requiring a reallocation of the internal cipher ctx.
      if (EVP_DecryptInit_ex(decrypt_ctx.get(), nullptr, nullptr, nullptr, iv->data()) != 1) {
        return false;
      }

      auto cipher = tagged_cipher.substr(tag_size);
      auto tag = tagged_cipher.substr(0, tag_size);

      plaintext.resize(round_to_pkcs7_padded(cipher.size()));

      int update_outlen, final_outlen;

      if (EVP_DecryptUpdate(decrypt_ctx.get(), plaintext.data(), &update_outlen, (const std::uint8_t *) cipher.data(), cipher.size()) != 1) {
        return -1;
      }

      if (EVP_CIPHER_CTX_ctrl(decrypt_ctx.get(), EVP_CTRL_GCM_SET_TAG, tag.size(), const_cast<char *>(tag.data())) != 1) {
        return -1;
      }

      if (EVP_DecryptFinal_ex(decrypt_ctx.get(), plaintext.data() + update_outlen, &final_outlen) != 1) {
        return -1;
      }

      plaintext.resize(update_outlen + final_outlen);
      return 0;
    }

    /**
     * This function encrypts the given plaintext using the AES key in GCM mode. The initialization vector (IV) is also provided.
     * The function handles the creation and initialization of the encryption context, and manages the encryption process.
     * The resulting ciphertext and the GCM tag are written into the tagged_cipher buffer.
     */
    int
    gcm_t::encrypt(const std::string_view &plaintext, std::uint8_t *tag, std::uint8_t *ciphertext, aes_t *iv) {
      if (!encrypt_ctx && init_encrypt_gcm(encrypt_ctx, &key, iv, padding)) {
        return -1;
      }

      // Calling with cipher == nullptr results in a parameter change
      // without requiring a reallocation of the internal cipher ctx.
      if (EVP_EncryptInit_ex(encrypt_ctx.get(), nullptr, nullptr, nullptr, iv->data()) != 1) {
        return -1;
      }

      int update_outlen, final_outlen;

      // Encrypt into the caller's buffer
      if (EVP_EncryptUpdate(encrypt_ctx.get(), ciphertext, &update_outlen, (const std::uint8_t *) plaintext.data(), plaintext.size()) != 1) {
        return -1;
      }

      // GCM encryption won't ever fill ciphertext here but we have to call it anyway
      if (EVP_EncryptFinal_ex(encrypt_ctx.get(), ciphertext + update_outlen, &final_outlen) != 1) {
        return -1;
      }

      if (EVP_CIPHER_CTX_ctrl(encrypt_ctx.get(), EVP_CTRL_GCM_GET_TAG, tag_size, tag) != 1) {
        return -1;
      }

      return update_outlen + final_outlen;
    }

    int
    gcm_t::encrypt(const std::string_view &plaintext, std::uint8_t *tagged_cipher, aes_t *iv) {
      // This overload handles the common case of [GCM tag][cipher text] buffer layout
      return encrypt(plaintext, tagged_cipher, tagged_cipher + tag_size, iv);
    }

    int
    ecb_t::decrypt(const std::string_view &cipher, std::vector<std::uint8_t> &plaintext) {
      auto fg = util::fail_guard([this]() {
        EVP_CIPHER_CTX_reset(decrypt_ctx.get());
      });

      // Gen 7 servers use 128-bit AES ECB
      if (EVP_DecryptInit_ex(decrypt_ctx.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr) != 1) {
        return -1;
      }

      EVP_CIPHER_CTX_set_padding(decrypt_ctx.get(), padding);
      plaintext.resize(round_to_pkcs7_padded(cipher.size()));

      int update_outlen, final_outlen;

      if (EVP_DecryptUpdate(decrypt_ctx.get(), plaintext.data(), &update_outlen, (const std::uint8_t *) cipher.data(), cipher.size()) != 1) {
        return -1;
      }

      if (EVP_DecryptFinal_ex(decrypt_ctx.get(), plaintext.data() + update_outlen, &final_outlen) != 1) {
        return -1;
      }

      plaintext.resize(update_outlen + final_outlen);
      return 0;
    }

    int
    ecb_t::encrypt(const std::string_view &plaintext, std::vector<std::uint8_t> &cipher) {
      auto fg = util::fail_guard([this]() {
        EVP_CIPHER_CTX_reset(encrypt_ctx.get());
      });

      // Gen 7 servers use 128-bit AES ECB
      if (EVP_EncryptInit_ex(encrypt_ctx.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr) != 1) {
        return -1;
      }

      EVP_CIPHER_CTX_set_padding(encrypt_ctx.get(), padding);
      cipher.resize(round_to_pkcs7_padded(plaintext.size()));

      int update_outlen, final_outlen;

      // Encrypt into the caller's buffer
      if (EVP_EncryptUpdate(encrypt_ctx.get(), cipher.data(), &update_outlen, (const std::uint8_t *) plaintext.data(), plaintext.size()) != 1) {
        return -1;
      }

      if (EVP_EncryptFinal_ex(encrypt_ctx.get(), cipher.data() + update_outlen, &final_outlen) != 1) {
        return -1;
      }

      cipher.resize(update_outlen + final_outlen);
      return 0;
    }

    /**
     * This function encrypts the given plaintext using the AES key in CBC mode. The initialization vector (IV) is also provided.
     * The function handles the creation and initialization of the encryption context, and manages the encryption process.
     * The resulting ciphertext is written into the cipher buffer.
     */
    int
    cbc_t::encrypt(const std::string_view &plaintext, std::uint8_t *cipher, aes_t *iv) {
      if (!encrypt_ctx && init_encrypt_cbc(encrypt_ctx, &key, iv, padding)) {
        return -1;
      }

      // Calling with cipher == nullptr results in a parameter change
      // without requiring a reallocation of the internal cipher ctx.
      if (EVP_EncryptInit_ex(encrypt_ctx.get(), nullptr, nullptr, nullptr, iv->data()) != 1) {
        return false;
      }

      int update_outlen, final_outlen;

      // Encrypt into the caller's buffer
      if (EVP_EncryptUpdate(encrypt_ctx.get(), cipher, &update_outlen, (const std::uint8_t *) plaintext.data(), plaintext.size()) != 1) {
        return -1;
      }

      if (EVP_EncryptFinal_ex(encrypt_ctx.get(), cipher + update_outlen, &final_outlen) != 1) {
        return -1;
      }

      return update_outlen + final_outlen;
    }

    ecb_t::
      ecb_t(const aes_t &key, bool padding):
        cipher_t { EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_new(), key, padding } {}

    cbc_t::
      cbc_t(const aes_t &key, bool padding):
        cipher_t { nullptr, nullptr, key, padding } {}

    gcm_t::
      gcm_t(const crypto::aes_t &key, bool padding):
        cipher_t { nullptr, nullptr, key, padding } {}

  }  // namespace cipher

  aes_t
  gen_aes_key(const std::array<uint8_t, 16> &salt, const std::string_view &pin) {
    aes_t key(16);

    std::string salt_pin;
    salt_pin.reserve(salt.size() + pin.size());

    salt_pin.insert(std::end(salt_pin), std::begin(salt), std::end(salt));
    salt_pin.insert(std::end(salt_pin), std::begin(pin), std::end(pin));

    auto hsh = hash(salt_pin);

    std::copy(std::begin(hsh), std::begin(hsh) + key.size(), std::begin(key));

    return key;
  }

  sha256_t
  hash(const std::string_view &plaintext) {
    sha256_t hsh;
    EVP_Digest(plaintext.data(), plaintext.size(), hsh.data(), nullptr, EVP_sha256(), nullptr);
    return hsh;
  }

  x509_t
  x509(const std::string_view &x) {
    bio_t io { BIO_new(BIO_s_mem()) };

    BIO_write(io.get(), x.data(), x.size());

    x509_t p;
    PEM_read_bio_X509(io.get(), &p, nullptr, nullptr);

    return p;
  }

  pkey_t
  pkey(const std::string_view &k) {
    bio_t io { BIO_new(BIO_s_mem()) };

    BIO_write(io.get(), k.data(), k.size());

    pkey_t p = nullptr;
    PEM_read_bio_PrivateKey(io.get(), &p, nullptr, nullptr);

    return p;
  }

  std::string
  pem(x509_t &x509) {
    bio_t bio { BIO_new(BIO_s_mem()) };

    PEM_write_bio_X509(bio.get(), x509.get());
    BUF_MEM *mem_ptr;
    BIO_get_mem_ptr(bio.get(), &mem_ptr);

    return { mem_ptr->data, mem_ptr->length };
  }

  std::string
  pem(pkey_t &pkey) {
    bio_t bio { BIO_new(BIO_s_mem()) };

    PEM_write_bio_PrivateKey(bio.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr);
    BUF_MEM *mem_ptr;
    BIO_get_mem_ptr(bio.get(), &mem_ptr);

    return { mem_ptr->data, mem_ptr->length };
  }

  std::string_view
  signature(const x509_t &x) {
    // X509_ALGOR *_ = nullptr;

    const ASN1_BIT_STRING *asn1 = nullptr;
    X509_get0_signature(&asn1, nullptr, x.get());

    return { (const char *) asn1->data, (std::size_t) asn1->length };
  }

  std::string
  rand(std::size_t bytes) {
    std::string r;
    r.resize(bytes);

    RAND_bytes((uint8_t *) r.data(), r.size());

    return r;
  }

  std::vector<uint8_t>
  sign(const pkey_t &pkey, const std::string_view &data, const EVP_MD *md) {
    md_ctx_t ctx { EVP_MD_CTX_create() };

    if (EVP_DigestSignInit(ctx.get(), nullptr, md, nullptr, (EVP_PKEY *) pkey.get()) != 1) {
      return {};
    }

    if (EVP_DigestSignUpdate(ctx.get(), data.data(), data.size()) != 1) {
      return {};
    }

    std::size_t slen;
    if (EVP_DigestSignFinal(ctx.get(), nullptr, &slen) != 1) {
      return {};
    }

    std::vector<uint8_t> digest(slen);
    if (EVP_DigestSignFinal(ctx.get(), digest.data(), &slen) != 1) {
      return {};
    }

    return digest;
  }

  creds_t
  gen_creds(const std::string_view &cn, std::uint32_t key_bits) {
    x509_t x509 { X509_new() };
    pkey_ctx_t ctx { EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr) };
    pkey_t pkey;

    EVP_PKEY_keygen_init(ctx.get());
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), key_bits);
    EVP_PKEY_keygen(ctx.get(), &pkey);

    X509_set_version(x509.get(), 2);

    // Generate a real serial number to avoid SEC_ERROR_REUSED_ISSUER_AND_SERIAL with Firefox
    bignum_t serial { BN_new() };
    BN_rand(serial.get(), 159, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY);  // 159 bits to fit in 20 bytes in DER format
    BN_set_negative(serial.get(), 0);  // Serial numbers must be positive
    BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(x509.get()));

    constexpr auto year = 60 * 60 * 24 * 365;
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    X509_gmtime_adj(X509_get_notBefore(x509.get()), 0);
    X509_gmtime_adj(X509_get_notAfter(x509.get()), 20 * year);
#else
    asn1_string_t not_before { ASN1_STRING_dup(X509_get0_notBefore(x509.get())) };
    asn1_string_t not_after { ASN1_STRING_dup(X509_get0_notAfter(x509.get())) };

    X509_gmtime_adj(not_before.get(), 0);
    X509_gmtime_adj(not_after.get(), 20 * year);

    X509_set1_notBefore(x509.get(), not_before.get());
    X509_set1_notAfter(x509.get(), not_after.get());
#endif

    X509_set_pubkey(x509.get(), pkey.get());

    auto name = X509_get_subject_name(x509.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
      (const std::uint8_t *) cn.data(), cn.size(),
      -1, 0);

    X509_set_issuer_name(x509.get(), name);
    X509_sign(x509.get(), pkey.get(), EVP_sha256());

    return { pem(x509), pem(pkey) };
  }

  std::vector<uint8_t>
  sign256(const pkey_t &pkey, const std::string_view &data) {
    return sign(pkey, data, EVP_sha256());
  }

  bool
  verify(const x509_t &x509, const std::string_view &data, const std::string_view &signature, const EVP_MD *md) {
    auto pkey = X509_get0_pubkey(x509.get());

    md_ctx_t ctx { EVP_MD_CTX_create() };

    if (EVP_DigestVerifyInit(ctx.get(), nullptr, md, nullptr, pkey) != 1) {
      return false;
    }

    if (EVP_DigestVerifyUpdate(ctx.get(), data.data(), data.size()) != 1) {
      return false;
    }

    if (EVP_DigestVerifyFinal(ctx.get(), (const uint8_t *) signature.data(), signature.size()) != 1) {
      return false;
    }

    return true;
  }

  bool
  verify256(const x509_t &x509, const std::string_view &data, const std::string_view &signature) {
    return verify(x509, data, signature, EVP_sha256());
  }

  void
  md_ctx_destroy(EVP_MD_CTX *ctx) {
    EVP_MD_CTX_destroy(ctx);
  }

  std::string
  rand_alphabet(std::size_t bytes, const std::string_view &alphabet) {
    auto value = rand(bytes);

    for (std::size_t i = 0; i != value.size(); ++i) {
      value[i] = alphabet[value[i] % alphabet.length()];
    }
    return value;
  }

}  // namespace crypto
