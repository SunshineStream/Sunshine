/**
 * @file src/crypto.h
 * @brief Declarations for cryptography functions.
 */
#pragma once

// standard includes
#include <array>

// lib includes
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

// local includes
#include "utility.h"

namespace crypto {
  /**
   * @brief PEM-encoded certificate and private key pair.
   */
  struct creds_t {
    std::string x509;  ///< PEM-encoded X.509 certificate.
    std::string pkey;  ///< Private key PEM string or path.
  };

  /**
   * @brief Destroy an OpenSSL message digest context.
   *
   * @param ctx Message digest context.
   */
  void md_ctx_destroy(EVP_MD_CTX *ctx);

  /**
   * @brief Fixed-size SHA-256 digest byte array.
   */
  using sha256_t = std::array<std::uint8_t, SHA256_DIGEST_LENGTH>;

  /**
   * @brief Byte buffer containing AES key material.
   */
  using aes_t = std::vector<std::uint8_t>;
  /**
   * @brief Owning pointer for an OpenSSL X.509 certificate.
   */
  using x509_t = util::safe_ptr<X509, &X509_free>;
  /**
   * @brief Owning pointer for an OpenSSL certificate store.
   */
  using x509_store_t = util::safe_ptr<X509_STORE, &X509_STORE_free>;
  /**
   * @brief Owning pointer for an OpenSSL certificate verification context.
   */
  using x509_store_ctx_t = util::safe_ptr<X509_STORE_CTX, &X509_STORE_CTX_free>;
  /**
   * @brief Owning pointer for an OpenSSL cipher context.
   */
  using cipher_ctx_t = util::safe_ptr<EVP_CIPHER_CTX, &EVP_CIPHER_CTX_free>;
  /**
   * @brief Owning pointer for an OpenSSL message digest context.
   */
  using md_ctx_t = util::safe_ptr<EVP_MD_CTX, &md_ctx_destroy>;
  /**
   * @brief Owning pointer for an OpenSSL BIO chain.
   */
  using bio_t = util::safe_ptr<BIO, &BIO_free_all>;
  /**
   * @brief Owning pointer for an OpenSSL public or private key.
   */
  using pkey_t = util::safe_ptr<EVP_PKEY, &EVP_PKEY_free>;
  /**
   * @brief Owning pointer for an OpenSSL key-generation context.
   */
  using pkey_ctx_t = util::safe_ptr<EVP_PKEY_CTX, &EVP_PKEY_CTX_free>;
  /**
   * @brief Owning pointer for an OpenSSL BIGNUM.
   */
  using bignum_t = util::safe_ptr<BIGNUM, &BN_free>;

  /**
   * @brief Hashes the given plaintext using SHA-256.
   * @param plaintext
   * @return The SHA-256 hash of the plaintext.
   */
  sha256_t hash(const std::string_view &plaintext);

  /**
   * @brief Derive the AES key used by the pairing protocol.
   *
   * @param salt Random salt used when deriving the pairing secret.
   * @param pin PIN supplied by the client during pairing.
   * @return Parsed or generated OpenSSL object or PEM data.
   */
  aes_t gen_aes_key(const std::array<uint8_t, 16> &salt, const std::string_view &pin);
  /**
   * @brief Parse PEM text into an X.509 certificate object.
   *
   * @param x Certificate object or PEM text, depending on the overload.
   * @return Parsed or generated OpenSSL object or PEM data.
   */
  x509_t x509(const std::string_view &x);
  /**
   * @brief Parse PEM text into an OpenSSL private key object.
   *
   * @param k PEM text containing a private key.
   * @return Parsed or generated OpenSSL object or PEM data.
   */
  pkey_t pkey(const std::string_view &k);
  /**
   * @brief Serialize an OpenSSL object to PEM text.
   *
   * @param x509 X.509 certificate object or PEM data.
   * @return Parsed or generated OpenSSL object or PEM data.
   */
  std::string pem(x509_t &x509);
  /**
   * @brief Serialize an OpenSSL object to PEM text.
   *
   * @param pkey Private key PEM data or private key file path.
   * @return Parsed or generated OpenSSL object or PEM data.
   */
  std::string pem(pkey_t &pkey);

  /**
   * @brief Sign data with SHA-256.
   *
   * @param pkey Private key PEM data or private key file path.
   * @param data Payload or state data to serialize, deserialize, or forward.
   * @return Number of bytes written, signature bytes, or an error status depending on the overload.
   */
  std::vector<uint8_t> sign256(const pkey_t &pkey, const std::string_view &data);
  /**
   * @brief Verify a SHA-256 signature with the certificate public key.
   *
   * @param x509 X.509 certificate object or PEM data.
   * @param data Payload or state data to serialize, deserialize, or forward.
   * @param signature Signature bytes to verify or encode.
   * @return True when the signature is valid for the supplied data.
   */
  bool verify256(const x509_t &x509, const std::string_view &data, const std::string_view &signature);

