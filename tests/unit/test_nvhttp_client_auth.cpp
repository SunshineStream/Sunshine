/**
 * @file tests/unit/test_nvhttp_client_auth.cpp
 * @brief Test exact paired-client certificate authorization and persistence.
 */

// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <filesystem>
#include <thread>
#include <type_traits>
#include <vector>

// local includes
#include <src/config.h>
#include <src/nvhttp.h>
#include <src/rtsp.h>

namespace nvhttp {
  /**
   * @brief Read the production authorization lifetime shared by streams and RTSP.
   * @param cert Canonical PEM client identity.
   * @return Revocation state for that identity, revoked when it is not authorized.
   */
  std::shared_ptr<std::atomic_bool> client_revocation_state(std::string_view cert);
}  // namespace nvhttp

namespace fs = std::filesystem;

static_assert(std::is_same_v<decltype(&nvhttp::pin), bool (*)(std::string_view, const std::string &, std::string)>);

/**
 * @brief Isolate paired-client authorization tests from the user's Sunshine state.
 */
class ClientAuthorizationTest: public BaseTest {
protected:
  /**
   * @brief Redirect persisted client state to the test build directory.
   */
  void SetUp() override {
    BaseTest::SetUp();
    original_state_file = config::nvhttp.file_state;
    original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    state_file = fs::path {SUNSHINE_TEST_BIN_DIR} / "client_authorization_state.json";

    config::nvhttp.file_state = state_file.string();
    config::sunshine.flags[config::flag::FRESH_STATE] = false;
    nvhttp::test_support::reset_client_state();

    std::error_code remove_error;
    fs::remove(state_file, remove_error);
  }

  /**
   * @brief Remove test state and restore the caller's configuration.
   */
  void TearDown() override {
    nvhttp::test_support::reset_client_state();
    std::error_code remove_error;
    fs::remove(state_file, remove_error);

    config::nvhttp.file_state = original_state_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
    BaseTest::TearDown();
  }

private:
  fs::path state_file;  ///< Task-specific persisted state fixture.
  std::string original_state_file;  ///< State-file setting restored after each test.
  bool original_fresh_state;  ///< Fresh-state flag restored after each test.
};

TEST_F(ClientAuthorizationTest, CanonicalIdentityFailsClosedAndTracksEnableState) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto crlf_certificate = test_utils::certificates::to_crlf_pem(paired_credentials.x509);
  const auto unknown_credentials = crypto::gen_creds("Sunshine Unknown Client", 2048);
  const auto uuid = nvhttp::test_support::add_client("paired", crlf_certificate, true);

  EXPECT_TRUE(nvhttp::test_support::add_client("invalid", "not a certificate", true).empty());
  ASSERT_FALSE(uuid.empty());
  EXPECT_EQ(nvhttp::get_cert_by_uuid(uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(crlf_certificate));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(unknown_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate("not a certificate"));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DerivedLeafDoesNotInheritPairedAuthorization) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto derived_credentials = test_utils::certificates::generate_derived_leaf(paired_credentials);
  const auto uuid = nvhttp::test_support::add_client("paired issuer", paired_credentials.x509, true);

  ASSERT_FALSE(uuid.empty());
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));
}

TEST_F(ClientAuthorizationTest, MultipleClientsPersistAndUnpairIndependently) {
  const auto enabled_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Enabled Client");
  const auto disabled_credentials = crypto::gen_creds("Sunshine Disabled Client", 2048);
  const auto expired_credentials = test_utils::certificates::expire_credentials(
    test_utils::certificates::generate_ca_credentials("Sunshine Expired Client")
  );

  const auto enabled_uuid = nvhttp::test_support::add_client("enabled", enabled_credentials.x509, true);
  const auto disabled_uuid = nvhttp::test_support::add_client("disabled", disabled_credentials.x509, false);
  const auto expired_uuid = nvhttp::test_support::add_client("expired", expired_credentials.x509, true);
  ASSERT_FALSE(enabled_uuid.empty());
  ASSERT_FALSE(disabled_uuid.empty());
  ASSERT_FALSE(expired_uuid.empty());
  EXPECT_EQ(nvhttp::get_all_clients().size(), 3);

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  nvhttp::test_support::reset_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  nvhttp::test_support::reload_client_state();

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(disabled_uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  ASSERT_TRUE(nvhttp::unpair_client(enabled_uuid));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));

  nvhttp::erase_all_clients();
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DuplicateCertificateIdentityFailsClosed) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("first", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  EXPECT_FALSE(nvhttp::test_support::duplicate_client("missing"));
  ASSERT_TRUE(nvhttp::test_support::duplicate_client(uuid));

  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, RePairingReplacesDuplicateCertificateIdentity) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto other_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Other Client");
  const auto original_uuid = nvhttp::test_support::add_client("original", paired_credentials.x509, true);
  const auto other_uuid = nvhttp::test_support::add_client("other", other_credentials.x509, true);
  ASSERT_FALSE(original_uuid.empty());
  ASSERT_FALSE(other_uuid.empty());
  ASSERT_TRUE(nvhttp::test_support::duplicate_client(original_uuid));
  ASSERT_EQ(nvhttp::get_all_clients().size(), 3);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));

  const auto repaired_uuid = nvhttp::test_support::add_client(
    "repaired",
    test_utils::certificates::to_crlf_pem(paired_credentials.x509),
    true
  );

  ASSERT_FALSE(repaired_uuid.empty());
  EXPECT_NE(repaired_uuid, original_uuid);
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2);
  EXPECT_EQ(nvhttp::get_cert_by_uuid(repaired_uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(other_credentials.x509));

  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(other_credentials.x509));
}

