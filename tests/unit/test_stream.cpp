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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <rs.h>

// local includes
#include <src/bitrate_controller.h>
#include <src/config.h>
#include <src/network.h>
#include <src/utility.h>

namespace stream {
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
  std::optional<std::pair<std::uint16_t, std::string_view>> parse_control_packet(const ENetPacket &packet);
  bool test_receive_ping_endpoints(bool session_id_pings);
  std::unique_ptr<bitrate::bitrate_controller_t> make_bitrate_controller(int negotiated_kbps);

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

namespace {
  /**
   * @brief Restores the stream and video settings changed by an adaptive bitrate test.
   */
  class StreamBitrateControllerTest: public BaseTest {
  protected:
    /**
     * @brief Restore the saved settings.
     */
    void TearDown() override {
      config::stream = original_stream;
      config::video.max_bitrate = original_max_bitrate;
      BaseTest::TearDown();
    }

  private:
    config::stream_t original_stream = config::stream;  ///< Stream settings before the test.
    int original_max_bitrate = config::video.max_bitrate;  ///< Host bitrate ceiling before the test.
  };
}  // namespace

/**
 * @brief Fixed bitrate mode does not create a controller.
 */
TEST_F(StreamBitrateControllerTest, FixedModeHasNoController) {
  config::stream.bitrate_mode = "fixed";
  EXPECT_EQ(stream::make_bitrate_controller(20000), nullptr);
}

/**
 * @brief Adaptive mode starts from the negotiated bitrate.
 */
TEST_F(StreamBitrateControllerTest, AdaptiveModeStartsAtTheNegotiatedBitrate) {
  config::stream.bitrate_mode = "adaptive";
  config::video.max_bitrate = 0;
  const auto controller = stream::make_bitrate_controller(20000);
  ASSERT_NE(controller, nullptr);
  EXPECT_EQ(controller->current_target_kbps(), 20000);
}

/**
 * @brief Adaptive mode starts from the host bitrate ceiling when it is lower than the negotiated bitrate.
 */
TEST_F(StreamBitrateControllerTest, AdaptiveModeStartsAtTheHostCeiling) {
  config::stream.bitrate_mode = "adaptive";
  config::video.max_bitrate = 8000;
  const auto controller = stream::make_bitrate_controller(20000);
  ASSERT_NE(controller, nullptr);
  EXPECT_EQ(controller->current_target_kbps(), 8000);
}

/**
 * @brief The controller uses the configured tuning parameters.
 */
TEST_F(StreamBitrateControllerTest, AdaptiveModeUsesTheConfiguredParameters) {
  config::stream.bitrate_mode = "adaptive";
  config::video.max_bitrate = 0;
  config::stream.abr_rtt_threshold_ms = 50;
  config::stream.abr_hysteresis_ms = 500;
  config::stream.abr_beta = 0.5;
  config::stream.abr_min_kbps = 6000;
  const auto controller = stream::make_bitrate_controller(20000);
  ASSERT_NE(controller, nullptr);

  std::vector<int> changes;
  std::chrono::steady_clock::time_point now {};
  for (int i = 0; i < 200; ++i) {
    now += std::chrono::milliseconds {16};
    if (auto target = controller->on_sample(now, 60); target.has_value()) {
      changes.push_back(*target);
    }
  }
  EXPECT_EQ(changes, (std::vector {10000, 6000}));
}