  /**
   * @brief Generate a self-signed certificate and private key for Sunshine pairing.
   *
   * @param cn Common name to place in the generated certificate.
   * @param key_bits Size in bits of the generated RSA key.
   * @return Parsed or generated OpenSSL object or PEM data.
   */
  creds_t gen_creds(const std::string_view &cn, std::uint32_t key_bits);

  /**
   * @brief Return the certificate signature bytes using the available OpenSSL length API.
   *
   * @param x Certificate containing the signature.
   * @return A view of the binary signature owned by the certificate.
   */
  std::string_view signature(const x509_t &x);

  /**
   * @brief Compute the SHA-256 fingerprint of a PEM certificate.
   *
   * The digest covers the DER encoding, as `openssl x509 -fingerprint -sha256` does.
   *
   * @param pem PEM text containing an X.509 certificate.
   * @return Uppercase hexadecimal digest without separators, or an empty string when the PEM is invalid.
   */
  std::string fingerprint256(const std::string_view &pem);

  /**
   * @brief Generate cryptographically secure random bytes.
   *
   * @param bytes Number of random bytes to generate.
   * @return Random bytes or text generated by OpenSSL.
   */
  std::string rand(std::size_t bytes);
  /**
   * @brief Generate random text from the supplied alphabet.
   *
   * @param bytes Number of random bytes to generate.
   * @param alphabet Allowed characters used for random string generation.
   * @return Random bytes or text generated by OpenSSL.
   */
  std::string rand_alphabet(std::size_t bytes, const std::string_view &alphabet = std::string_view {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!%&()=-"});

  /**
   * @brief Owns trusted certificates and a verification context initialized for each chain.
   */
  class cert_chain_t {
  public:
    KITTY_DECL_CONSTR(cert_chain_t)

    /**
     * @brief Add a certificate to the verification chain.
     *
     * @param cert Certificate data or object used by the operation.
     */
    void add(x509_t &&cert);

    /**
     * @brief Remove all certificates from the verification chain.
     */
    void clear();

    /**
     * @brief Verify that a certificate exactly matches one stored trust anchor.
     *
     * @param cert Client certificate to verify.
     * @return `nullptr` when the exact certificate is trusted, otherwise an OpenSSL error string.
     */
    const char *verify(x509_t::element_type *cert);

  private:
    std::vector<std::pair<x509_t, x509_store_t>> _certs;
    x509_store_ctx_t _cert_ctx {X509_STORE_CTX_new()};  ///< Verification context created for each certificate chain.
  };

  namespace cipher {
    constexpr std::size_t tag_size = 16;  ///< Tag size.

    /**
     * @brief Round a byte count up to the next PKCS#7 padding boundary.
     *
     * @param size Number of bytes or elements requested.
     * @return `size` rounded up to the next PKCS#7 block boundary.
     */
    constexpr std::size_t round_to_pkcs7_padded(std::size_t size) {
      return ((size + 15) / 16) * 16;
    }

    /**
     * @brief AES-GCM encrypt/decrypt context pair used for GameStream messages.
     */
    class cipher_t {
    public:
      cipher_ctx_t decrypt_ctx;  ///< Decrypt ctx.
      cipher_ctx_t encrypt_ctx;  ///< Encrypt ctx.

      aes_t key;  ///< AES key used by the cipher context.

      bool padding;  ///< Enables block padding for the cipher.
    };

    /**
     * @brief AES-ECB cipher helper used by pairing and stream encryption code.
     */
    class ecb_t: public cipher_t {
    public:
      ecb_t() = default;
      /**
       * @brief Construct an AES-ECB cipher helper with key material.
       */
      ecb_t(ecb_t &&) noexcept = default;
      /**
       * @brief Assign state from another instance while preserving ownership semantics.
       *
       * @return Reference or value produced by the operator.
       */
      ecb_t &operator=(ecb_t &&) noexcept = default;

      /**
       * @brief Explicitly construct an AES-ECB cipher helper with key material.
       *
       * @param key_material AES key material used to initialize the cipher.
       * @param use_padding Whether the cipher should use block padding.
       */
      explicit ecb_t(const aes_t &key_material, bool use_padding = true);

      /**
       * @brief Encrypt plaintext into the supplied ciphertext buffer.
       *
       * @param plaintext Plaintext bytes to encrypt.
       * @param cipher Ciphertext bytes to decrypt or output buffer for encryption.
       * @return Number of bytes written, signature bytes, or an error status depending on the overload.
       */
      int encrypt(const std::string_view &plaintext, std::vector<std::uint8_t> &cipher);
      /**
       * @brief Decrypt ciphertext into the supplied plaintext buffer.
       *
       * @param cipher Ciphertext bytes to decrypt or output buffer for encryption.
       * @param plaintext Plaintext bytes to encrypt.
       * @return Number of bytes written, signature bytes, or an error status depending on the overload.
       */
      int decrypt(const std::string_view &cipher, std::vector<std::uint8_t> &plaintext);
    };

    /**
     * @brief AES-GCM cipher helper that encrypts and authenticates payloads.
     */
    class gcm_t: public cipher_t {
    public:
      gcm_t() = default;
      /**
       * @brief Construct an AES-GCM cipher helper with key material and IV state.
       */
      gcm_t(gcm_t &&) noexcept = default;
      /**
       * @brief Assign state from another instance while preserving ownership semantics.
       *
       * @return Reference or value produced by the operator.
       */
      gcm_t &operator=(gcm_t &&) noexcept = default;

      /**
       * @brief Explicitly construct an AES-GCM cipher helper with key material and IV state.
       *
       * @param key_material AES key material used to initialize the cipher.
       * @param use_padding Whether the cipher should use block padding.
       */
      explicit gcm_t(const crypto::aes_t &key_material, bool use_padding = true);

      /**
       * @brief Encrypts the plaintext using AES GCM mode.
       * @param plaintext The plaintext data to be encrypted.
       * @param tag The buffer where the GCM tag will be written.
       * @param ciphertext The buffer where the resulting ciphertext will be written.
       * @param iv The initialization vector to be used for the encryption.
       * @return The total length of the ciphertext and GCM tag. Returns -1 in case of an error.
       */
      int encrypt(const std::string_view &plaintext, std::uint8_t *tag, std::uint8_t *ciphertext, aes_t *iv);

      /**
       * @brief Encrypts the plaintext using AES GCM mode.
       * length of cipher must be at least: round_to_pkcs7_padded(plaintext.size()) + crypto::cipher::tag_size
       * @param plaintext The plaintext data to be encrypted.
       * @param tagged_cipher The buffer where the resulting ciphertext and GCM tag will be written.
       * @param iv The initialization vector to be used for the encryption.
       * @return The total length of the ciphertext and GCM tag written into tagged_cipher. Returns -1 in case of an error.
       */
      int encrypt(const std::string_view &plaintext, std::uint8_t *tagged_cipher, aes_t *iv);

      /**
       * @brief Decrypt ciphertext into the supplied plaintext buffer.
       *
       * @param cipher Ciphertext bytes to decrypt or output buffer for encryption.
       * @param plaintext Plaintext bytes to encrypt.
       * @param iv Initialization vector for the cipher operation.
       * @return Number of bytes written, signature bytes, or an error status depending on the overload.
       */
      int decrypt(const std::string_view &cipher, std::vector<std::uint8_t> &plaintext, aes_t *iv);
    };

    /**
     * @brief AES-CBC cipher helper used for block-mode encryption and decryption.
     */
    class cbc_t: public cipher_t {
    public:
      cbc_t() = default;
      /**
       * @brief Construct an AES-CBC cipher helper with key material and IV state.
       */
      cbc_t(cbc_t &&) noexcept = default;
      /**
       * @brief Assign state from another instance while preserving ownership semantics.
       *
       * @return Reference or value produced by the operator.
       */
      cbc_t &operator=(cbc_t &&) noexcept = default;

      /**
       * @brief Explicitly construct an AES-CBC cipher helper with key material and IV state.
       *
       * @param key_material AES key material used to initialize the cipher.
       * @param use_padding Whether the cipher should use block padding.
       */
      explicit cbc_t(const crypto::aes_t &key_material, bool use_padding = true);

      /**
       * @brief Encrypts the plaintext using AES CBC mode.
       * length of cipher must be at least: round_to_pkcs7_padded(plaintext.size())
       * @param plaintext The plaintext data to be encrypted.
       * @param cipher The buffer where the resulting ciphertext will be written.
       * @param iv The initialization vector to be used for the encryption.
       * @return The total length of the ciphertext written into cipher. Returns -1 in case of an error.
       */
      int encrypt(const std::string_view &plaintext, std::uint8_t *cipher, aes_t *iv);
    };
  }  // namespace cipher
}  // namespace crypto
