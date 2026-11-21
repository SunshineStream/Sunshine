/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// lib includes
#include <rs.h>

// local includes
#include <src/network.h>
#include <src/rtsp.h>
#include <src/stream.h>

namespace stream {
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
  std::optional<std::pair<std::uint16_t, std::string_view>> parse_control_packet(const ENetPacket &packet);
  bool test_receive_ping_endpoints(bool session_id_pings);

  namespace fec {
    /**
     * @brief Return the production video FEC data-shard limit.
     *
     * @param percentage Recovery-shard percentage between 0 and 100.
     * @return Maximum data shards in one 8-bit Reed-Solomon block.
     */
    int max_data_shards(int percentage);
  }  // namespace fec
}  // namespace stream

/**
 * @brief Preserve the 8-bit shard limits for disabled, default, and maximum FEC.
 */
TEST(StreamFecTests, PreservesVideoShardLimits) {
  EXPECT_EQ(stream::fec::max_data_shards(0), 255);
  EXPECT_EQ(stream::fec::max_data_shards(20), 212);
  EXPECT_EQ(stream::fec::max_data_shards(100), 127);
}

/**
 * @brief Every supported FEC percentage fills the encoder without exceeding its shard limit.
 */
TEST(StreamFecTests, FitsDataAndRoundedParityWithinEncoderLimit) {
  for (int percentage = 1; percentage <= 100; ++percentage) {
    SCOPED_TRACE(percentage);
    const auto data_shards = stream::fec::max_data_shards(percentage);
    const auto parity_shards = (data_shards * percentage + 99) / 100;
    auto encoder = reed_solomon_new(data_shards, parity_shards);
    ASSERT_NE(encoder, nullptr);
    reed_solomon_release(encoder);

    const auto next_data_shards = data_shards + 1;
    const auto next_parity_shards = (next_data_shards * percentage + 99) / 100;
    EXPECT_GT(next_data_shards + next_parity_shards, RS8_DATA_SHARDS_MAX);
  }
}

TEST(StreamPingTests, KeepsLegacyAudioAndVideoSenderEndpointsSeparate) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    ASSERT_TRUE(stream::test_receive_ping_endpoints(false)) << "Attempt " << attempt;
  }
}

TEST(StreamPingTests, KeepsSessionIdAudioAndVideoSenderEndpointsSeparate) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    ASSERT_TRUE(stream::test_receive_ping_endpoints(true)) << "Attempt " << attempt;
  }
}

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  std::array<char, 2> b1 = {'a', 'b'};
  std::array<char, 3> b2 = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(0, 2, std::string_view {b1.data(), b1.size()}, std::string_view {b2.data(), b2.size()});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  std::array<char, 2> b1 = {'a', 'b'};
  std::array<char, 3> b2 = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, b1.size() + b2.size() + 1, std::string_view {b1.data(), b1.size()}, std::string_view {b2.data(), b2.size()});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  std::array<char, 2> b1 = {'a', 'b'};
  std::array<char, 3> b2 = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, 1, std::string_view {b1.data(), b1.size()}, std::string_view {b2.data(), b2.size()});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

/**
 * @brief Protect only the critical prefix and respect Reed-Solomon and wire-header limits.
 */
TEST(PyrowaveFecTest, ProtectsCriticalPacketsWithoutAddingParityToDetail) {
  for (const auto &[total, critical, percentage, minimum, expected] : std::vector<std::tuple<size_t, size_t, size_t, size_t, std::vector<std::pair<size_t, size_t>>>> {
         {100, 10, 20, 0, {{10, 20}, {90, 0}}},
         {1, 1, 20, 0, {{1, 20}}},
         {10, 10, 20, 10, {{10, 20}}},
         {10, 1, 20, 3, {{10, 0}}},
         {100, 10, 0, 0, {{100, 0}}},
         {100, 0, 20, 0, {{100, 0}}},
         {300, 213, 20, 0, {{300, 0}}},
         {300, 212, 20, 0, {{212, 20}, {88, 0}}},
         {300, 256, 20, 0, {{300, 0}}},
         {300, 10, 20, 256, {{300, 0}}},
         {3000, 10, 20, 0, {{10, 20}, {1023, 0}, {1023, 0}, {944, 0}}},
         {4092, 10, 20, 0, {{1023, 0}, {1023, 0}, {1023, 0}, {1023, 0}}},
         {0, 0, 20, 0, {}},
         {4093, 0, 20, 0, {}},
         {10, 11, 20, 0, {}},
         {10, 1, 101, 0, {}}
       }) {
    auto plan = stream::pyrowave_fec_blocks(total, critical, percentage, minimum);
    std::vector<std::pair<size_t, size_t>> actual;
    for (const auto &block : plan) {
      actual.emplace_back(block.data_shards, block.percentage);
    }
    EXPECT_EQ(actual, expected) << "packets " << total << ", critical " << critical << ", FEC " << percentage << ", minimum " << minimum;
  }
}

TEST(ControlPacketTests, RejectsZeroLengthPacket) {
  net::packet_t packet {enet_packet_create(nullptr, 0, 0)};

  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(packet->data, nullptr);
  EXPECT_EQ(stream::parse_control_packet(*packet), std::nullopt);
}

TEST(ControlPacketTests, RejectsOneBytePacket) {
  const std::uint8_t data {0x06};
  net::packet_t packet {enet_packet_create(&data, sizeof(data), 0)};

  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(stream::parse_control_packet(*packet), std::nullopt);
}

TEST(ControlPacketTests, AcceptsTypeWithoutPayload) {
  const std::array<std::uint8_t, 2> data {0x06, 0x02};
  net::packet_t packet {enet_packet_create(data.data(), data.size(), 0)};

  ASSERT_NE(packet, nullptr);
  auto message = stream::parse_control_packet(*packet);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->first, 0x0206);
  EXPECT_TRUE(message->second.empty());
}

TEST(ControlPacketTests, AcceptsTypeAndPayload) {
  const std::array<std::uint8_t, 5> data {0x06, 0x02, 'a', 'b', 'c'};
  net::packet_t packet {enet_packet_create(data.data(), data.size(), 0)};

  ASSERT_NE(packet, nullptr);
  auto message = stream::parse_control_packet(*packet);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->first, 0x0206);
  EXPECT_EQ(message->second, "abc");
}
