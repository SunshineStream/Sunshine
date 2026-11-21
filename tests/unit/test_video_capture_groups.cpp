/**
 * @file tests/unit/test_video_capture_groups.cpp
 * @brief Regressions for capture groups with mixed encoder backends.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <memory>
#include <vector>

// local includes
#include <src/video.h>
#include <src/video_capture_groups.h>

namespace {
  /**
   * @brief Capture context whose backend identity and pending frames must stay isolated.
   */
  struct capture_context_t {
    int backend = 0;  ///< Identity assigned by the capture backend.
    std::vector<int> pending_frames;  ///< Frames waiting only for this group's clients.
  };

  /**
   * @brief Exercise software-first and hardware-first session arrival orders.
   */
  class CaptureGroupOrderTest: public BaseTest, public testing::WithParamInterface<bool> {};
}  // namespace

/**
 * @brief Sessions on different backends never inherit the first session's capture context.
 */
TEST_P(CaptureGroupOrderTest, SeparatesBackendsAndRetainsOtherClientsAfterOneLeaves) {
  int starts = 0;
  int stops = 0;
  video::capture_group_pool_t<capture_context_t> groups {
    [&](auto &) {
      ++starts;
      return 0;
    },
    [&](auto &) {
      ++stops;
    },
  };
  video::encoder_t hardware {};
  video::encoder_t software {};
  const auto *first_encoder = GetParam() ? &software : &hardware;
  const auto *second_encoder = GetParam() ? &hardware : &software;
  auto first = groups.ref(first_encoder);
  auto second = groups.ref(second_encoder);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first.get(), second.get());
  first->backend = 1;
  second->backend = 2;
  first->pending_frames.push_back(10);
  second->pending_frames.push_back(20);

  auto same_backend = groups.ref(first_encoder);
  EXPECT_EQ(same_backend.get(), first.get());
  EXPECT_EQ(starts, 2);
  first = {};
  EXPECT_EQ(stops, 0);
  EXPECT_EQ(same_backend->pending_frames, std::vector<int> {10});
  same_backend = {};
  EXPECT_EQ(stops, 1);
  EXPECT_EQ(second->backend, 2);
  EXPECT_EQ(second->pending_frames, std::vector<int> {20});
  second = {};
  EXPECT_EQ(stops, 2);
  auto restarted = groups.ref(first_encoder);
  EXPECT_EQ(starts, 3);
  EXPECT_TRUE(restarted->pending_frames.empty());
  EXPECT_EQ(restarted->backend, 0);
  EXPECT_FALSE(groups.ref(nullptr));
}

INSTANTIATE_TEST_SUITE_P(SessionArrivalOrder, CaptureGroupOrderTest, testing::Bool());

/**
 * @brief A monitor request reaches every active group after its raw event has been consumed.
 */
TEST(VideoCaptureMonitorTest, BroadcastsRequestsAndResetsBetweenStreamEpochs) {
  video::display_switch_state_t state;
  auto mailbox = std::make_shared<safe::mail_raw_t>();
  auto request = mailbox->event<int>("monitor");
  state.start_worker();
  state.start_worker();
  request->raise(2);
  const auto hardware = state.poll(request);
  const auto software = state.poll(request);
  ASSERT_TRUE(hardware.index);
  EXPECT_EQ(*hardware.index, 2);
  EXPECT_EQ(software.index, hardware.index);
  EXPECT_EQ(software.generation, hardware.generation);
  EXPECT_FALSE(request->peek());
  request->raise(1);
  EXPECT_GT(state.poll(request).generation, software.generation);
  state.start_worker();
  EXPECT_EQ(state.poll(request).index, 1);
  state.stop_worker();
  state.stop_worker();
  state.stop_worker();
  state.start_worker();
  EXPECT_FALSE(state.poll(request).index);
  state.stop_worker();
}

/**
 * @brief Failed capture startup destroys its context and permits a fresh retry.
 */
TEST(VideoCaptureGroupTest, CleansUpFailedStartupBeforeRetry) {
  /**
   * @brief Track resources owned by a capture worker during startup and shutdown.
   */
  struct context_t {
    std::shared_ptr<int> lifetime;  ///< Resource whose lifetime follows the capture context.
  };

  auto resource = std::make_shared<int>(1);
  int starts = 0;
  int stops = 0;
  video::capture_group_pool_t<context_t> groups {
    [&](auto &context) {
      context.lifetime = resource;
      return ++starts == 1 ? -1 : 0;
    },
    [&](auto &) {
      ++stops;
    },
  };
  video::encoder_t encoder {};
  EXPECT_FALSE(groups.ref(&encoder));
  EXPECT_EQ(resource.use_count(), 1);
  EXPECT_EQ(stops, 0);
  auto retry = groups.ref(&encoder);
  ASSERT_TRUE(retry);
  EXPECT_EQ(resource.use_count(), 2);
  retry = {};
  EXPECT_EQ(stops, 1);
  EXPECT_EQ(resource.use_count(), 1);
}
