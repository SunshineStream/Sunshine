/**
 * @file tests/unit/test_upnp.cpp
 * @brief Test src/upnp.*
 */

// test includes
#include "../tests_common.h"

// local includes
#include <src/upnp.h>

TEST(UpnpIsValidIgdTest, RejectsNoIgd) {
  ASSERT_FALSE(upnp::is_valid_igd(upnp::igd_status_e::none));
}

TEST(UpnpIsValidIgdTest, AcceptsConnectedIgd) {
  ASSERT_TRUE(upnp::is_valid_igd(upnp::igd_status_e::connected));
}

#if (MINIUPNPC_API_VERSION >= 18)
TEST(UpnpIsValidIgdTest, AcceptsPrivateIpIgd) {
  // MiniUPnPc 2.2.8+ reports a valid connected IGD with a private IP address as igd_status_e::private_ip.
  ASSERT_TRUE(upnp::is_valid_igd(upnp::igd_status_e::private_ip));
}
#endif

TEST(UpnpIsValidIgdTest, AcceptsDisconnectedIgd) {
  // Regression test for #5462: MiniUPnPc 2.2.8+ reports a valid but disconnected IGD as
  // igd_status_e::disconnected. The old check (status == 1 || status == 2) rejected it, so UPnP port
  // mapping silently failed for routers advertising a valid IGD with NewConnectionStatus=Disconnected.
  ASSERT_TRUE(upnp::is_valid_igd(upnp::igd_status_e::disconnected));
}

TEST(UpnpIsValidIgdTest, RejectsUnknownDevice) {
  ASSERT_FALSE(upnp::is_valid_igd(upnp::igd_status_e::unknown_device));
}

TEST(UpnpIsValidIgdTest, RejectsOutOfRange) {
  ASSERT_FALSE(upnp::is_valid_igd(static_cast<upnp::igd_status_e>(-1)));
  ASSERT_FALSE(upnp::is_valid_igd(static_cast<upnp::igd_status_e>(5)));
  ASSERT_FALSE(upnp::is_valid_igd(static_cast<upnp::igd_status_e>(100)));
}

TEST(UpnpSharedPrefixLengthTest, CountsSharedLeadingBits) {
  EXPECT_EQ(upnp::shared_prefix_length("192.168.20.181", "192.168.20.181"), 32);
  EXPECT_EQ(upnp::shared_prefix_length("192.168.20.181", "192.168.1.100"), 19);
  EXPECT_EQ(upnp::shared_prefix_length("10.0.0.86", "10.0.0.1"), 25);
  EXPECT_EQ(upnp::shared_prefix_length("10.0.0.1", "192.168.1.1"), 0);
}

TEST(UpnpSharedPrefixLengthTest, RejectsNonIpv4Values) {
  EXPECT_EQ(upnp::shared_prefix_length("router.local", "192.168.1.1"), -1);
  EXPECT_EQ(upnp::shared_prefix_length("192.168.1.1", "fe80::1"), -1);
  EXPECT_EQ(upnp::shared_prefix_length("", ""), -1);
}

TEST(UpnpUrlHostTest, ExtractsHost) {
  EXPECT_EQ(upnp::url_host("http://192.168.1.100:1900/rootDesc.xml"), "192.168.1.100");
  EXPECT_EQ(upnp::url_host("http://10.0.0.1/rootDesc.xml"), "10.0.0.1");
  EXPECT_EQ(upnp::url_host("http://[fe80::1]:5000/rootDesc.xml"), "fe80::1");
  EXPECT_EQ(upnp::url_host("192.168.1.1:80"), "192.168.1.1");
}

TEST(UpnpSelectInnerIgdTest, KeepsFirstIgdWhenAlone) {
  EXPECT_EQ(upnp::select_inner_igd({"192.168.1.1"}, "192.168.1.20"), 0U);
  EXPECT_EQ(upnp::select_inner_igd({}, "192.168.1.20"), 0U);
}

TEST(UpnpSelectInnerIgdTest, PrefersIgdOnHostNetwork) {
  // Double NAT: UPNP_GetValidIGD() chose the outer router (public address), the inner router shares the host's network.
  EXPECT_EQ(upnp::select_inner_igd({"10.0.0.1", "192.168.1.100"}, "192.168.20.181"), 1U);
}

TEST(UpnpSelectInnerIgdTest, KeepsFirstIgdOnTie) {
  EXPECT_EQ(upnp::select_inner_igd({"192.168.1.1", "192.168.1.2"}, "192.168.1.20"), 0U);
  EXPECT_EQ(upnp::select_inner_igd({"router.local", "gateway.local"}, "192.168.1.20"), 0U);
}

TEST(UpnpSelectOuterIgdTest, FindsIgdInFrontOfGateway) {
  EXPECT_EQ(upnp::select_outer_igd({"192.168.1.100", "10.0.0.1"}, 0, "10.0.0.86"), 1U);
}

TEST(UpnpSelectOuterIgdTest, IgnoresGatewayWithPublicAddress) {
  EXPECT_EQ(upnp::select_outer_igd({"192.168.1.100", "10.0.0.1"}, 0, "203.0.113.5"), std::nullopt);
}

TEST(UpnpSelectOuterIgdTest, IgnoresMissingOrInvalidExternalAddress) {
  EXPECT_EQ(upnp::select_outer_igd({"192.168.1.100", "10.0.0.1"}, 0, ""), std::nullopt);
  EXPECT_EQ(upnp::select_outer_igd({"192.168.1.100", "10.0.0.1"}, 0, "not-an-address"), std::nullopt);
  EXPECT_EQ(upnp::select_outer_igd({"192.168.1.100", "10.0.0.1"}, 0, "fc00::1"), std::nullopt);
}

TEST(UpnpSelectOuterIgdTest, IgnoresInnerGatewayAndUnrelatedIgds) {
  // The only other IGD is on the host's own network, not in front of the gateway.
  EXPECT_EQ(upnp::select_outer_igd({"192.168.1.100", "192.168.1.2"}, 0, "10.0.0.86"), std::nullopt);
  EXPECT_EQ(upnp::select_outer_igd({"10.0.0.1"}, 0, "10.0.0.86"), std::nullopt);
}

TEST(UpnpSelectOuterIgdTest, PrefersClosestIgd) {
  EXPECT_EQ(upnp::select_outer_igd({"192.168.1.100", "10.0.5.1", "10.0.0.1"}, 0, "10.0.0.86"), 2U);
}
