/**
 * @file tests/unit/test_network.cpp
 * @brief Test src/network.*
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <cstring>

// lib includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

// local includes
#include <src/network.h>

namespace {
  /**
   * @brief Copy the leading bytes of a socket address storage into a concrete socket address type.
   *
   * Copying avoids the strict-aliasing violation of reinterpreting the storage in place.
   *
   * @tparam T Concrete socket address type, such as `sockaddr_in` or `sockaddr_in6`.
   * @param storage The socket address storage to copy from.
   * @return A copy of the address as `T`.
   */
  template<typename T>
  T sockaddr_as(const sockaddr_storage &storage) {
    static_assert(sizeof(T) <= sizeof(sockaddr_storage));
    T address {};
    std::memcpy(&address, &storage, sizeof(address));
    return address;
  }
}  // namespace

TEST(NetworkClassificationTest, DeducedNetworkListsPreserveAddressScopes) {
  EXPECT_EQ(net::from_address("127.0.0.1"), net::PC);
  EXPECT_EQ(net::from_address("192.168.1.2"), net::LAN);
  EXPECT_EQ(net::from_address("100.64.0.1"), net::LAN);
  EXPECT_EQ(net::from_address("169.254.1.2"), net::LAN);
  EXPECT_EQ(net::from_address("8.8.8.8"), net::WAN);
  EXPECT_EQ(net::from_address("::1"), net::PC);
  EXPECT_EQ(net::from_address("fc00::1"), net::LAN);
  EXPECT_EQ(net::from_address("fe80::1"), net::LAN);
  EXPECT_EQ(net::from_address("2001:4860:4860::8888"), net::WAN);
}

/**
 * @brief Loopback and documentation addresses retain their classification when mapped to IPv6.
 */
TEST(NetworkClassificationTest, ClassifiesMappedIpv4AddressesWithoutChangingSubsequentLookups) {
  for (const auto *address : {"127.0.0.1", "192.0.2.1", "198.51.100.2", "203.0.113.3"}) {
    const auto expected = net::from_address(address);
    EXPECT_EQ(net::from_address(std::string {"::ffff:"} + address), expected);
    EXPECT_EQ(net::from_address(address), expected);
  }
}

struct MdnsInstanceNameTest: BaseTest, testing::WithParamInterface<std::tuple<std::string, std::string>> {};

