/**
 * @file src/nvhttp.h
 * @brief Declarations for the nvhttp (GameStream) server.
 */
// macros
#pragma once

// standard includes
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "crypto.h"

/**
 * @brief Contains all the functions and variables related to the nvhttp (GameStream) server.
 */
namespace nvhttp {

  /**
   * @brief The protocol version.
   * @details The version of the GameStream protocol we are mocking.
   * @note The negative 4th number indicates to Moonlight that this is Sunshine.
   */
  constexpr auto VERSION = "7.1.431.-1";

  /**
   * @brief The GFE version we are replicating.
   */
  constexpr auto GFE_VERSION = "3.23.0.74";

  /**
   * @brief The HTTP port, as a difference from the config port.
   */
  constexpr auto PORT_HTTP = 0;

  /**
   * @brief The HTTPS port, as a difference from the config port.
   */
  constexpr auto PORT_HTTPS = -5;

  /**
   * @brief Maximum number of pairing sessions retained at one time.
   */
  constexpr std::size_t MAX_PENDING_PAIRING_SESSIONS = 32;

  /**
   * @brief Number of hexadecimal characters in an operator approval identifier.
   */
  constexpr std::size_t PAIRING_ID_SIZE = 32;

  /**
   * @brief Maximum number of bytes accepted for an operator-supplied paired-client name.
   */
  constexpr std::size_t MAX_PAIRING_CLIENT_NAME_SIZE = 128;

  /**
   * @brief Lifetime of an incomplete pairing session.
   */
  constexpr auto PAIRING_SESSION_TIMEOUT = std::chrono::minutes {5};

  /**
   * @brief Start the nvhttp server.
   * @examples
   * nvhttp::start();
   * @examples_end
   */
  void start();

  /**
   * @brief Setup the nvhttp server.
   * @param pkey
   * @param cert
   */
  void setup(const std::string &pkey, const std::string &cert);

  /**
   * @brief Simple-Web-Server HTTPS backend configured for Sunshine certificate handling.
   */
  class SunshineHTTPS: public SimpleWeb::HTTPS {
  public:
    /**
     * @brief Construct an HTTPS connection using Sunshine's TLS context.
     *
     * @param io_context Boost.Asio context used for network operations.
     * @param ctx TLS context configured with Sunshine's certificate and key.
     */
    SunshineHTTPS(boost::asio::io_context &io_context, boost::asio::ssl::context &ctx):
        SimpleWeb::HTTPS(io_context, ctx) {
    }

    virtual ~SunshineHTTPS() {
      // Gracefully shutdown the TLS connection
      SimpleWeb::error_code ec;
      shutdown(ec);
    }
  };

  /**
   * @brief Enumerates supported pAIR PHASE options.
   */
  enum class PAIR_PHASE {
    NONE,  ///< Sunshine is not in a pairing phase
    GETSERVERCERT,  ///< Sunshine is in the get server certificate phase
    CLIENTCHALLENGE,  ///< Sunshine is in the client challenge phase
    SERVERCHALLENGERESP,  ///< Sunshine is in the server challenge response phase
    CLIENTPAIRINGSECRET  ///< Sunshine is in the client pairing secret phase
  };

  /**
   * @brief Shared result used to return the completed pairing outcome to a PIN submitter.
   */
  struct pairing_completion_t {
    std::condition_variable condition;  ///< Wakes the REST request when pairing finishes.
    std::mutex mutex;  ///< Protects the completion result.
    std::optional<bool> result;  ///< Final pairing result, or no value while the handshake is pending.
  };

  /**
   * @brief Pairing handshake state exchanged with a Moonlight client.
   */
  struct pair_session_t {
    struct {
      std::string uniqueID = {};  ///< Client-provided pairing-session identifier.
      std::string cert = {};  ///< Client certificate bytes exchanged during pairing.
      std::string name = {};  ///< Client name recorded after successful pairing.
    } client;  ///< Client object or client certificate data owned by this state..

    std::unique_ptr<crypto::aes_t> cipher_key = {};  ///< Cipher key.
    std::vector<uint8_t> clienthash = {};  ///< Client certificate hash used during pairing.

    std::string serversecret = {};  ///< Server pairing secret.
    std::string serverchallenge = {};  ///< Server challenge sent during pairing.
    std::shared_ptr<pairing_completion_t> completion = std::make_shared<pairing_completion_t>();  ///< Result shared with the REST request waiting for the handshake.

    struct {
      util::Either<
        std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>,
        std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>>
        response;  ///< Pending HTTP or HTTPS response completed after PIN approval.
      std::string salt = {};  ///< Client-provided salt used to derive the pairing key.
      std::string id = {};  ///< Unguessable identifier used by the Web UI to approve this session.
      std::string device_name = {};  ///< Untrusted device name reported by the pairing client.
      std::string address = {};  ///< Network address from which the pairing request originated.
      std::chrono::steady_clock::time_point expires_at = std::chrono::steady_clock::time_point::max();  ///< Deadline for completing this pairing session.
    } async_insert_pin;  ///< Async insert pin.