TEST_F(ClientAuthorizationTest, ConcurrentStateChangesRemainConsistent) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("concurrent", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  std::atomic_bool operations_succeeded {true};
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&operations_succeeded, uuid, certificate = credentials.x509, worker]() {
      for (std::size_t iteration = 0; iteration < 8; ++iteration) {
        if (!nvhttp::set_client_enabled(uuid, (worker + iteration) % 2 == 0)) {
          operations_succeeded = false;
        }
        static_cast<void>(nvhttp::test_support::authorize_client_certificate(certificate));
      }
    });
  }
  workers.clear();

  EXPECT_TRUE(operations_succeeded);
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

/**
 * @brief Removing one client revokes retained sessions while preserving other identities.
 */
TEST_F(ClientAuthorizationTest, UnpairRevokesExistingLaunchStateWithoutRevokingOtherClients) {
  const auto first = crypto::gen_creds("revoked client", 2048);
  const auto second = crypto::gen_creds("retained client", 2048);
  const auto first_id = nvhttp::test_support::add_client("first", first.x509, true);
  ASSERT_FALSE(first_id.empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("second", second.x509, true).empty());
  const auto pending_or_active = nvhttp::client_revocation_state(first.x509);
  const auto other = nvhttp::client_revocation_state(second.x509);
  EXPECT_FALSE(pending_or_active->load());
  ASSERT_TRUE(nvhttp::unpair_client(first_id));
  EXPECT_TRUE(pending_or_active->load());
  EXPECT_TRUE(nvhttp::client_revocation_state(first.x509)->load());
  EXPECT_FALSE(other->load());
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(second.x509));
}

/**
 * @brief Re-enabling a client creates a new lifetime instead of resurrecting revoked sessions.
 */
TEST_F(ClientAuthorizationTest, ReenableDoesNotRestoreOldLaunchAuthorization) {
  const auto credentials = crypto::gen_creds("reenabled client", 2048);
  const auto uuid = nvhttp::test_support::add_client("client", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  const auto old = nvhttp::client_revocation_state(credentials.x509);
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_TRUE(old->load());
  EXPECT_TRUE(nvhttp::client_revocation_state(credentials.x509)->load());
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));
  EXPECT_TRUE(old->load());
  EXPECT_FALSE(nvhttp::client_revocation_state(credentials.x509)->load());
  EXPECT_TRUE(nvhttp::client_revocation_state("unknown certificate")->load());
}

/**
 * @brief Re-pairing and removing all clients invalidate previously retained launch state.
 */
TEST_F(ClientAuthorizationTest, RePairAndUnpairAllInvalidateRetainedLaunches) {
  const auto credentials = crypto::gen_creds("repaired client", 2048);
  ASSERT_FALSE(nvhttp::test_support::add_client("original", credentials.x509, true).empty());
  const auto old = nvhttp::client_revocation_state(credentials.x509);
  ASSERT_FALSE(nvhttp::test_support::add_client("replacement", credentials.x509, true).empty());
  EXPECT_TRUE(old->load());
  const auto replacement = nvhttp::client_revocation_state(credentials.x509);
  EXPECT_FALSE(replacement->load());
  nvhttp::erase_all_clients();
  EXPECT_TRUE(replacement->load());
}

/**
 * @brief Teardown from an earlier disable cannot invalidate a subsequent authorized launch.
 */
TEST_F(ClientAuthorizationTest, DelayedTeardownPreservesReenabledPendingLaunch) {
  const auto credentials = crypto::gen_creds("reenabled pending client", 2048);
  const auto uuid = nvhttp::test_support::add_client("client", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  const auto old = nvhttp::client_revocation_state(credentials.x509);
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));

  auto pending = std::make_shared<rtsp_stream::launch_session_t>();
  pending->id = 42;
  pending->client_cert = credentials.x509;
  pending->revoked = nvhttp::client_revocation_state(credentials.x509);
  pending->rtsp_cipher.emplace(crypto::aes_t(16, 0), false);
  rtsp_stream::launch_session_raise(pending);
  const auto cleanup = util::fail_guard([&]() {
    rtsp_stream::launch_session_clear(pending->id);
  });

  // The disable's teardown is delayed until after the new enabled lifetime is in use.
  rtsp_stream::terminate_sessions_by_cert(credentials.x509);
  EXPECT_TRUE(old->load());
  EXPECT_FALSE(pending->revoked->load());
  EXPECT_TRUE(rtsp_stream::accepts_rtsp(*pending));
  EXPECT_EQ(nvhttp::client_revocation_state(credentials.x509), pending->revoked);
}