TEST_P(MdnsInstanceNameTest, Run) {
  auto [input, expected] = GetParam();
  ASSERT_EQ(net::mdns_instance_name(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  MdnsInstanceNameTests,
  MdnsInstanceNameTest,
  testing::Values(
    std::make_tuple("shortname-123", "shortname-123"),
    std::make_tuple("space 123", "space-123"),
    std::make_tuple("hostname.domain.test", "hostname"),
    std::make_tuple("&", "Sunshine"),
    std::make_tuple("", "Sunshine"),
    std::make_tuple("😁", "Sunshine"),
    std::make_tuple(std::string(128, 'a'), std::string(63, 'a'))
  )
);

/**
 * @brief Test fixture for bind_address tests with setup/teardown
 */
class BindAddressTest: public BaseTest {
protected:
  std::string original_bind_address;

  void SetUp() override {
    BaseTest::SetUp();
    // Save the original bind_address config
    original_bind_address = config::sunshine.bind_address;
  }

  void TearDown() override {
    // Restore the original bind_address config
    config::sunshine.bind_address = original_bind_address;
    BaseTest::TearDown();
  }
};

/**
 * @brief Test that get_bind_address returns wildcard when bind_address is not configured
 */
TEST_F(BindAddressTest, DefaultBehaviorIPv4) {
  // Clear bind_address to test the default behavior
  config::sunshine.bind_address = "";

  const auto bind_addr = net::get_bind_address(net::af_e::IPV4);
  ASSERT_EQ(bind_addr, "0.0.0.0");
}

/**
 * @brief Test that get_bind_address returns wildcard when bind_address is not configured (IPv6)
 */
TEST_F(BindAddressTest, DefaultBehaviorIPv6) {
  // Clear bind_address to test the default behavior
  config::sunshine.bind_address = "";

  const auto bind_addr = net::get_bind_address(net::af_e::BOTH);
  ASSERT_EQ(bind_addr, "::");
}

/**
 * @brief Test that an empty bind address preserves the configured address family.
 */
TEST_F(BindAddressTest, EmptyAddressPreservesConfiguredFamily) {
  config::sunshine.bind_address = "";

  EXPECT_EQ(net::get_effective_address_family(net::af_e::IPV4), net::af_e::IPV4);
  EXPECT_EQ(net::get_effective_address_family(net::af_e::BOTH), net::af_e::BOTH);
}

/**
 * @brief Test that an explicit IPv4 bind address selects an IPv4 socket.
 */
TEST_F(BindAddressTest, IPv4AddressSelectsIPv4Family) {
  config::sunshine.bind_address = "192.0.2.101";

  EXPECT_EQ(net::get_effective_address_family(net::af_e::IPV4), net::af_e::IPV4);
  EXPECT_EQ(net::get_effective_address_family(net::af_e::BOTH), net::af_e::IPV4);
}

/**
 * @brief Test that an explicit IPv6 bind address does not override the configured family.
 */
TEST_F(BindAddressTest, IPv6AddressPreservesConfiguredFamily) {
  config::sunshine.bind_address = "2001:db8::1";

  EXPECT_EQ(net::get_effective_address_family(net::af_e::IPV4), net::af_e::IPV4);
  EXPECT_EQ(net::get_effective_address_family(net::af_e::BOTH), net::af_e::BOTH);
}

/**
 * @brief Test that invalid bind addresses preserve the configured family for downstream error reporting.
 */
TEST_F(BindAddressTest, InvalidAddressPreservesConfiguredFamily) {
  config::sunshine.bind_address = "not-an-address";

  EXPECT_EQ(net::get_effective_address_family(net::af_e::IPV4), net::af_e::IPV4);
  EXPECT_EQ(net::get_effective_address_family(net::af_e::BOTH), net::af_e::BOTH);
}

/**
 * @brief Test that bind addresses are formatted for use as URL hosts.
 */
TEST_F(BindAddressTest, AddressFormatsAsUrlHost) {
  config::sunshine.bind_address = "";
  EXPECT_EQ(net::get_bind_address_url_host(), "localhost");

  config::sunshine.bind_address = "198.51.100.56";
  EXPECT_EQ(net::get_bind_address_url_host(), "198.51.100.56");

  config::sunshine.bind_address = "2001:db8::1";
  EXPECT_EQ(net::get_bind_address_url_host(), "[2001:db8::1]");

  config::sunshine.bind_address = "not-an-address";
  EXPECT_EQ(net::get_bind_address_url_host(), "not-an-address");
}

/**
 * @brief Test that get_bind_address returns configured IPv4 address
 */
TEST_F(BindAddressTest, ConfiguredIPv4Address) {
  // Set a specific IPv4 address
  config::sunshine.bind_address = "192.168.1.100";

  const auto bind_addr = net::get_bind_address(net::af_e::IPV4);
  ASSERT_EQ(bind_addr, "192.168.1.100");
}

/**
 * @brief Test that get_bind_address returns configured IPv6 address
 */
TEST_F(BindAddressTest, ConfiguredIPv6Address) {
  // Set a specific IPv6 address
  config::sunshine.bind_address = "::1";

  const auto bind_addr = net::get_bind_address(net::af_e::BOTH);
  ASSERT_EQ(bind_addr, "::1");
}

/**
 * @brief Test that get_bind_address returns configured address regardless of address family
 */
TEST_F(BindAddressTest, ConfiguredAddressOverridesFamily) {
  // Set a specific IPv6 address but request IPv4 family
  // The configured address should still be returned
  config::sunshine.bind_address = "2001:db8::1";

  const auto bind_addr = net::get_bind_address(net::af_e::IPV4);
  ASSERT_EQ(bind_addr, "2001:db8::1");
}

/**
 * @brief Test with loopback addresses
 */
TEST_F(BindAddressTest, LoopbackAddresses) {
  // Test IPv4 loopback
  config::sunshine.bind_address = "127.0.0.1";
  const auto bind_addr_v4 = net::get_bind_address(net::af_e::IPV4);
  ASSERT_EQ(bind_addr_v4, "127.0.0.1");

  // Test IPv6 loopback
  config::sunshine.bind_address = "::1";
  const auto bind_addr_v6 = net::get_bind_address(net::af_e::BOTH);
  ASSERT_EQ(bind_addr_v6, "::1");
}

/**
 * @brief Test with link-local addresses
 */
TEST_F(BindAddressTest, LinkLocalAddresses) {
  // Test IPv4 link-local
  config::sunshine.bind_address = "169.254.1.1";
  const auto bind_addr_v4 = net::get_bind_address(net::af_e::IPV4);
  ASSERT_EQ(bind_addr_v4, "169.254.1.1");

  // Test IPv6 link-local
  config::sunshine.bind_address = "fe80::1";
  const auto bind_addr_v6 = net::get_bind_address(net::af_e::BOTH);
  ASSERT_EQ(bind_addr_v6, "fe80::1");
}

/**
 * @brief Test that af_to_any_address_string still works correctly
 */
TEST_F(BindAddressTest, WildcardAddressFunction) {
  ASSERT_EQ(net::af_to_any_address_string(net::af_e::IPV4), "0.0.0.0");
  ASSERT_EQ(net::af_to_any_address_string(net::af_e::BOTH), "::");
}

/**
 * @brief Test that host_create binds an ENet host on an available loopback port.
 */
TEST_F(BindAddressTest, HostCreateBindsAvailablePort) {
  config::sunshine.bind_address = "127.0.0.1";

  ENetAddress addr {};
  const auto host = net::host_create(net::af_e::IPV4, addr, 0);

  ASSERT_TRUE(host);
  ASSERT_EQ(host->address.address.ss_family, AF_INET);
  EXPECT_NE(sockaddr_as<sockaddr_in>(host->address.address).sin_port, 0);
}

/**
 * @brief Test that host_create returns an empty handle instead of crashing when the port is in use.
 */
TEST_F(BindAddressTest, HostCreateReturnsEmptyHandleWhenPortIsInUse) {
  config::sunshine.bind_address = "127.0.0.1";

  boost::asio::io_context io_context;
  boost::asio::ip::udp::socket blocker(io_context, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address_v4("127.0.0.1"), 0));
  const auto port = blocker.local_endpoint().port();

  ENetAddress addr {};
  const auto host = net::host_create(net::af_e::IPV4, addr, port);

  EXPECT_FALSE(host);
}

/**
 * @brief Test that set_enet_address converts the IPv6 wildcard without consulting the resolver.
 */
TEST(SetEnetAddressTest, ConvertsIpv6Wildcard) {
  ENetAddress addr {};

  ASSERT_TRUE(net::set_enet_address(addr, "::", 47999));

  ASSERT_EQ(addr.address.ss_family, AF_INET6);
  ASSERT_EQ(addr.addressLength, sizeof(sockaddr_in6));
  const auto sin6 = sockaddr_as<sockaddr_in6>(addr.address);
  EXPECT_EQ(sin6.sin6_port, ENET_HOST_TO_NET_16(47999));
  const auto bytes = boost::asio::ip::make_address_v6("::").to_bytes();
  EXPECT_EQ(std::memcmp(&sin6.sin6_addr, bytes.data(), bytes.size()), 0);
}

/**
 * @brief Test that set_enet_address converts a numeric IPv4 address.
 */
TEST(SetEnetAddressTest, ConvertsIpv4Address) {
  ENetAddress addr {};

  ASSERT_TRUE(net::set_enet_address(addr, "192.0.2.1", 48010));

  ASSERT_EQ(addr.address.ss_family, AF_INET);
  ASSERT_EQ(addr.addressLength, sizeof(sockaddr_in));
  const auto sin = sockaddr_as<sockaddr_in>(addr.address);
  EXPECT_EQ(sin.sin_port, ENET_HOST_TO_NET_16(48010));
  const auto bytes = boost::asio::ip::make_address_v4("192.0.2.1").to_bytes();
  EXPECT_EQ(std::memcmp(&sin.sin_addr, bytes.data(), bytes.size()), 0);
}

/**
 * @brief Test that set_enet_address resolves host names through ENet.
 */
TEST(SetEnetAddressTest, ResolvesHostName) {
  ENetAddress addr {};

  ASSERT_TRUE(net::set_enet_address(addr, "localhost", 47999));

  EXPECT_TRUE(addr.address.ss_family == AF_INET || addr.address.ss_family == AF_INET6);
  EXPECT_GT(addr.addressLength, 0);
}

/**
 * @brief Test that set_enet_address reports host names that cannot be resolved.
 */
TEST(SetEnetAddressTest, RejectsUnresolvableHostName) {
  ENetAddress addr {};

  EXPECT_FALSE(net::set_enet_address(addr, "sunshine-test.invalid", 47999));
}

/**
 * @brief Test that host_create binds the dual-stack wildcard used by address_family = both.
 */
TEST_F(BindAddressTest, HostCreateBindsIpv6Wildcard) {
  config::sunshine.bind_address = "";

  boost::asio::io_context io_context;
  boost::asio::ip::udp::socket probe(io_context);
  boost::system::error_code ec;
  probe.open(boost::asio::ip::udp::v6(), ec);
  if (ec) {
    GTEST_SKIP() << "IPv6 sockets are not available: " << ec.message();
  }

  ENetAddress addr {};
  const auto host = net::host_create(net::af_e::BOTH, addr, 0);

  ASSERT_TRUE(host);
  EXPECT_EQ(host->address.address.ss_family, AF_INET6);
}

/**
 * @brief Test that host_create returns an empty handle when the bind address cannot be resolved.
 */
TEST_F(BindAddressTest, HostCreateReturnsEmptyHandleForUnresolvableAddress) {
  config::sunshine.bind_address = "sunshine-test.invalid";

  ENetAddress addr {};
  const auto host = net::host_create(net::af_e::IPV4, addr, 0);

  EXPECT_FALSE(host);
}