    /**
     * @brief used as a security measure to prevent out of order calls
     */
    PAIR_PHASE last_phase = PAIR_PHASE::NONE;
    bool failed = false;  ///< Whether protocol validation failed and the session must be removed.
  };

  /**
   * @brief Operator-visible context for a pairing request awaiting PIN approval.
   */
  struct pending_pairing_t {
    std::string id;  ///< Unguessable approval identifier.
    std::string name;  ///< Untrusted device name reported by the pairing client.
    std::string address;  ///< Network address from which the request originated.
  };

  /**
   * @brief Result of inserting a new pairing session into bounded pending storage.
   */
  enum class pair_session_insert_e {
    ADDED,  ///< The session was inserted successfully.
    ALREADY_EXISTS,  ///< A session with the same client unique ID is already active.
    FULL  ///< The bounded session store has reached its limit.
  };

  /**
   * @brief Validate an operator approval identifier received from the REST API.
   *
   * @param pairing_id Approval identifier to validate.
   * @return `true` when the value has the exact hexadecimal format Sunshine generates.
   */
  bool is_valid_pairing_id(std::string_view pairing_id);

  /**
   * @brief Validate a Moonlight pairing PIN received from the REST API.
   *
   * @param pin PIN to validate.
   * @return `true` when the value contains exactly four ASCII digits.
   */
  bool is_valid_pairing_pin(std::string_view pin);

  /**
   * @brief Validate an operator-supplied paired-client name received from the REST API.
   *
   * @param name Client name to validate.
   * @return `true` when the name is non-empty and within the storage limit.
   */
  bool is_valid_pairing_name(std::string_view name);

  /**
   * @brief Insert a newly created pairing session into bounded pending storage.
   *
   * @param sess Pairing session to insert.
   * @param pairing_id Receives the unguessable approval identifier on success.
   * @return Status describing whether the session was inserted.
   */
  pair_session_insert_e insert_pair_session(pair_session_t sess, std::string &pairing_id);

  /**
   * @brief Evict an abandoned in-progress pairing session for a client.
   *
   * A fresh `getservercert` request means the client abandoned its previous handshake and
   * started over: the phase machine never accepts `getservercert` twice for one session, so
   * the old handshake cannot resume. Evicting it lets the retry proceed instead of failing
   * with 409 until the session expires. Sessions still waiting for operator PIN approval
   * (`last_phase == NONE`) are left untouched, so a pending approval cannot be displaced
   * by a stray `getservercert`. Eviction completes the old session as failed, waking any
   * PIN submitter waiting on it, and orphans no live HTTP response: the response is
   * consumed when PIN approval advances the session past the certificate phase.
   *
   * @param unique_id Client-provided pairing-session identifier.
   * @return `true` when an in-progress session was evicted.
   */
  bool evict_abandoned_pair_session(const std::string &unique_id);

  /**
   * @brief Remove pairing sessions whose completion deadline has passed.
   *
   * @param now Monotonic time used to evaluate session deadlines.
   */
  void expire_pair_sessions(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  /**
   * @brief List pairing requests that are waiting for operator PIN approval.
   *
   * @return Pending requests ordered from oldest to newest.
   */
  std::vector<pending_pairing_t> get_pending_pairings();

  /**
   * @brief Cancel a pairing request that is waiting for operator approval.
   *
   * @param pairing_id Unguessable approval identifier returned by @ref get_pending_pairings.
   * @return `true` if the pending request was found and cancelled.
   */
  bool cancel_pairing(std::string_view pairing_id);

  /**
   * @brief removes the temporary pairing session
   * @param sess
   */
  void remove_session(const pair_session_t &sess);

  /**
   * @brief Pair, phase 1
   *
   * Moonlight will send a salt and client certificate, we'll also need the user provided pin.
   *
   * PIN and SALT will be used to derive a shared AES key that needs to be stored
   * in order to be used to decrypt_symmetric in the next phases.
   *
   * At this stage we only have to send back our public certificate.
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param pin PIN supplied by the client during pairing.
   */
  void getservercert(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &pin);

  /**
   * @brief Pair, phase 2
   *
   * Using the AES key that we generated in phase 1 we have to decrypt the client challenge,
   *
   * We generate a SHA256 hash with the following:
   *  - Decrypted challenge
   *  - Server certificate signature
   *  - Server secret: a randomly generated secret
   *
   * The hash + server_challenge will then be AES encrypted and sent as the `challengeresponse` in the returned XML
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param challenge Client challenge bytes from the pairing request.
   */
  void clientchallenge(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &challenge);

  /**
   * @brief Pair, phase 3
   *
   * Moonlight will send back a `serverchallengeresp`: an AES encrypted client hash,
   * we have to send back the `pairingsecret`:
   * using our private key we have to sign the certificate_signature + server_secret (generated in phase 2)
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param encrypted_response Encrypted response.
   */
  void serverchallengeresp(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &encrypted_response);

  /**
   * @brief Pair, phase 4 (final)
   *
   * We now have to use everything we exchanged before in order to verify and finally pair the clients
   *
   * We'll check the client_hash obtained at phase 3, it should contain the following:
   *   - The original server_challenge
   *   - The signature of the X509 client_cert
   *   - The unencrypted client_pairing_secret
   * We'll check that SHA256(server_challenge + client_public_cert_signature + client_secret) == client_hash
   *
   * Then using the client certificate public key we should be able to verify that
   * the client secret has been signed by Moonlight
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param client_pairing_secret Client pairing secret.
   */
  void clientpairingsecret(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &client_pairing_secret);

  /**
   * @brief Apply the user supplied PIN to the explicitly selected pairing request.
   * @param pairing_id Unguessable identifier of the pairing request to approve.
   * @param pin The user supplied pin.
   * @param name The user supplied name.
   * @return `true` if Moonlight proves the PIN by completing the handshake, `false` otherwise.
   * @note The handshake wait uses the configured `ping_timeout` and never exceeds the pairing session deadline.
   * @examples
   * bool pin_status = nvhttp::pin("0123456789abcdef0123456789abcdef", "1234", "laptop");
   * @examples_end
   */
  bool pin(std::string_view pairing_id, const std::string &pin, std::string name);

  /**
   * @brief Remove single client.
   * @param uuid The UUID of the client to remove.
   * @examples
   * nvhttp::unpair_client("4D7BB2DD-5704-A405-B41C-891A022932E1");
   * @examples_end
   *
   * @return True when the client entry was found and removed.
   */
  bool unpair_client(std::string_view uuid);

  /**
   * @brief Enable or disable a client.
   * @param uuid The UUID of the client.
   * @param enabled Whether the client should be enabled.
   * @return true if the client was found and updated.
   */
  bool set_client_enabled(std::string_view uuid, bool enabled);
  /**
   * @brief Get cert by UUID.
   *
   * @param uuid Client UUID being looked up or removed.
   * @return PEM certificate for the paired client, or an empty string when unknown.
   */
  std::string get_cert_by_uuid(std::string_view uuid);

  /**
   * @brief Get all paired clients.
   * @return The list of all paired clients.
   * @examples
   * nlohmann::json clients = nvhttp::get_all_clients();
   * @examples_end
   */
  nlohmann::json get_all_clients();

  /**
   * @brief Remove all paired clients.
   * @examples
   * nvhttp::erase_all_clients();
   * @examples_end
   */
  void erase_all_clients();

  /**
   * @brief Get codec mode flags.
   *
   * @return Moonlight codec capability bitmask for the currently probed encoders.
   */
  uint32_t get_codec_mode_flags();

#ifdef SUNSHINE_TESTS
  /**
   * @brief Test-only accessors for paired-client authorization state.
   */
  namespace test_support {
    /**
     * @brief Create the production HTTPS server and expose its callback lifetime guard.
     *
     * @param certificate_file Path to the server certificate PEM file.
     * @param private_key_file Path to the matching private key PEM file.
     * @param handler_scope Receives the scope runner that prevents destruction during callbacks.
     * @return HTTPS server ready for test endpoint configuration.
     */
    std::unique_ptr<SimpleWeb::ServerBase<SunshineHTTPS>> make_https_server(
      const std::string &certificate_file,
      const std::string &private_key_file,
      std::shared_ptr<SimpleWeb::ScopeRunner> &handler_scope
    );

    /**
     * @brief Dispatch a plain-HTTP pairing request through the production handler.
     *
     * @param response HTTP response object to populate.
     * @param request HTTP request data from the test client.
     */
    void pair_http(
      std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response> response,
      std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request> request
    );

    /**
     * @brief Clear in-memory paired-client records without changing persisted state.
     */
    void reset_client_state();

    /**
     * @brief Add a paired-client record for authorization tests.
     *
     * @param name Human-readable client name.
     * @param cert PEM-encoded client certificate.
     * @param enabled Whether the client may connect.
     * @return Persistent UUID for the added client, or an empty string when invalid.
     */
    std::string add_client(const std::string &name, std::string cert, bool enabled);

    /**
     * @brief Duplicate a paired-client record to simulate legacy conflicting state.
     *
     * @param uuid Persistent UUID of the record to duplicate.
     * @return `true` when the source record was found and duplicated.
     */
    bool duplicate_client(std::string_view uuid);

    /**
     * @brief Run the production certificate authorization checks against PEM input.
     *
     * @param cert PEM-encoded certificate presented by a client.
     * @return `true` when the exact certificate belongs to one enabled paired client.
     */
    bool authorize_client_certificate(std::string_view cert);

    /**
     * @brief Complete and remove a pairing session without running the protocol phases.
     *
     * @param pairing_id Operator approval identifier of the test session.
     * @param success Pairing result delivered to the waiting PIN submitter.
     * @return `true` when the session was found and completed.
     */
    bool complete_pairing(std::string_view pairing_id, bool success);

    /**
     * @brief Reload paired-client authorization state from the configured state file.
     */
    void reload_client_state();
  }  // namespace test_support
#endif
}  // namespace nvhttp
