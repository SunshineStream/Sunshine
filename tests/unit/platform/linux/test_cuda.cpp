/**
 * @file tests/unit/platform/linux/test_cuda.cpp
 * @brief Tests for Linux CUDA capture selection.
 */

#ifdef __linux__

  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <bit>
  #include <chrono>
  #include <cstdint>
  #include <cstdlib>
  #include <deque>
  #include <fstream>
  #include <latch>
  #include <map>
  #include <memory>
  #include <ranges>
  #include <set>
  #include <string>
  #include <thread>
  #include <utility>
  #include <vector>

  // lib includes
  #include <glad/gl.h>
  #ifdef SUNSHINE_BUILD_CUDA
    #include <ffnvcodec/dynlink_loader.h>

extern "C" {
    #include <libavutil/frame.h>
    #include <libavutil/pixdesc.h>
}
  #endif

  // local includes
  #include "src/platform/linux/cuda.h"
  #include "src/utility.h"
  #ifdef SUNSHINE_BUILD_CUDA
    #include "src/platform/linux/cuda_gl_textures.h"
    #include "src/platform/linux/graphics.h"
  #endif

namespace {
  /**
   * @brief Expected virtual desktop selection for an NvFBC status.
   */
  struct nvfbc_virtual_desktop_case_t {
    std::uint32_t output_count;  ///< Physical outputs reported by NvFBC.
    bool capture_possible;  ///< Whether NvFBC reports capture support.
    std::uint32_t screen_width;  ///< Virtual desktop width.
    std::uint32_t screen_height;  ///< Virtual desktop height.
    bool expected;  ///< Whether Sunshine should offer the virtual desktop.
  };
}  // namespace

TEST(NvFbcDisplaySelectionTest, OffersVirtualDesktopOnlyForValidNoOutputCapture) {
  constexpr std::array cases {
    nvfbc_virtual_desktop_case_t {1U, true, 1920U, 1080U, false},
    nvfbc_virtual_desktop_case_t {0U, true, 1920U, 1080U, true},
    nvfbc_virtual_desktop_case_t {0U, false, 1920U, 1080U, false},
    nvfbc_virtual_desktop_case_t {0U, true, 0U, 1080U, false},
    nvfbc_virtual_desktop_case_t {0U, true, 1920U, 0U, false},
  };

  for (const auto &[output_count, capture_possible, screen_width, screen_height, expected] : cases) {
    EXPECT_EQ(cuda::nvfbc::should_offer_virtual_desktop(output_count, capture_possible, screen_width, screen_height), expected)
      << "output_count=" << output_count
      << ", capture_possible=" << capture_possible
      << ", screen_width=" << screen_width
      << ", screen_height=" << screen_height;
  }
}

  #ifdef SUNSHINE_BUILD_CUDA
namespace {
  /**
   * @brief Fake GL->CUDA handoff steps that record their order and check plane access.
   *
   * GL must not render into the planes while they are mapped into CUDA or before
   * CUDA has copied the previous frame out, and CUDA must not copy them before GL
   * has finished rendering.
   */
  struct fake_handoff_t {
    std::vector<std::string> calls;  ///< Steps in the order they ran.
    std::string failing_step;  ///< Step that returns an error, or empty.
    int failing_plane = -1;  ///< Plane whose copy returns an error, or -1.
    bool mapped = false;  ///< Whether the planes are mapped into CUDA.
    bool gl_finished = false;  ///< Whether GL has finished the current frame's rendering.
    bool copies_finished = true;  ///< Whether CUDA has finished the previous frame's copies.

    /**
     * @brief Record a step and return its result.
     *
     * @param name Step name.
     * @return -1 when `name` is the failing step, 0 otherwise.
     */
    int step(const std::string &name) {
      calls.push_back(name);
      return name == failing_step ? -1 : 0;
    }

    /**
     * @brief Handoff steps backed by this fake.
     *
     * @return The steps.
     */
    cuda::gl_cuda_handoff_ops_t ops() {
      return {
        .wait_for_cuda = [this] {
          const int status = step("wait_for_cuda");
          copies_finished = copies_finished || status == 0;
          return status;
        },
        .render = [this] {
          calls.emplace_back("render");
          EXPECT_FALSE(mapped) << "GL rendered into planes mapped into CUDA";
          EXPECT_TRUE(copies_finished) << "GL rendered before CUDA copied the previous frame";
          gl_finished = false;
        },
        .wait_for_gl = [this] {
          const int status = step("wait_for_gl");
          gl_finished = status == 0;
          return status;
        },
        .map = [this] {
          const int status = step("map");
          mapped = status == 0;
          gl_finished = mapped;  // Mapping waits for GL.
          return status;
        },
        .copy = [this](int plane) {
          calls.push_back("copy" + std::to_string(plane));
          EXPECT_TRUE(gl_finished) << "CUDA copied before GL finished rendering";
          copies_finished = false;
          return plane == failing_plane ? -1 : 0;
        },
        .unmap = [this] {
          EXPECT_TRUE(mapped) << "unmapped planes that weren't mapped";
          mapped = false;
          copies_finished = true;  // Unmapping orders the copies before later GL work.
          return step("unmap");
        },
        .record_copies = [this] {
          return step("record_copies");
        },
      };
    }
  };

  /**
   * @brief Expected steps of one successful frame.
   *
   * @param shared_planes Whether the planes are shared through external memory.
   * @param plane_count Number of planes.
   * @return The steps in order.
   */
  std::vector<std::string> successful_frame(bool shared_planes, int plane_count) {
    std::vector<std::string> steps;
    if (shared_planes) {
      steps.emplace_back("wait_for_cuda");
    }
    steps.emplace_back("render");
    steps.emplace_back(shared_planes ? "wait_for_gl" : "map");
    for (int plane = 0; plane < plane_count; ++plane) {
      steps.push_back("copy" + std::to_string(plane));
    }
    steps.emplace_back(shared_planes ? "record_copies" : "unmap");
    return steps;
  }

  /**
   * @brief Plane counts: two for NV12 and P010, three for YUV 4:4:4.
   */
  class GlCudaHandoffPlanesTest: public testing::TestWithParam<int> {};
}  // namespace

TEST_P(GlCudaHandoffPlanesTest, SharedPlanesTakeTurnsAcrossFrames) {
  const int plane_count = GetParam();
  fake_handoff_t fake;

  ASSERT_EQ(cuda::gl_cuda_handoff(true, plane_count, fake.ops()), 0);
  ASSERT_EQ(cuda::gl_cuda_handoff(true, plane_count, fake.ops()), 0);

  auto expected = successful_frame(true, plane_count);
  const auto second_frame = expected;
  expected.insert(expected.end(), second_frame.begin(), second_frame.end());
  EXPECT_EQ(fake.calls, expected);
}

TEST_P(GlCudaHandoffPlanesTest, MappedPlanesAreMappedOnlyForTheCopies) {
  const int plane_count = GetParam();
  fake_handoff_t fake;

  ASSERT_EQ(cuda::gl_cuda_handoff(false, plane_count, fake.ops()), 0);
  ASSERT_EQ(cuda::gl_cuda_handoff(false, plane_count, fake.ops()), 0);

  auto expected = successful_frame(false, plane_count);
  const auto second_frame = expected;
  expected.insert(expected.end(), second_frame.begin(), second_frame.end());
  EXPECT_EQ(fake.calls, expected);
  EXPECT_FALSE(fake.mapped);
}

INSTANTIATE_TEST_SUITE_P(PlaneCounts, GlCudaHandoffPlanesTest, testing::Values(2, 3));

TEST(GlCudaHandoffTest, SharedPlanesStopWhenWaitingForCudaFails) {
  fake_handoff_t fake {.failing_step = "wait_for_cuda"};

  EXPECT_EQ(cuda::gl_cuda_handoff(true, 2, fake.ops()), -1);
  EXPECT_EQ(fake.calls, (std::vector<std::string> {"wait_for_cuda"}));
}

TEST(GlCudaHandoffTest, SharedPlanesSkipTheCopiesWhenWaitingForGlFails) {
  fake_handoff_t fake {.failing_step = "wait_for_gl"};

  EXPECT_EQ(cuda::gl_cuda_handoff(true, 2, fake.ops()), -1);
  EXPECT_EQ(fake.calls, (std::vector<std::string> {"wait_for_cuda", "render", "wait_for_gl"}));
}

TEST(GlCudaHandoffTest, SharedPlanesRecordQueuedCopiesWhenACopyFails) {
  fake_handoff_t fake {.failing_plane = 0};

  EXPECT_EQ(cuda::gl_cuda_handoff(true, 3, fake.ops()), -1);
  EXPECT_EQ(fake.calls, (std::vector<std::string> {"wait_for_cuda", "render", "wait_for_gl", "copy0", "record_copies"}));
}

TEST(GlCudaHandoffTest, SharedPlanesFailWhenRecordingTheCopiesFails) {
  fake_handoff_t fake {.failing_step = "record_copies"};

  EXPECT_EQ(cuda::gl_cuda_handoff(true, 2, fake.ops()), -1);
  EXPECT_EQ(fake.calls, successful_frame(true, 2));
}

TEST(GlCudaHandoffTest, MappedPlanesSkipTheCopiesWhenMappingFails) {
  fake_handoff_t fake {.failing_step = "map"};

  EXPECT_EQ(cuda::gl_cuda_handoff(false, 2, fake.ops()), -1);
  EXPECT_EQ(fake.calls, (std::vector<std::string> {"render", "map"}));
  EXPECT_FALSE(fake.mapped);
}

TEST(GlCudaHandoffTest, MappedPlanesAreUnmappedOnceWhenACopyFails) {
  fake_handoff_t fake {.failing_plane = 1};

  EXPECT_EQ(cuda::gl_cuda_handoff(false, 3, fake.ops()), -1);
  EXPECT_EQ(fake.calls, (std::vector<std::string> {"render", "map", "copy0", "copy1", "unmap"}));
  EXPECT_FALSE(fake.mapped);

  // The next frame starts with the planes back in GL's hands.
  fake.failing_plane = -1;
  EXPECT_EQ(cuda::gl_cuda_handoff(false, 3, fake.ops()), 0);
}

TEST(GlCudaHandoffTest, MappedPlanesFailWhenUnmappingFails) {
  fake_handoff_t fake {.failing_step = "unmap"};

  EXPECT_EQ(cuda::gl_cuda_handoff(false, 2, fake.ops()), -1);
  EXPECT_EQ(fake.calls, successful_frame(false, 2));
}

namespace {
  /**
   * @brief Fake GL fence that records how it was used.
   */
  struct fake_fence_t {
    bool create_fails = false;  ///< Whether creating the fence fails.
    std::uint32_t wait_status = GL_ALREADY_SIGNALED;  ///< Status the wait returns.
    int waits = 0;  ///< Number of waits.
    int destroys = 0;  ///< Number of deletions.

    /**
     * @brief Fence calls backed by this fake.
     *
     * @return The calls.
     */
    cuda::gl_fence_ops_t ops() {
      return {
        .create = [this] {
          return !create_fails;
        },
        .client_wait = [this] {
          ++waits;
          return wait_status;
        },
        .destroy = [this] {
          ++destroys;
        },
      };
    }
  };

  /**
   * @brief GL wait statuses and whether waiting for GL succeeds with them.
   */
  class GlFenceWaitStatusTest: public testing::TestWithParam<std::pair<std::uint32_t, int>> {};
}  // namespace

TEST(GlFenceWaitTest, FailsWithoutWaitingWhenTheFenceCannotBeCreated) {
  fake_fence_t fake {.create_fails = true};

  EXPECT_EQ(cuda::wait_for_gl_fence(fake.ops()), -1);
  EXPECT_EQ(fake.waits, 0);
  EXPECT_EQ(fake.destroys, 0);
}

TEST_P(GlFenceWaitStatusTest, DeletesTheFenceOnceAndReportsTheStatus) {
  const auto [status, expected] = GetParam();
  fake_fence_t fake {.wait_status = status};

  EXPECT_EQ(cuda::wait_for_gl_fence(fake.ops()), expected);
  EXPECT_EQ(fake.waits, 1);
  EXPECT_EQ(fake.destroys, 1);
}

INSTANTIATE_TEST_SUITE_P(
  Statuses,
  GlFenceWaitStatusTest,
  testing::Values(
    std::pair<std::uint32_t, int> {GL_ALREADY_SIGNALED, 0},
    std::pair<std::uint32_t, int> {GL_CONDITION_SATISFIED, 0},
    std::pair<std::uint32_t, int> {GL_TIMEOUT_EXPIRED, -1},
    std::pair<std::uint32_t, int> {GL_WAIT_FAILED, -1}
  )
);

namespace {
  /**
   * @brief Fake shared memory driver that records its calls and which descriptors it was given.
   */
  class fake_shared_memory_t final: public cuda::shared_memory_driver_t {
  public:
    /**
     * @brief Format and size a plane step was given.
     */
    struct plane_args_t {
      std::uint32_t format;  ///< Sized GL internal format.
      int width;  ///< Plane width.
      int height;  ///< Plane height.

      /**
       * @brief Compare all fields.
       */
      bool operator==(const plane_args_t &) const = default;
    };

    /**
     * @brief What a test wants from and sees in the fake, kept alive after the planes own the driver.
     */
    struct state_t {
      bool is_supported = true;  ///< What supported() returns.
      std::string failing_step;  ///< Step that fails, or empty when none does.
      std::size_t failing_plane = 0;  ///< Plane that `failing_step` fails on.
      int failing_export = 1;  ///< Which of a plane's exports fails, counting from 1.
      std::vector<std::string> calls;  ///< Every call, as "step" or "step:plane".
      std::set<int> open_fds;  ///< Exported descriptors that nobody has taken over or closed.
      std::map<int, std::size_t> imported_fds;  ///< Imported descriptors and the number of imports of each.
      std::vector<CUDA_EXTERNAL_MEMORY_HANDLE_DESC> memory_descs;  ///< Descriptors given to import_cuda().
      std::vector<CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC> array_descs;  ///< Descriptors given to map_array().
      std::vector<plane_args_t> memory_args;  ///< Arguments given to allocate_memory().
      std::vector<plane_args_t> storage_args;  ///< Arguments given to create_gl_storage().
      std::vector<std::uint64_t> gl_sizes;  ///< Sizes given to import_gl().
      int next_fd = 100;  ///< Next descriptor to export.

      /**
       * @brief Count the calls of one step.
       *
       * @param call Call as recorded in `calls`.
       * @return The number of those calls.
       */
      auto count(const std::string &call) const {
        return std::ranges::count(calls, call);
      }

      /**
       * @brief Find the position of a call.
       *
       * @param call Call as recorded in `calls`.
       * @return Position of its first occurrence, or the number of calls when it wasn't made.
       */
      auto position(const std::string &call) const {
        return std::ranges::find(calls, call) - calls.begin();
      }
    };

    /**
     * @brief Construct the fake.
     *
     * @param test_state State shared with the test.
     */
    explicit fake_shared_memory_t(std::shared_ptr<state_t> test_state):
        state {std::move(test_state)} {
    }

    bool supported() override {  ///< Configured answer.
      state->calls.emplace_back("supported");
      return state->is_supported;
    }

    int create_device() override {  ///< Recorded step.
      return step("create_device");
    }

    void destroy_device() override {  ///< Recorded step.
      state->calls.emplace_back("destroy_device");
    }

    int create_event() override {  ///< Recorded step.
      return step("create_event");
    }

    void destroy_event() override {  ///< Recorded step.
      state->calls.emplace_back("destroy_event");
    }

    int wait_event() override {  ///< Recorded step.
      return step("wait_event");
    }

    int record_event(CUstream) override {  ///< Recorded step.
      return step("record_event");
    }

    int allocate_memory(std::size_t plane, std::uint32_t internal_format, int width, int height, std::uint64_t &size) override {  ///< Recorded step, with a size derived from the plane.
      state->memory_args.push_back({internal_format, width, height});
      size = 4096 * (plane + 1);
      return step("allocate_memory", plane);
    }

    void free_memory(std::size_t plane) override {  ///< Recorded step.
      record("free_memory", plane);
    }

    int export_fd(std::size_t plane, int &fd) override {  ///< Recorded step, opening a new descriptor.
      record("export_fd", plane);
      if (state->failing_step == "export_fd" && state->failing_plane == plane && state->failing_export == ++exports[plane]) {
        return -1;
      }

      fd = state->next_fd++;
      state->open_fds.insert(fd);
      return 0;
    }

    void close_fd(int fd) override {  ///< Recorded step. The descriptor must be open.
      state->calls.emplace_back("close_fd");
      EXPECT_EQ(state->open_fds.erase(fd), 1u) << "closed a descriptor that wasn't open: " << fd;
    }

    int import_gl(std::size_t plane, int fd, std::uint64_t size) override {  ///< Recorded step, taking over the descriptor on success.
      state->gl_sizes.push_back(size);
      if (step("import_gl", plane)) {
        return -1;
      }

      take_over(fd);
      return 0;
    }

    int create_gl_storage(std::size_t plane, std::uint32_t internal_format, int width, int height) override {  ///< Recorded step.
      state->storage_args.push_back({internal_format, width, height});
      return step("create_gl_storage", plane);
    }

    void release_gl(std::size_t plane) override {  ///< Recorded step.
      record("release_gl", plane);
    }

    int import_cuda(std::size_t plane, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC &desc) override {  ///< Recorded step, taking over the descriptor on success.
      state->memory_descs.push_back(desc);
      if (step("import_cuda", plane)) {
        return -1;
      }

      take_over(desc.handle.fd);
      return 0;
    }

    void release_cuda_memory(std::size_t plane) override {  ///< Recorded step.
      record("release_cuda_memory", plane);
    }

    int map_array(std::size_t plane, const CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC &desc) override {  ///< Recorded step.
      state->array_descs.push_back(desc);
      return step("map_array", plane);
    }

    void release_array(std::size_t plane) override {  ///< Recorded step.
      record("release_array", plane);
    }

    CUarray array(std::size_t plane) const override {  ///< Recorded step. The fake has no arrays.
      state->calls.push_back("array:" + std::to_string(plane));
      return nullptr;
    }

  private:
    void record(const std::string &name, std::size_t plane) {  ///< Record a call on a plane.
      state->calls.push_back(name + ":" + std::to_string(plane));
    }

    int step(const std::string &name) {  ///< Record a device step, returning -1 when configured to fail.
      state->calls.push_back(name);
      return state->failing_step == name ? -1 : 0;
    }

    int step(const std::string &name, std::size_t plane) {  ///< Record a plane step, returning -1 when configured to fail.
      record(name, plane);
      return state->failing_step == name && state->failing_plane == plane ? -1 : 0;
    }

    void take_over(int fd) {  ///< Move an open descriptor to an importing API.
      EXPECT_EQ(state->open_fds.erase(fd), 1u) << "imported a descriptor that wasn't open: " << fd;
      ++state->imported_fds[fd];
    }

    std::shared_ptr<state_t> state;  ///< State shared with the test.
    std::array<int, 3> exports {};  ///< Number of exports of each plane.
  };

  /**
   * @brief Fake state and planes built on it.
   */
  struct shared_planes_fixture_t {
    std::shared_ptr<fake_shared_memory_t::state_t> state = std::make_shared<fake_shared_memory_t::state_t>();  ///< State of the fake.
    cuda::gl_cuda_shared_planes_t planes {std::make_unique<fake_shared_memory_t>(state)};  ///< Planes under test.
  };

  /**
   * @brief Expected layout of a plane format.
   */
  struct plane_format_case_t {
    std::uint32_t gl_format;  ///< Sized GL internal format.
    CUarray_format cu_format;  ///< Expected CUDA array format.
    unsigned int channels;  ///< Expected channels per pixel.
    #ifdef SUNSHINE_BUILD_VULKAN
    VkFormat vk_format;  ///< Expected Vulkan format.
    #endif
  };

  /**
   * @brief All plane formats a stream can use.
   */
  constexpr std::array plane_format_cases {
    #ifdef SUNSHINE_BUILD_VULKAN
    plane_format_case_t {GL_R8, CU_AD_FORMAT_UNSIGNED_INT8, 1, VK_FORMAT_R8_UNORM},
    plane_format_case_t {GL_RG8, CU_AD_FORMAT_UNSIGNED_INT8, 2, VK_FORMAT_R8G8_UNORM},
    plane_format_case_t {GL_R16, CU_AD_FORMAT_UNSIGNED_INT16, 1, VK_FORMAT_R16_UNORM},
    plane_format_case_t {GL_RG16, CU_AD_FORMAT_UNSIGNED_INT16, 2, VK_FORMAT_R16G16_UNORM},
    #else
    plane_format_case_t {GL_R8, CU_AD_FORMAT_UNSIGNED_INT8, 1},
    plane_format_case_t {GL_RG8, CU_AD_FORMAT_UNSIGNED_INT8, 2},
    plane_format_case_t {GL_R16, CU_AD_FORMAT_UNSIGNED_INT16, 1},
    plane_format_case_t {GL_RG16, CU_AD_FORMAT_UNSIGNED_INT16, 2},
    #endif
  };

  /**
   * @brief Tests that run for every plane format.
   */
  class CudaGlInteropFormatTest: public testing::TestWithParam<plane_format_case_t> {};

  /**
   * @brief Allocation steps that can fail on the second plane.
   */
  class CudaGlInteropFailureTest: public testing::TestWithParam<std::string> {};

  /**
   * @brief All planes' steps that must be released once when they were started.
   */
  constexpr std::array release_steps {"release_array", "release_cuda_memory", "release_gl", "free_memory"};
}  // namespace

TEST(CudaGlInteropTest, InitFailsWithoutADriver) {
  cuda::gl_cuda_shared_planes_t planes {nullptr};

  EXPECT_EQ(planes.init(), -1);
  EXPECT_FALSE(planes.is_active());
  planes.reset();
}

TEST(CudaGlInteropTest, InitFailsWithoutTouchingTheDeviceWhenUnsupported) {
  shared_planes_fixture_t f;
  f.state->is_supported = false;

  EXPECT_EQ(f.planes.init(), -1);
  EXPECT_FALSE(f.planes.is_active());
  f.planes.reset();
  EXPECT_EQ(f.state->calls, std::vector<std::string> {"supported"});
}

TEST(CudaGlInteropTest, DeviceFailureDestroysTheDeviceOnceAndSkipsTheEvent) {
  shared_planes_fixture_t f;
  f.state->failing_step = "create_device";

  EXPECT_EQ(f.planes.init(), -1);
  EXPECT_FALSE(f.planes.is_active());
  f.planes.reset();
  f.planes.reset();
  EXPECT_EQ(f.state->calls, (std::vector<std::string> {"supported", "create_device", "destroy_device"}));
}

TEST(CudaGlInteropTest, EventFailureReleasesTheEventBeforeTheDevice) {
  shared_planes_fixture_t f;
  f.state->failing_step = "create_event";

  EXPECT_EQ(f.planes.init(), -1);
  EXPECT_FALSE(f.planes.is_active());
  f.planes.reset();
  f.planes.reset();
  EXPECT_EQ(f.state->calls, (std::vector<std::string> {"supported", "create_device", "create_event", "destroy_event", "destroy_device"}));
}

TEST(CudaGlInteropTest, UnsupportedFormatTouchesNothing) {
  shared_planes_fixture_t f;

  EXPECT_EQ(f.planes.init(), 0);
  f.state->calls.clear();
  EXPECT_EQ(f.planes.allocate(0, GL_RGBA8, 64, 32), -1);
  EXPECT_TRUE(f.state->calls.empty());
}

TEST(CudaGlInteropTest, EverythingIsReleasedOnceInOrderAndResetIsRepeatable) {
  shared_planes_fixture_t f;

  ASSERT_EQ(f.planes.init(), 0);
  EXPECT_TRUE(f.planes.is_active());
  for (std::size_t plane = 0; plane < 3; ++plane) {
    ASSERT_EQ(f.planes.allocate(plane, GL_R8, 64, 32), 0);
  }
  EXPECT_TRUE(f.state->open_fds.empty());

  f.state->calls.clear();
  f.planes.reset();
  EXPECT_FALSE(f.planes.is_active());
  const std::vector<std::string> expected {
    "release_array:0",
    "release_cuda_memory:0",
    "release_gl:0",
    "free_memory:0",
    "release_array:1",
    "release_cuda_memory:1",
    "release_gl:1",
    "free_memory:1",
    "release_array:2",
    "release_cuda_memory:2",
    "release_gl:2",
    "free_memory:2",
    "destroy_event",
    "destroy_device",
  };
  EXPECT_EQ(f.state->calls, expected);

  f.planes.reset();
  EXPECT_EQ(f.state->calls, expected);
}

TEST(CudaGlInteropTest, ArrayComesFromTheRequestedPlane) {
  shared_planes_fixture_t f;

  EXPECT_EQ(f.planes.array(2), nullptr);
  EXPECT_EQ(f.state->count("array:2"), 1);
}

TEST(CudaGlInteropTest, DestructionReleasesEverything) {
  const auto state = std::make_shared<fake_shared_memory_t::state_t>();
  {
    cuda::gl_cuda_shared_planes_t planes {std::make_unique<fake_shared_memory_t>(state)};
    ASSERT_EQ(planes.init(), 0);
    ASSERT_EQ(planes.allocate(0, GL_R8, 64, 32), 0);
  }

  EXPECT_EQ(state->count("free_memory:0"), 1);
  EXPECT_EQ(state->count("destroy_device"), 1);
}

TEST(CudaGlInteropTest, WaitsForCudaOnlyAfterCopiesWereRecorded) {
  shared_planes_fixture_t f;
  ASSERT_EQ(f.planes.init(), 0);

  EXPECT_EQ(f.planes.wait_for_cuda(), 0);
  EXPECT_EQ(f.state->count("wait_event"), 0);

  EXPECT_EQ(f.planes.record_copies(nullptr), 0);
  EXPECT_EQ(f.planes.wait_for_cuda(), 0);
  EXPECT_EQ(f.state->count("wait_event"), 1);

  // That wait consumed the recorded copies.
  EXPECT_EQ(f.planes.wait_for_cuda(), 0);
  EXPECT_EQ(f.state->count("wait_event"), 1);
}

TEST(CudaGlInteropTest, FailedWaitIsReportedAndNotRepeated) {
  shared_planes_fixture_t f;
  ASSERT_EQ(f.planes.init(), 0);
  ASSERT_EQ(f.planes.record_copies(nullptr), 0);
  f.state->failing_step = "wait_event";

  EXPECT_EQ(f.planes.wait_for_cuda(), -1);
  EXPECT_EQ(f.planes.wait_for_cuda(), 0);
  EXPECT_EQ(f.state->count("wait_event"), 1);
}

TEST(CudaGlInteropTest, FailedRecordLeavesNothingToWaitFor) {
  shared_planes_fixture_t f;
  ASSERT_EQ(f.planes.init(), 0);
  f.state->failing_step = "record_event";

  EXPECT_EQ(f.planes.record_copies(nullptr), -1);
  EXPECT_EQ(f.planes.wait_for_cuda(), 0);
  EXPECT_EQ(f.state->count("wait_event"), 0);
}

TEST(CudaGlInteropTest, ResetForgetsRecordedCopies) {
  shared_planes_fixture_t f;
  ASSERT_EQ(f.planes.init(), 0);
  ASSERT_EQ(f.planes.record_copies(nullptr), 0);

  f.planes.reset();

  EXPECT_EQ(f.planes.wait_for_cuda(), 0);
  EXPECT_EQ(f.state->count("wait_event"), 0);
}

TEST_P(CudaGlInteropFormatTest, FormatMapsToItsLayout) {
  const auto &expected = GetParam();

  const auto format = cuda::shared_plane_format(expected.gl_format);

  ASSERT_TRUE(format.has_value());
  EXPECT_EQ(format->gl_format, expected.gl_format);
  EXPECT_EQ(format->cu_format, expected.cu_format);
  EXPECT_EQ(format->channels, expected.channels);
}

TEST_P(CudaGlInteropFormatTest, ImportsDescribeThePlane) {
  const auto &expected = GetParam();
  shared_planes_fixture_t f;
  ASSERT_EQ(f.planes.init(), 0);

  ASSERT_EQ(f.planes.allocate(1, expected.gl_format, 640, 360), 0);

  EXPECT_EQ(f.state->gl_sizes, std::vector<std::uint64_t> {4096 * 2});
  const std::vector<fake_shared_memory_t::plane_args_t> plane_args {{expected.gl_format, 640, 360}};
  EXPECT_EQ(f.state->memory_args, plane_args);
  EXPECT_EQ(f.state->storage_args, plane_args);

  ASSERT_EQ(f.state->memory_descs.size(), 1u);
  const auto &memory = f.state->memory_descs[0];
  EXPECT_EQ(memory.type, CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD);
  EXPECT_EQ(memory.size, 4096u * 2);
  EXPECT_EQ(memory.flags, 0x1u);  // CUDA_EXTERNAL_MEMORY_DEDICATED

  ASSERT_EQ(f.state->array_descs.size(), 1u);
  const auto &array = f.state->array_descs[0];
  EXPECT_EQ(array.offset, 0u);
  EXPECT_EQ(array.numLevels, 1u);
  EXPECT_EQ(array.arrayDesc.Width, 640u);
  EXPECT_EQ(array.arrayDesc.Height, 360u);
  EXPECT_EQ(array.arrayDesc.Depth, 0u);
  EXPECT_EQ(array.arrayDesc.Format, expected.cu_format);
  EXPECT_EQ(array.arrayDesc.NumChannels, expected.channels);
  EXPECT_EQ(array.arrayDesc.Flags, 0x20u);  // CUDA_ARRAY3D_COLOR_ATTACHMENT

  // GL and CUDA each got their own descriptor.
  EXPECT_TRUE(f.state->open_fds.empty());
  EXPECT_EQ(f.state->imported_fds.size(), 2u);
}

    #ifdef SUNSHINE_BUILD_VULKAN
TEST_P(CudaGlInteropFormatTest, VulkanImageCanAliasAGlTexture) {
  const auto &expected = GetParam();
  VkExternalMemoryImageCreateInfo external_info = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};

  const auto info = cuda::shared_plane_image_info(expected.gl_format, 640, 360, 0, external_info);

  EXPECT_EQ(info.sType, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
  EXPECT_EQ(info.pNext, &external_info);
  EXPECT_EQ(info.format, expected.vk_format);
  EXPECT_EQ(cuda::shared_plane_vk_format(expected.gl_format), expected.vk_format);
  EXPECT_EQ(info.imageType, VK_IMAGE_TYPE_2D);
  EXPECT_EQ(info.extent.width, 640u);
  EXPECT_EQ(info.extent.height, 360u);
  EXPECT_EQ(info.extent.depth, 1u);
  EXPECT_EQ(info.mipLevels, 1u);
  EXPECT_EQ(info.arrayLayers, 1u);
  EXPECT_EQ(info.samples, VK_SAMPLE_COUNT_1_BIT);
  EXPECT_EQ(info.tiling, VK_IMAGE_TILING_OPTIMAL);

  // EXT_external_objects issue 17.
  EXPECT_EQ(info.flags, static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT));

  // EXT_external_objects issue 13: every usage the format supports.
  const VkImageUsageFlags always = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  EXPECT_EQ(info.usage, always);

  const auto with_storage = cuda::shared_plane_image_info(expected.gl_format, 640, 360, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, external_info);
  EXPECT_EQ(with_storage.usage, always | VK_IMAGE_USAGE_STORAGE_BIT);
}

TEST(CudaGlInteropTest, UnknownFormatHasNoVulkanFormat) {
  EXPECT_EQ(cuda::shared_plane_vk_format(GL_RGBA8), VK_FORMAT_UNDEFINED);
}
    #endif

INSTANTIATE_TEST_SUITE_P(PlaneFormats, CudaGlInteropFormatTest, testing::ValuesIn(plane_format_cases));

TEST_P(CudaGlInteropFailureTest, SecondPlaneFailureReleasesEverythingStartedOnce) {
  shared_planes_fixture_t f;
  f.state->failing_step = GetParam();
  f.state->failing_plane = 1;
  ASSERT_EQ(f.planes.init(), 0);
  ASSERT_EQ(f.planes.allocate(0, GL_R8, 64, 32), 0);

  EXPECT_EQ(f.planes.allocate(1, GL_RG8, 32, 16), -1);

  // Nothing leaks, whoever ended up holding each descriptor.
  EXPECT_TRUE(f.state->open_fds.empty());
  for (const auto &[fd, imports] : f.state->imported_fds) {
    EXPECT_EQ(imports, 1u) << "descriptor " << fd;
  }

  // The steps after the failed one never ran.
  constexpr std::array steps {"allocate_memory", "export_fd", "import_gl", "create_gl_storage", "import_cuda", "map_array"};
  const auto failed = std::ranges::find(steps, GetParam()) - steps.begin();
  for (const auto *later : steps | std::views::drop(failed + 1)) {
    EXPECT_EQ(f.state->count(std::string(later) + ":1"), 0) << later;
  }

  const auto calls_before_reset = f.state->calls;
  f.planes.reset();
  f.planes.reset();

  // The first plane is released completely.
  for (const auto *step : release_steps) {
    EXPECT_EQ(f.state->count(std::string(step) + ":0"), 1) << step;
  }

  // The second plane releases exactly what it started.
  const std::map<std::string, std::string> acquired_by {
    {"release_array", "map_array:1"},
    {"release_cuda_memory", "import_cuda:1"},
    {"release_gl", "import_gl:1"},
    {"free_memory", "allocate_memory:1"},
  };
  for (const auto *step : release_steps) {
    const bool started = std::ranges::count(calls_before_reset, acquired_by.at(step)) == 1;
    EXPECT_EQ(f.state->count(std::string(step) + ":1"), started ? 1 : 0) << step;
  }

  // CUDA goes before GL and Vulkan, and the device goes last.
  EXPECT_LT(f.state->position("free_memory:1"), f.state->position("destroy_device"));
  if (f.state->count("release_cuda_memory:1")) {
    EXPECT_LT(f.state->position("release_cuda_memory:1"), f.state->position("release_gl:1"));
  }
  if (f.state->count("release_gl:1")) {
    EXPECT_LT(f.state->position("release_gl:1"), f.state->position("free_memory:1"));
  }
  EXPECT_EQ(f.state->count("destroy_event"), 1);
  EXPECT_EQ(f.state->count("destroy_device"), 1);
}

INSTANTIATE_TEST_SUITE_P(
  Steps,
  CudaGlInteropFailureTest,
  testing::Values("allocate_memory", "export_fd", "import_gl", "create_gl_storage", "import_cuda", "map_array")
);

TEST(CudaGlInteropTest, SecondExportFailureClosesTheFirstDescriptor) {
  shared_planes_fixture_t f;
  f.state->failing_step = "export_fd";
  f.state->failing_plane = 0;
  f.state->failing_export = 2;
  ASSERT_EQ(f.planes.init(), 0);

  EXPECT_EQ(f.planes.allocate(0, GL_R8, 64, 32), -1);

  EXPECT_EQ(f.state->count("close_fd"), 1);
  EXPECT_TRUE(f.state->open_fds.empty());
}

namespace {
  /**
   * @brief Fake steps of creating the plane textures, recording their order.
   */
  struct fake_target_t {
    std::set<std::string> failing_steps;  ///< Steps that fail.
    std::vector<std::string> calls;  ///< Steps in the order they ran.

    /**
     * @brief Target steps backed by this fake.
     *
     * @return The steps.
     */
    cuda::gl_cuda_target_ops_t ops() {
      const auto step = [this](const std::string &name) {
        return [this, name] {
          calls.push_back(name);
          return failing_steps.contains(name) ? -1 : 0;
        };
      };
      const auto void_step = [this](const std::string &name) {
        return [this, name] {
          calls.push_back(name);
        };
      };

      return {
        .init_shared = step("init_shared"),
        .create_shared_target = step("create_shared_target"),
        .reset_shared = void_step("reset_shared"),
        .create_mapped_target = step("create_mapped_target"),
        .register_mapped_target = step("register_mapped_target"),
      };
    }
  };
}  // namespace

TEST(CudaGlInteropTest, SharedTargetNeedsNoFallback) {
  fake_target_t fake;

  EXPECT_EQ(cuda::create_gl_cuda_target(fake.ops()), 0);
  EXPECT_EQ(fake.calls, (std::vector<std::string> {"init_shared", "create_shared_target"}));
}

TEST(CudaGlInteropTest, InitFailureFallsBackToMappedTarget) {
  fake_target_t fake {.failing_steps = {"init_shared"}};

  EXPECT_EQ(cuda::create_gl_cuda_target(fake.ops()), 0);
  EXPECT_EQ(fake.calls, (std::vector<std::string> {"init_shared", "reset_shared", "create_mapped_target", "register_mapped_target"}));
}

TEST(CudaGlInteropTest, SharedTargetFailureReleasesItsMemoryAndFallsBack) {
  fake_target_t fake {.failing_steps = {"create_shared_target"}};

  EXPECT_EQ(cuda::create_gl_cuda_target(fake.ops()), 0);
  EXPECT_EQ(fake.calls, (std::vector<std::string> {"init_shared", "create_shared_target", "reset_shared", "create_mapped_target", "register_mapped_target"}));
}

TEST(CudaGlInteropTest, RealPlaneFailureReleasesItsMemoryAndFallsBack) {
  shared_planes_fixture_t f;
  f.state->failing_step = "import_cuda";
  f.state->failing_plane = 1;
  fake_target_t target;

  auto ops = target.ops();
  ops.init_shared = [&f] {
    return f.planes.init();
  };
  ops.create_shared_target = [&f] {
    return f.planes.allocate(0, GL_R8, 64, 32) || f.planes.allocate(1, GL_RG8, 32, 16) ? -1 : 0;
  };
  ops.reset_shared = [&f, &target] {
    f.planes.reset();
    target.calls.emplace_back("reset_shared");
  };

  EXPECT_EQ(cuda::create_gl_cuda_target(ops), 0);

  EXPECT_FALSE(f.planes.is_active());
  EXPECT_TRUE(f.state->open_fds.empty());
  EXPECT_EQ(f.state->count("free_memory:0"), 1);
  EXPECT_EQ(f.state->count("free_memory:1"), 1);
  EXPECT_EQ(f.state->count("destroy_device"), 1);
  EXPECT_EQ(target.calls, (std::vector<std::string> {"reset_shared", "create_mapped_target", "register_mapped_target"}));
}

TEST(CudaGlInteropTest, MappedTargetFailureSkipsRegistration) {
  fake_target_t fake {.failing_steps = {"init_shared", "create_mapped_target"}};

  EXPECT_EQ(cuda::create_gl_cuda_target(fake.ops()), -1);
  EXPECT_EQ(fake.calls, (std::vector<std::string> {"init_shared", "reset_shared", "create_mapped_target"}));
}

TEST(CudaGlInteropTest, RegistrationFailureIsReported) {
  fake_target_t fake {.failing_steps = {"init_shared", "register_mapped_target"}};

  EXPECT_EQ(cuda::create_gl_cuda_target(fake.ops()), -1);
}

TEST(CudaGlInteropTest, DriverMatchesTheBuild) {
    #ifdef SUNSHINE_BUILD_VULKAN
  EXPECT_NE(cuda::make_shared_memory_driver(), nullptr);
    #else
  EXPECT_EQ(cuda::make_shared_memory_driver(), nullptr);
    #endif
}

    #ifdef SUNSHINE_BUILD_VULKAN
// The real handle types are opaque. Completing them here lets the fakes hand out real, distinct addresses.
struct VkInstance_T {};  ///< Fake Vulkan instance.

struct VkPhysicalDevice_T {};  ///< Fake Vulkan physical device.

struct VkDevice_T {};  ///< Fake Vulkan device.

struct VkImage_T {};  ///< Fake Vulkan image.

struct VkDeviceMemory_T {};  ///< Fake Vulkan allocation.

struct CUextMemory_st {};  ///< Fake CUDA external memory.

struct CUmipmappedArray_st {};  ///< Fake CUDA mipmapped array.

struct CUarray_st {};  ///< Fake CUDA array.

struct CUevent_st {};  ///< Fake CUDA event.

struct CUstream_st {};  ///< Fake CUDA stream.

struct CUgraphicsResource_st {};  ///< Fake CUDA graphics resource.

struct __GLsync {};  ///< Fake GL fence.

namespace {
  /**
   * @brief Fake Vulkan, GL and CUDA drivers, recording every call in one log so order across them can be checked.
   */
  struct fake_api_t {
    /**
     * @brief Arguments of `vkAllocateMemory`.
     */
    struct allocation_t {
      VkDeviceSize size;  ///< Allocation size.
      std::uint32_t type_index;  ///< Memory type.
      VkImage dedicated_image;  ///< Image of the chained dedicated allocation.
      VkExternalMemoryHandleTypeFlags handle_types;  ///< Handle types of the chained export info.
    };

    /**
     * @brief Arguments of `glImportMemoryFdEXT`.
     */
    struct gl_import_t {
      GLuint memory;  ///< Memory object.
      GLuint64 size;  ///< Allocation size.
      GLenum handle_type;  ///< Handle type.
      GLint fd;  ///< File descriptor.
    };

    /**
     * @brief Arguments of a GL storage call, and the texture bound during it.
     */
    struct storage_t {
      GLuint texture;  ///< Bound texture.
      GLenum target;  ///< Texture target.
      GLsizei levels;  ///< Mip levels.
      GLenum format;  ///< Sized internal format.
      GLsizei width;  ///< Width.
      GLsizei height;  ///< Height.
      GLuint memory;  ///< Memory object, or 0 for plain storage.
      GLuint64 offset;  ///< Offset into the memory object.
    };

    std::map<std::string, int> fail_at;  ///< Call name, and which call of that name fails, counting from 1.
    std::map<std::string, int> counts;  ///< Number of calls by name.
    std::vector<std::string> log;  ///< Every call, in order.
    std::multiset<std::string> live;  ///< Objects created and not yet destroyed.
    std::set<int> open_fds;  ///< Exported descriptors that nobody has taken over or closed.
    std::deque<GLenum> gl_errors;  ///< Errors glGetError will report.

    VkInstance_T instance;  ///< Vulkan instance.
    std::array<VkPhysicalDevice_T, 2> physical_devices;  ///< Vulkan physical devices. Only the second matches CUDA.
    VkDevice_T device;  ///< Vulkan device.
    std::array<VkImage_T, 3> images;  ///< Vulkan images.
    std::array<VkDeviceMemory_T, 3> memories;  ///< Vulkan allocations.
    std::array<CUextMemory_st, 3> cu_memories;  ///< CUDA external memory.
    std::array<CUmipmappedArray_st, 3> cu_mipmaps;  ///< CUDA mipmapped arrays.
    std::array<CUarray_st, 3> cu_arrays;  ///< CUDA arrays.
    CUevent_st event;  ///< CUDA event.
    CUstream_st stream;  ///< CUDA stream.
    std::array<CUgraphicsResource_st, 3> resources;  ///< CUDA graphics resources.
    std::array<CUarray_st, 3> mapped_arrays;  ///< CUDA arrays of mapped resources.
    __GLsync fence;  ///< GL fence.

    std::uint32_t memory_type_bits = 0b11;  ///< Memory types an image can use.
    bool uuid_matches = true;  ///< Whether a Vulkan device matches the CUDA device.
    GLenum fence_status = GL_ALREADY_SIGNALED;  ///< Status of waiting for a GL fence.
    int next_fd = 100;  ///< Next descriptor to export.
    GLuint next_texture = 1;  ///< Next texture name.
    GLuint next_framebuffer = 1;  ///< Next framebuffer name.
    GLuint next_gl_memory = 1;  ///< Next GL memory object name.
    GLuint bound_texture = 0;  ///< Texture bound to `GL_TEXTURE_2D`.

    std::uint32_t api_version = 0;  ///< Version given to `vkCreateInstance`.
    VkPhysicalDevice chosen_device = nullptr;  ///< Physical device given to `vkCreateDevice`.
    std::vector<std::string> device_extensions;  ///< Extensions given to `vkCreateDevice`.
    std::vector<VkImageCreateInfo> image_infos;  ///< Infos given to `vkCreateImage`.
    std::vector<VkExternalMemoryHandleTypeFlags> image_handle_types;  ///< Handle types chained to those infos.
    std::vector<allocation_t> allocations;  ///< Arguments of `vkAllocateMemory`.
    std::vector<VkDeviceMemory> bound_memories;  ///< Memory given to `vkBindImageMemory`.
    std::vector<VkMemoryGetFdInfoKHR> fd_infos;  ///< Infos given to `vkGetMemoryFdKHR`.
    std::vector<GLint> dedicated_params;  ///< Values given to `glMemoryObjectParameterivEXT`.
    std::vector<gl_import_t> gl_imports;  ///< Arguments of `glImportMemoryFdEXT`.
    std::vector<storage_t> shared_storage;  ///< Arguments of `glTexStorageMem2DEXT`.
    std::vector<storage_t> plain_storage;  ///< Arguments of `glTexStorage2D`.
    std::vector<CUDA_EXTERNAL_MEMORY_HANDLE_DESC> cu_imports;  ///< Descriptors given to `cuImportExternalMemory`.
    std::vector<CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC> cu_array_descs;  ///< Descriptors given to `cuExternalMemoryGetMappedMipmappedArray`.
    std::vector<unsigned int> cu_levels;  ///< Levels given to `cuMipmappedArrayGetLevel`.
    std::vector<CUDA_MEMCPY2D> copies;  ///< Descriptors given to `cuMemcpy2DAsync`.
    std::vector<GLuint> registered_textures;  ///< Textures given to `cuGraphicsGLRegisterImage`.
    int mapped_count = 0;  ///< Number of resources currently mapped.
    unsigned int expected_map_count = 0;  ///< Number of resources each map and unmap must act on.
    CUstream expected_stream = nullptr;  ///< Stream the copy event must be recorded on.
    CudaFunctions cuda {};  ///< CUDA function table.

    /**
     * @brief Log a call and decide whether it fails.
     *
     * @param name Call name.
     * @return True when the call must fail.
     */
    bool fails(const std::string &name) {
      log.push_back(name);
      const int call = ++counts[name];
      const auto configured = fail_at.find(name);
      return configured != fail_at.end() && configured->second == call;
    }

    /**
     * @brief Note that an object was created.
     *
     * @param object Object name.
     */
    void acquire(const std::string &object) {
      live.insert(object);
    }

    /**
     * @brief Note that an object was destroyed. It must exist.
     *
     * @param object Object name.
     */
    void release(const std::string &object) {
      const auto found = live.find(object);
      ASSERT_NE(found, live.end()) << "released twice or never created: " << object;
      live.erase(found);
    }

    /**
     * @brief Move an open descriptor to an importing API.
     *
     * @param fd File descriptor. It must be open.
     */
    void take_over(int fd) {
      EXPECT_EQ(open_fds.erase(fd), 1u) << "imported a descriptor that wasn't open: " << fd;
    }

    /**
     * @brief Find the first call of a name.
     *
     * @param name Call name.
     * @return Its position, or the size of the log when it wasn't called.
     */
    auto first(const std::string &name) const {
      return std::ranges::find(log, name) - log.begin();
    }

    /**
     * @brief Find the last call of a name.
     *
     * @param name Call name.
     * @return Its position, or -1 when it wasn't called.
     */
    auto last(const std::string &name) const {
      const auto found = std::ranges::find(log | std::views::reverse, name);
      return static_cast<long>(log.rend() - found) - 1;
    }
  };

  /**
   * @brief The fake that the C function pointers below act on.
   *
   * @return The fake.
   */
  fake_api_t &api() {
    static fake_api_t instance;
    return instance;
  }

  /**
   * @brief Index of an object in a fake's array of them.
   *
   * @param objects The array.
   * @param object The object.
   * @return The index.
   */
  template<typename Objects, typename Object>
  std::string index_of(const Objects &objects, const Object *object) {
    return std::to_string(object - objects.data());
  }

  /**
   * @brief Fake Vulkan entry points.
   *
   * @return The entry points.
   */
  cuda::vulkan_calls_t fake_vulkan_calls() {
    return {
      .create_instance = [](const VkInstanceCreateInfo *info, const VkAllocationCallbacks *, VkInstance *out) {
        if (api().fails("vkCreateInstance")) {
          return VK_ERROR_INITIALIZATION_FAILED;
        }
        api().api_version = info->pApplicationInfo->apiVersion;
        api().acquire("instance");
        *out = &api().instance;
        return VK_SUCCESS;
      },
      .destroy_instance = [](VkInstance instance, const VkAllocationCallbacks *) {
        api().log.emplace_back("vkDestroyInstance");
        if (instance) {
          api().release("instance");
        }
      },
      .enumerate_physical_devices = [](VkInstance, std::uint32_t *count, VkPhysicalDevice *devices) {
        *count = 2;
        if (devices) {
          devices[0] = &api().physical_devices[0];
          devices[1] = &api().physical_devices[1];
        }
        return VK_SUCCESS;
      },
      .get_properties2 = [](VkPhysicalDevice device, VkPhysicalDeviceProperties2 *properties) {
        const bool matches = api().uuid_matches && device == &api().physical_devices[1];
        auto *id = static_cast<VkPhysicalDeviceIDProperties *>(properties->pNext);
        std::ranges::fill(id->deviceUUID, matches ? 0x11 : 0x22);
      },
      .get_format_properties = [](VkPhysicalDevice, VkFormat, VkFormatProperties *properties) {
        properties->optimalTilingFeatures = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
      },
      .get_memory_properties = [](VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *properties) {
        properties->memoryTypeCount = 2;
        properties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        properties->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      },
      .create_device = [](VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *, VkDevice *out) {
        if (api().fails("vkCreateDevice")) {
          return VK_ERROR_INITIALIZATION_FAILED;
        }
        api().chosen_device = physical_device;
        for (std::uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
          api().device_extensions.emplace_back(info->ppEnabledExtensionNames[i]);
        }
        api().acquire("device");
        *out = &api().device;
        return VK_SUCCESS;
      },
      .destroy_device = [](VkDevice device, const VkAllocationCallbacks *) {
        api().log.emplace_back("vkDestroyDevice");
        if (device) {
          api().release("device");
        }
      },
      .create_image = [](VkDevice, const VkImageCreateInfo *info, const VkAllocationCallbacks *, VkImage *out) {
        if (api().fails("vkCreateImage")) {
          return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto plane = api().image_infos.size();
        api().image_infos.push_back(*info);
        api().image_handle_types.push_back(static_cast<const VkExternalMemoryImageCreateInfo *>(info->pNext)->handleTypes);
        api().image_infos.back().pNext = nullptr;
        api().acquire("image:" + std::to_string(plane));
        *out = &api().images[plane];
        return VK_SUCCESS;
      },
      .destroy_image = [](VkDevice, VkImage image, const VkAllocationCallbacks *) {
        if (image) {
          api().log.emplace_back("vkDestroyImage");
          api().release("image:" + index_of(api().images, image));
        }
      },
      .get_image_memory_requirements = [](VkDevice, VkImage image, VkMemoryRequirements *requirements) {
        requirements->size = 4096 * static_cast<VkDeviceSize>(image - api().images.data() + 1);
        requirements->memoryTypeBits = api().memory_type_bits;
      },
      .allocate_memory = [](VkDevice, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *, VkDeviceMemory *out) {
        if (api().fails("vkAllocateMemory")) {
          return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto *exported = static_cast<const VkExportMemoryAllocateInfo *>(info->pNext);
        const auto *dedicated = static_cast<const VkMemoryDedicatedAllocateInfo *>(exported->pNext);
        const auto plane = api().allocations.size();
        api().allocations.push_back({info->allocationSize, info->memoryTypeIndex, dedicated->image, exported->handleTypes});
        api().acquire("memory:" + std::to_string(plane));
        *out = &api().memories[plane];
        return VK_SUCCESS;
      },
      .free_memory = [](VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks *) {
        if (memory) {
          api().log.emplace_back("vkFreeMemory");
          api().release("memory:" + index_of(api().memories, memory));
        }
      },
      .bind_image_memory = [](VkDevice, VkImage, VkDeviceMemory memory, VkDeviceSize) {
        if (api().fails("vkBindImageMemory")) {
          return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        api().bound_memories.push_back(memory);
        return VK_SUCCESS;
      },
      .load_get_memory_fd = [](VkDevice) -> PFN_vkGetMemoryFdKHR {
        if (api().fails("load_get_memory_fd")) {
          return nullptr;
        }
        return [](VkDevice, const VkMemoryGetFdInfoKHR *info, int *fd) {
          if (api().fails("vkGetMemoryFdKHR")) {
            return VK_ERROR_TOO_MANY_OBJECTS;
          }
          api().fd_infos.push_back(*info);
          *fd = api().next_fd++;
          api().open_fds.insert(*fd);
          return VK_SUCCESS;
        };
      },
      .close_fd = [](int fd) {
        api().log.emplace_back("close");
        EXPECT_EQ(api().open_fds.erase(fd), 1u) << "closed a descriptor that wasn't open: " << fd;
        return 0;
      },
    };
  }

  constexpr auto cu_failed = static_cast<CUresult>(1);  ///< Result of a failed CUDA call.

  /**
   * @brief Fake CUDA function table.
   *
   * @return The table.
   */
  CudaFunctions fake_cuda_functions() {
    CudaFunctions functions {};
    functions.cuGetErrorName = [](CUresult, const char **text) {
      *text = "fake";
      return CUresult {};
    };
    functions.cuGetErrorString = [](CUresult, const char **text) {
      *text = "failure";
      return CUresult {};
    };
    functions.cuCtxGetDevice = [](CUdevice *device) {
      *device = 0;
      return api().fails("cuCtxGetDevice") ? cu_failed : CUresult {};
    };
    functions.cuDeviceGetUuid = [](CUuuid *uuid, CUdevice) {
      std::ranges::fill(uuid->bytes, 0x11);
      return api().fails("cuDeviceGetUuid") ? cu_failed : CUresult {};
    };
    functions.cuEventCreate = [](CUevent *event, unsigned int) {
      if (api().fails("cuEventCreate")) {
        return cu_failed;
      }
      api().acquire("event");
      *event = &api().event;
      return CUresult {};
    };
    functions.cuEventDestroy = [](CUevent) {
      api().log.emplace_back("cuEventDestroy");
      api().release("event");
      return CUresult {};
    };
    functions.cuEventSynchronize = [](CUevent event) {
      EXPECT_EQ(event, &api().event);
      return api().fails("cuEventSynchronize") ? cu_failed : CUresult {};
    };
    functions.cuEventRecord = [](CUevent event, CUstream stream) {
      EXPECT_EQ(event, &api().event);
      EXPECT_EQ(stream, api().expected_stream);
      return api().fails("cuEventRecord") ? cu_failed : CUresult {};
    };
    functions.cuImportExternalMemory = [](CUexternalMemory *out, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC *desc) {
      if (api().fails("cuImportExternalMemory")) {
        return cu_failed;
      }
      const auto plane = api().cu_imports.size();
      api().cu_imports.push_back(*desc);
      api().take_over(desc->handle.fd);
      api().acquire("cu_memory:" + std::to_string(plane));
      *out = &api().cu_memories[plane];
      return CUresult {};
    };
    functions.cuDestroyExternalMemory = [](CUexternalMemory memory) {
      api().log.emplace_back("cuDestroyExternalMemory");
      api().release("cu_memory:" + index_of(api().cu_memories, memory));
      return CUresult {};
    };
    functions.cuExternalMemoryGetMappedMipmappedArray = [](CUmipmappedArray *out, CUexternalMemory memory, const CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC *desc) {
      if (api().fails("cuExternalMemoryGetMappedMipmappedArray")) {
        return cu_failed;
      }
      const auto plane = static_cast<std::size_t>(memory - api().cu_memories.data());
      api().cu_array_descs.push_back(*desc);
      api().acquire("cu_mipmap:" + std::to_string(plane));
      *out = &api().cu_mipmaps[plane];
      return CUresult {};
    };
    functions.cuMipmappedArrayGetLevel = [](CUarray *out, CUmipmappedArray mipmap, unsigned int level) {
      if (api().fails("cuMipmappedArrayGetLevel")) {
        return cu_failed;
      }
      api().cu_levels.push_back(level);
      *out = &api().cu_arrays[static_cast<std::size_t>(mipmap - api().cu_mipmaps.data())];
      return CUresult {};
    };
    functions.cuMipmappedArrayDestroy = [](CUmipmappedArray mipmap) {
      api().log.emplace_back("cuMipmappedArrayDestroy");
      api().release("cu_mipmap:" + index_of(api().cu_mipmaps, mipmap));
      return CUresult {};
    };
    functions.cuGraphicsGLRegisterImage = [](CUgraphicsResource *resource, GLuint texture, GLenum target, unsigned int flags) {
      if (api().fails("cuGraphicsGLRegisterImage")) {
        return cu_failed;
      }
      EXPECT_EQ(target, static_cast<GLenum>(GL_TEXTURE_2D));
      EXPECT_EQ(flags, static_cast<unsigned int>(CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY));
      api().registered_textures.push_back(texture);
      *resource = &api().resources[0];
      return CUresult {};
    };
    functions.cuGraphicsMapResources = [](unsigned int count, CUgraphicsResource *resources, CUstream stream) {
      if (api().fails("cuGraphicsMapResources")) {
        return cu_failed;
      }
      EXPECT_EQ(stream, &api().stream);
      EXPECT_EQ(count, api().expected_map_count);
      for (unsigned int i = 0; i < count; ++i) {
        EXPECT_EQ(resources[i], &api().resources[i]);
      }
      api().mapped_count = static_cast<int>(count);
      return CUresult {};
    };
    functions.cuGraphicsUnmapResources = [](unsigned int count, CUgraphicsResource *resources, CUstream stream) {
      if (api().fails("cuGraphicsUnmapResources")) {
        return cu_failed;
      }
      EXPECT_EQ(stream, &api().stream);
      EXPECT_EQ(count, api().expected_map_count);
      for (unsigned int i = 0; i < count; ++i) {
        EXPECT_EQ(resources[i], &api().resources[i]);
      }
      api().mapped_count = 0;
      return CUresult {};
    };
    functions.cuGraphicsSubResourceGetMappedArray = [](CUarray *array, CUgraphicsResource resource, unsigned int index, unsigned int level) {
      if (api().fails("cuGraphicsSubResourceGetMappedArray")) {
        return cu_failed;
      }
      EXPECT_LT(resource - api().resources.data(), api().mapped_count) << "the array of an unmapped resource";
      EXPECT_EQ(index, 0u);
      EXPECT_EQ(level, 0u);
      *array = &api().mapped_arrays[static_cast<std::size_t>(resource - api().resources.data())];
      return CUresult {};
    };
    functions.cuMemcpy2DAsync = [](const CUDA_MEMCPY2D *copy, CUstream stream) {
      EXPECT_EQ(stream, &api().stream);
      if (api().fails("cuMemcpy2DAsync")) {
        return cu_failed;
      }
      api().copies.push_back(*copy);
      return CUresult {};
    };
    return functions;
  }

  /**
   * @brief Point the GL function table at the fake.
   */
  void install_fake_gl() {
    gl::ctx.EXT_memory_object_fd = 1;
    gl::ctx.GetError = [] {
      if (api().gl_errors.empty()) {
        return GLenum {GL_NO_ERROR};
      }
      const auto error = api().gl_errors.front();
      api().gl_errors.pop_front();
      return error;
    };
    gl::ctx.GenTextures = [](GLsizei count, GLuint *textures) {
      for (const auto i : std::views::iota(0, count)) {
        textures[i] = api().next_texture++;
        api().acquire("texture:" + std::to_string(textures[i]));
      }
    };
    gl::ctx.DeleteTextures = [](GLsizei count, const GLuint *textures) {
      for (const auto i : std::views::iota(0, count)) {
        api().log.emplace_back("DeleteTextures");
        api().release("texture:" + std::to_string(textures[i]));
      }
    };
    gl::ctx.BindTexture = [](GLenum, GLuint texture) {
      api().bound_texture = texture;
    };
    gl::ctx.TexParameteri = [](GLenum, GLenum, GLint) {
    };
    gl::ctx.TexParameterfv = [](GLenum, GLenum, const GLfloat *) {
    };
    gl::ctx.TexStorage2D = [](GLenum target, GLsizei levels, GLenum format, GLsizei width, GLsizei height) {
      api().log.emplace_back("TexStorage2D");
      api().plain_storage.push_back({api().bound_texture, target, levels, format, width, height, 0, 0});
    };
    gl::ctx.GenFramebuffers = [](GLsizei count, GLuint *framebuffers) {
      for (const auto i : std::views::iota(0, count)) {
        framebuffers[i] = api().next_framebuffer++;
      }
    };
    gl::ctx.DeleteFramebuffers = [](GLsizei, const GLuint *) {
    };
    gl::ctx.BindFramebuffer = [](GLenum, GLuint) {
    };
    gl::ctx.FramebufferTexture = [](GLenum, GLenum, GLuint, GLint) {
    };
    gl::ctx.DrawBuffers = [](GLsizei, const GLenum *) {
    };
    gl::ctx.ClearBufferfv = [](GLenum, GLint, const GLfloat *) {
    };
    gl::ctx.CreateMemoryObjectsEXT = [](GLsizei, GLuint *memory) {
      api().log.emplace_back("CreateMemoryObjectsEXT");
      *memory = api().next_gl_memory++;
      api().acquire("gl_memory:" + std::to_string(*memory));
    };
    gl::ctx.DeleteMemoryObjectsEXT = [](GLsizei, const GLuint *memory) {
      api().log.emplace_back("DeleteMemoryObjectsEXT");
      api().release("gl_memory:" + std::to_string(*memory));
    };
    gl::ctx.MemoryObjectParameterivEXT = [](GLuint, GLenum parameter, const GLint *value) {
      EXPECT_EQ(parameter, static_cast<GLenum>(GL_DEDICATED_MEMORY_OBJECT_EXT));
      api().dedicated_params.push_back(*value);
    };
    gl::ctx.ImportMemoryFdEXT = [](GLuint memory, GLuint64 size, GLenum handle_type, GLint fd) {
      if (api().fails("ImportMemoryFdEXT")) {
        api().gl_errors.push_back(GL_INVALID_OPERATION);
        return;
      }
      api().gl_imports.push_back({memory, size, handle_type, fd});
      api().take_over(fd);
    };
    gl::ctx.TexStorageMem2DEXT = [](GLenum target, GLsizei levels, GLenum format, GLsizei width, GLsizei height, GLuint memory, GLuint64 offset) {
      if (api().fails("TexStorageMem2DEXT")) {
        api().gl_errors.push_back(GL_INVALID_OPERATION);
        return;
      }
      api().shared_storage.push_back({api().bound_texture, target, levels, format, width, height, memory, offset});
    };
    gl::ctx.FenceSync = [](GLenum condition, GLbitfield flags) -> GLsync {
      EXPECT_EQ(condition, static_cast<GLenum>(GL_SYNC_GPU_COMMANDS_COMPLETE));
      EXPECT_EQ(flags, 0u);
      if (api().fails("FenceSync")) {
        return nullptr;
      }
      api().acquire("fence");
      return &api().fence;
    };
    gl::ctx.ClientWaitSync = [](GLsync fence, GLbitfield flags, GLuint64 timeout) {
      EXPECT_EQ(fence, &api().fence);
      // Without the flush, a fence that was never submitted would only time out.
      EXPECT_EQ(flags, static_cast<GLbitfield>(GL_SYNC_FLUSH_COMMANDS_BIT));
      EXPECT_GT(timeout, 0u);
      api().log.emplace_back("ClientWaitSync");
      return api().fence_status;
    };
    gl::ctx.DeleteSync = [](GLsync fence) {
      EXPECT_EQ(fence, &api().fence);
      api().log.emplace_back("DeleteSync");
      api().release("fence");
    };
  }

  /**
   * @brief A plane a stream format must end up with.
   */
  struct expected_plane_t {
    GLenum gl_format;  ///< Sized GL internal format.
    VkFormat vk_format;  ///< Vulkan format.
    CUarray_format cu_format;  ///< CUDA array format.
    unsigned int channels;  ///< Channels per pixel.
    int width;  ///< Plane width.
    int height;  ///< Plane height.
  };

  /**
   * @brief A stream format and its planes.
   */
  struct stream_format_t {
    AVPixelFormat format;  ///< Frame format.
    bool is_yuv444;  ///< Whether the frame has three planes.
    std::vector<expected_plane_t> planes;  ///< Expected planes.
  };

  /**
   * @brief Every stream format, at 640x360.
   *
   * @return The formats.
   */
  std::vector<stream_format_t> stream_formats() {
    const expected_plane_t r8 {GL_R8, VK_FORMAT_R8_UNORM, CU_AD_FORMAT_UNSIGNED_INT8, 1, 640, 360};
    const expected_plane_t rg8 {GL_RG8, VK_FORMAT_R8G8_UNORM, CU_AD_FORMAT_UNSIGNED_INT8, 2, 320, 180};
    const expected_plane_t r16 {GL_R16, VK_FORMAT_R16_UNORM, CU_AD_FORMAT_UNSIGNED_INT16, 1, 640, 360};
    const expected_plane_t rg16 {GL_RG16, VK_FORMAT_R16G16_UNORM, CU_AD_FORMAT_UNSIGNED_INT16, 2, 320, 180};
    return {
      {AV_PIX_FMT_NV12, false, {r8, rg8}},
      {AV_PIX_FMT_P010LE, false, {r16, rg16}},
      {AV_PIX_FMT_YUV444P, true, {r8, r8, r8}},
      {AV_PIX_FMT_YUV444P16LE, true, {r16, r16, r16}},
    };
  }

  /**
   * @brief Tests of the real driver and texture creation on top of the fake Vulkan, GL and CUDA.
   */
  class CudaGlDriverTest: public testing::Test {
  protected:
    /**
     * @brief Install the fakes.
     */
    void SetUp() override {
      saved_gl = gl::ctx;
      api() = fake_api_t {};
      api().cuda = fake_cuda_functions();
      install_fake_gl();
    }

    /**
     * @brief Restore the real GL table.
     */
    void TearDown() override {
      textures = {};
      planes.reset();
      gl::ctx = saved_gl;
    }

    /**
     * @brief Create shared planes on the real driver and fake APIs.
     *
     * @return The planes.
     */
    static std::unique_ptr<cuda::gl_cuda_shared_planes_t> make_planes() {
      return std::make_unique<cuda::gl_cuda_shared_planes_t>(cuda::make_vulkan_shared_memory(fake_vulkan_calls(), [] {
        return static_cast<const CudaFunctions *>(&api().cuda);
      }));
    }

    /**
     * @brief Create textures for a stream format, recording which textures get registered.
     *
     * @param format Stream format.
     * @return The result of creating them.
     */
    int create(const stream_format_t &format) {
      return cuda::create_gl_cuda_textures(textures, *planes, 640, 360, format.format, format.is_yuv444, [this](std::size_t plane, std::uint32_t texture) {
        registered.emplace_back(plane, texture);
        return register_fails ? -1 : 0;
      });
    }

    /**
     * @brief Check that a stream fell back to plain, registered textures with nothing of the shared attempt left.
     *
     * @param format Stream format.
     */
    void expect_clean_fallback(const stream_format_t &format) {
      EXPECT_FALSE(planes->is_active());
      EXPECT_TRUE(api().open_fds.empty());

      // Only the plain textures remain, for each plane.
      const auto &tex = format.is_yuv444 ? textures.yuv444->tex : textures.nv12->tex;
      EXPECT_EQ(api().live.size(), format.planes.size());
      for (std::size_t plane = 0; plane < format.planes.size(); ++plane) {
        EXPECT_EQ(api().live.count("texture:" + std::to_string(tex[plane])), 1u);
      }
      ASSERT_EQ(api().plain_storage.size(), format.planes.size());
      ASSERT_EQ(registered.size(), format.planes.size());
      for (std::size_t plane = 0; plane < format.planes.size(); ++plane) {
        EXPECT_EQ(api().plain_storage[plane].format, format.planes[plane].gl_format);
        EXPECT_EQ(api().plain_storage[plane].width, format.planes[plane].width);
        EXPECT_EQ(api().plain_storage[plane].height, format.planes[plane].height);
        EXPECT_EQ(registered[plane].first, plane);
        EXPECT_EQ(registered[plane].second, api().plain_storage[plane].texture);
      }

      // Each plain texture's storage comes after the shared attempt was torn down.
      EXPECT_GT(api().first("TexStorage2D"), api().last("vkDestroyInstance"));
    }

    /**
     * @brief Check the release order of everything the shared attempt created.
     */
    static void expect_release_order() {
      const auto &a = api();
      const auto check_before = [&a](const std::string &earlier, const std::string &later) {
        if (a.counts.contains(earlier) || std::ranges::count(a.log, earlier)) {
          EXPECT_LT(a.last(earlier), a.first(later)) << earlier << " must come before " << later;
        }
      };

      // Textures can't outlive the memory backing them.
      check_before("DeleteTextures", "cuMipmappedArrayDestroy");
      check_before("DeleteTextures", "cuDestroyExternalMemory");
      check_before("DeleteTextures", "DeleteMemoryObjectsEXT");
      check_before("DeleteTextures", "vkDestroyImage");
      check_before("DeleteTextures", "vkFreeMemory");

      // Everything goes before the device that allocated it.
      for (const auto *call : {"vkDestroyImage", "vkFreeMemory", "cuEventDestroy"}) {
        check_before(call, "vkDestroyDevice");
      }
      check_before("vkDestroyDevice", "vkDestroyInstance");
    }

    /**
     * @brief Create a target frame, its plane pointers distinct and its pitches larger than the planes.
     *
     * @param format Stream format.
     * @return The frame.
     */
    AVFrame make_frame(const stream_format_t &format) {
      AVFrame frame {};
      frame.width = 640;
      frame.height = 360;
      for (std::size_t plane = 0; plane < format.planes.size(); ++plane) {
        frame.data[plane] = &frame_memory[plane];
        frame.linesize[plane] = 4096 + static_cast<int>(plane);
      }
      return frame;
    }

    /**
     * @brief Hand one frame over with the real handoff steps.
     *
     * @param format Stream format.
     * @param frame Target frame.
     * @return The result of the handoff.
     */
    int run_frame(const stream_format_t &format, const AVFrame &frame) {
      const auto plane_count = static_cast<int>(format.planes.size());
      api().expected_map_count = format.planes.size();
      api().expected_stream = &api().stream;
      const cuda::gl_cuda_frame_copy_t copy {
        .cu = api().cuda,
        .shared = *planes,
        .stream = &api().stream,
        .frame = frame,
        .fmt_desc = *av_pix_fmt_desc_get(format.format),
        .resources = {{&api().resources[0], &api().resources[1], format.is_yuv444 ? &api().resources[2] : nullptr}},
        .plane_count = plane_count,
      };

      return cuda::gl_cuda_handoff(planes->is_active(), plane_count, cuda::make_gl_cuda_handoff_ops(copy, [] {
                                     api().log.emplace_back("render");
                                   }));
    }

    /**
     * @brief Check the copies of one frame.
     *
     * @param format Stream format.
     * @param frame Target frame.
     * @param sources Arrays the copies must read from.
     */
    static void expect_copies(const stream_format_t &format, const AVFrame &frame, const std::array<CUarray_st, 3> &sources) {
      ASSERT_EQ(api().copies.size(), format.planes.size());
      for (std::size_t plane = 0; plane < format.planes.size(); ++plane) {
        SCOPED_TRACE("plane " + std::to_string(plane));
        const auto &expected = format.planes[plane];
        const auto &copy = api().copies[plane];
        const std::size_t channel_bytes = expected.cu_format == CU_AD_FORMAT_UNSIGNED_INT16 ? 2 : 1;

        EXPECT_EQ(copy.srcMemoryType, CU_MEMORYTYPE_ARRAY);
        EXPECT_EQ(copy.srcArray, &sources[plane]);
        EXPECT_EQ(copy.dstMemoryType, CU_MEMORYTYPE_DEVICE);
        EXPECT_EQ(copy.dstDevice, std::bit_cast<CUdeviceptr>(frame.data[plane]));
        EXPECT_EQ(copy.dstPitch, static_cast<std::size_t>(frame.linesize[plane]));
        EXPECT_EQ(copy.WidthInBytes, expected.width * expected.channels * channel_bytes);
        EXPECT_EQ(copy.Height, static_cast<std::size_t>(expected.height));
      }
    }

    GladGLContext saved_gl {};  ///< The GL table before the test.
    std::array<std::uint8_t, 3> frame_memory {};  ///< What the target frame's plane pointers point into.
    std::unique_ptr<cuda::gl_cuda_shared_planes_t> planes = make_planes();  ///< Planes under test.
    cuda::gl_cuda_textures_t textures;  ///< Textures under test.
    std::vector<std::pair<std::size_t, std::uint32_t>> registered;  ///< Plain textures registered with CUDA.
    bool register_fails = false;  ///< Whether registering a texture fails.
  };

  /**
   * @brief Tests that run for every stream format.
   */
  class CudaGlDriverFormatTest: public CudaGlDriverTest, public testing::WithParamInterface<stream_format_t> {};

  /**
   * @brief A failing call, and which call of that name fails.
   */
  using failing_call_t = std::pair<std::string, int>;

  /**
   * @brief Tests that fail one call while setting up shared planes.
   */
  class CudaGlDriverFailureTest: public CudaGlDriverTest, public testing::WithParamInterface<failing_call_t> {};
}  // namespace

TEST_P(CudaGlDriverFormatTest, SharedTexturesPassTheRightValuesToEveryApi) {
  const auto &format = GetParam();

  ASSERT_EQ(create(format), 0);

  EXPECT_TRUE(planes->is_active());
  EXPECT_TRUE(registered.empty());
  EXPECT_TRUE(api().plain_storage.empty());
  EXPECT_TRUE(api().open_fds.empty());

  EXPECT_EQ(api().api_version, static_cast<std::uint32_t>(VK_API_VERSION_1_1));
  EXPECT_EQ(api().chosen_device, &api().physical_devices[1]);
  EXPECT_EQ(api().device_extensions, std::vector<std::string> {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME});

  const auto count = format.planes.size();
  ASSERT_EQ(api().image_infos.size(), count);
  ASSERT_EQ(api().allocations.size(), count);
  ASSERT_EQ(api().bound_memories.size(), count);
  ASSERT_EQ(api().fd_infos.size(), count * 2);
  ASSERT_EQ(api().gl_imports.size(), count);
  ASSERT_EQ(api().dedicated_params.size(), count);
  ASSERT_EQ(api().shared_storage.size(), count);
  ASSERT_EQ(api().cu_imports.size(), count);
  ASSERT_EQ(api().cu_array_descs.size(), count);
  ASSERT_EQ(api().cu_levels.size(), count);

  const auto &tex = format.is_yuv444 ? textures.yuv444->tex : textures.nv12->tex;
  for (std::size_t plane = 0; plane < count; ++plane) {
    SCOPED_TRACE("plane " + std::to_string(plane));
    const auto &expected = format.planes[plane];
    const VkDeviceSize size = 4096 * (plane + 1);

    const auto &image = api().image_infos[plane];
    EXPECT_EQ(image.format, expected.vk_format);
    EXPECT_EQ(image.extent.width, static_cast<std::uint32_t>(expected.width));
    EXPECT_EQ(image.extent.height, static_cast<std::uint32_t>(expected.height));
    EXPECT_EQ(image.flags, static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT));
    EXPECT_TRUE(image.usage & VK_IMAGE_USAGE_STORAGE_BIT);
    EXPECT_TRUE(image.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    EXPECT_EQ(image.tiling, VK_IMAGE_TILING_OPTIMAL);
    EXPECT_EQ(api().image_handle_types[plane], static_cast<VkExternalMemoryHandleTypeFlags>(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT));

    const auto &allocation = api().allocations[plane];
    EXPECT_EQ(allocation.size, size);
    EXPECT_EQ(allocation.type_index, 1u);  // The device-local one.
    EXPECT_EQ(allocation.dedicated_image, &api().images[plane]);
    EXPECT_EQ(allocation.handle_types, static_cast<VkExternalMemoryHandleTypeFlags>(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT));
    EXPECT_EQ(api().bound_memories[plane], &api().memories[plane]);

    for (const auto &fd_info : {api().fd_infos[plane * 2], api().fd_infos[plane * 2 + 1]}) {
      EXPECT_EQ(fd_info.memory, &api().memories[plane]);
      EXPECT_EQ(fd_info.handleType, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
    }

    EXPECT_EQ(api().dedicated_params[plane], GL_TRUE);
    const auto &gl_import = api().gl_imports[plane];
    EXPECT_EQ(gl_import.size, size);
    EXPECT_EQ(gl_import.handle_type, static_cast<GLenum>(GL_HANDLE_TYPE_OPAQUE_FD_EXT));

    const auto &storage = api().shared_storage[plane];
    EXPECT_EQ(storage.texture, tex[plane]);
    EXPECT_EQ(storage.target, static_cast<GLenum>(GL_TEXTURE_2D));
    EXPECT_EQ(storage.levels, 1);
    EXPECT_EQ(storage.format, expected.gl_format);
    EXPECT_EQ(storage.width, expected.width);
    EXPECT_EQ(storage.height, expected.height);
    EXPECT_EQ(storage.memory, gl_import.memory);
    EXPECT_EQ(storage.offset, 0u);

    const auto &cu_import = api().cu_imports[plane];
    EXPECT_EQ(cu_import.type, CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD);
    EXPECT_EQ(cu_import.size, size);
    EXPECT_EQ(cu_import.flags, 0x1u);  // CUDA_EXTERNAL_MEMORY_DEDICATED
    EXPECT_NE(cu_import.handle.fd, gl_import.fd);

    const auto &array = api().cu_array_descs[plane].arrayDesc;
    EXPECT_EQ(array.Width, static_cast<std::size_t>(expected.width));
    EXPECT_EQ(array.Height, static_cast<std::size_t>(expected.height));
    EXPECT_EQ(array.Format, expected.cu_format);
    EXPECT_EQ(array.NumChannels, expected.channels);
    EXPECT_EQ(array.Flags, 0x20u);  // CUDA_ARRAY3D_COLOR_ATTACHMENT
    EXPECT_EQ(api().cu_array_descs[plane].numLevels, 1u);
    EXPECT_EQ(api().cu_levels[plane], 0u);

    EXPECT_EQ(planes->array(plane), &api().cu_arrays[plane]);
  }
}

TEST_P(CudaGlDriverFormatTest, MissingGlExtensionUsesPlainRegisteredTextures) {
  const auto &format = GetParam();
  gl::ctx.EXT_memory_object_fd = 0;

  ASSERT_EQ(create(format), 0);

  EXPECT_FALSE(planes->is_active());
  EXPECT_FALSE(api().counts.contains("vkCreateInstance"));
  ASSERT_EQ(registered.size(), format.planes.size());
  ASSERT_EQ(api().plain_storage.size(), format.planes.size());
  for (std::size_t plane = 0; plane < format.planes.size(); ++plane) {
    EXPECT_EQ(api().plain_storage[plane].format, format.planes[plane].gl_format);
    EXPECT_EQ(api().plain_storage[plane].width, format.planes[plane].width);
    EXPECT_EQ(api().plain_storage[plane].height, format.planes[plane].height);
    EXPECT_EQ(registered[plane].first, plane);
    EXPECT_EQ(registered[plane].second, api().plain_storage[plane].texture);
  }
}

TEST_P(CudaGlDriverFormatTest, EverythingIsReleasedOnceWhenTexturesThenPlanesGoAway) {
  const auto &format = GetParam();
  ASSERT_EQ(create(format), 0);

  textures = {};
  planes->reset();
  planes->reset();
  planes.reset();

  EXPECT_TRUE(api().live.empty());
  expect_release_order();
}

TEST_P(CudaGlDriverFormatTest, SharedFramesTakeTurnsAndCopyEveryPlane) {
  const auto &format = GetParam();
  ASSERT_EQ(create(format), 0);
  ASSERT_TRUE(planes->is_active());

  const auto frame = make_frame(format);
  api().log.clear();
  const auto first_frame = run_frame(format, frame);
  ASSERT_EQ(first_frame, 0);

  // Nothing to wait for before the first frame.
  std::vector<std::string> expected {"render", "FenceSync", "ClientWaitSync", "DeleteSync"};
  expected.insert(expected.end(), format.planes.size(), "cuMemcpy2DAsync");
  expected.emplace_back("cuEventRecord");
  EXPECT_EQ(api().log, expected);
  expect_copies(format, frame, api().cu_arrays);

  api().log.clear();
  api().copies.clear();
  ASSERT_EQ(run_frame(format, frame), 0);

  // GL renders again only after CUDA finished the previous copies.
  expected.insert(expected.begin(), "cuEventSynchronize");
  EXPECT_EQ(api().log, expected);
}

TEST_P(CudaGlDriverFormatTest, MappedFramesMapOnlyForTheCopies) {
  const auto &format = GetParam();
  gl::ctx.EXT_memory_object_fd = 0;
  ASSERT_EQ(create(format), 0);

  const auto frame = make_frame(format);
  api().log.clear();
  ASSERT_EQ(run_frame(format, frame), 0);

  std::vector<std::string> expected {"render", "cuGraphicsMapResources"};
  for (std::size_t plane = 0; plane < format.planes.size(); ++plane) {
    expected.emplace_back("cuGraphicsSubResourceGetMappedArray");
    expected.emplace_back("cuMemcpy2DAsync");
  }
  expected.emplace_back("cuGraphicsUnmapResources");
  EXPECT_EQ(api().log, expected);
  EXPECT_EQ(api().mapped_count, 0);
  expect_copies(format, frame, api().mapped_arrays);
}

INSTANTIATE_TEST_SUITE_P(StreamFormats, CudaGlDriverFormatTest, testing::ValuesIn(stream_formats()));

TEST_P(CudaGlDriverFailureTest, FailureFallsBackWithNothingLeftBehind) {
  const auto &[call, nth] = GetParam();

  for (const auto &format : stream_formats()) {
    SCOPED_TRACE(format.format);
    textures = {};
    planes = make_planes();
    registered.clear();
    api() = fake_api_t {};
    api().cuda = fake_cuda_functions();
    api().fail_at[call] = nth;

    EXPECT_EQ(create(format), 0);

    EXPECT_EQ(api().counts[call], nth) << "the failing call must run, and nothing may repeat it";
    expect_clean_fallback(format);
    expect_release_order();
  }
}

INSTANTIATE_TEST_SUITE_P(
  Calls,
  CudaGlDriverFailureTest,
  testing::Values(
    failing_call_t {"cuCtxGetDevice", 1},
    failing_call_t {"cuDeviceGetUuid", 1},
    failing_call_t {"vkCreateInstance", 1},
    failing_call_t {"vkCreateDevice", 1},
    failing_call_t {"load_get_memory_fd", 1},
    failing_call_t {"cuEventCreate", 1},
    failing_call_t {"vkCreateImage", 1},
    failing_call_t {"vkCreateImage", 2},
    failing_call_t {"vkAllocateMemory", 2},
    failing_call_t {"vkBindImageMemory", 2},
    failing_call_t {"vkGetMemoryFdKHR", 3},  // The second plane's fd for GL.
    failing_call_t {"vkGetMemoryFdKHR", 4},  // The second plane's fd for CUDA.
    failing_call_t {"ImportMemoryFdEXT", 2},
    failing_call_t {"TexStorageMem2DEXT", 2},
    failing_call_t {"cuImportExternalMemory", 2},
    failing_call_t {"cuExternalMemoryGetMappedMipmappedArray", 2},
    failing_call_t {"cuMipmappedArrayGetLevel", 2}
  )
);

TEST_F(CudaGlDriverTest, FailedSecondPlaneDropsTexturesBeforeTheirMemory) {
  api().fail_at["cuImportExternalMemory"] = 2;

  EXPECT_EQ(create(stream_formats()[0]), 0);

  // The first plane's memory was fully set up, so all of its release calls ran.
  for (const auto *call : {"cuMipmappedArrayDestroy", "cuDestroyExternalMemory", "DeleteMemoryObjectsEXT", "vkDestroyImage", "vkFreeMemory"}) {
    EXPECT_GE(std::ranges::count(api().log, std::string(call)), 1) << call;
    EXPECT_LT(api().first("DeleteTextures"), api().first(call)) << call;
  }
}

TEST_F(CudaGlDriverTest, GlImportFailureClosesBothDescriptors) {
  api().fail_at["ImportMemoryFdEXT"] = 1;

  EXPECT_EQ(create(stream_formats()[0]), 0);

  EXPECT_EQ(std::ranges::count(api().log, std::string("close")), 2);
  EXPECT_TRUE(api().open_fds.empty());
}

TEST_F(CudaGlDriverTest, GlStorageFailureClosesOnlyTheCudaDescriptor) {
  api().fail_at["TexStorageMem2DEXT"] = 1;

  EXPECT_EQ(create(stream_formats()[0]), 0);

  EXPECT_EQ(std::ranges::count(api().log, std::string("close")), 1);
  EXPECT_TRUE(api().open_fds.empty());
}

TEST_F(CudaGlDriverTest, StaleGlErrorDoesNotFailTheImports) {
  api().gl_errors.push_back(GL_INVALID_VALUE);

  EXPECT_EQ(create(stream_formats()[0]), 0);

  EXPECT_TRUE(planes->is_active());
  EXPECT_EQ(std::ranges::count(api().log, std::string("close")), 0);
}

TEST_F(CudaGlDriverTest, MissingCudaExternalMemoryUsesPlainTextures) {
  api().cuda.cuImportExternalMemory = nullptr;

  EXPECT_EQ(create(stream_formats()[0]), 0);

  EXPECT_FALSE(planes->is_active());
  EXPECT_FALSE(api().counts.contains("vkCreateInstance"));
  EXPECT_EQ(registered.size(), 2u);
}

TEST_F(CudaGlDriverTest, MissingCudaUuidUsesPlainTextures) {
  api().cuda.cuDeviceGetUuid = nullptr;

  EXPECT_EQ(create(stream_formats()[0]), 0);

  EXPECT_FALSE(planes->is_active());
  EXPECT_FALSE(api().counts.contains("vkCreateInstance"));
  EXPECT_EQ(registered.size(), 2u);
}

TEST_F(CudaGlDriverTest, NoVulkanDeviceMatchingCudaFallsBack) {
  api().uuid_matches = false;

  EXPECT_EQ(create(stream_formats()[0]), 0);

  EXPECT_FALSE(api().counts.contains("vkCreateDevice"));
  expect_clean_fallback(stream_formats()[0]);
}

TEST_F(CudaGlDriverTest, NoDeviceLocalMemoryTypeFallsBack) {
  api().memory_type_bits = 0b01;

  EXPECT_EQ(create(stream_formats()[0]), 0);

  EXPECT_FALSE(api().counts.contains("vkAllocateMemory"));
  expect_clean_fallback(stream_formats()[0]);
}

TEST_F(CudaGlDriverTest, RegistrationFailureIsReported) {
  gl::ctx.EXT_memory_object_fd = 0;
  register_fails = true;

  EXPECT_EQ(create(stream_formats()[0]), -1);
  EXPECT_EQ(registered.size(), 1u);
}

TEST_F(CudaGlDriverTest, HandoffUsesTheCopyEvent) {
  ASSERT_EQ(create(stream_formats()[0]), 0);

  api().expected_stream = &api().stream;
  EXPECT_EQ(planes->record_copies(&api().stream), 0);
  EXPECT_EQ(planes->wait_for_cuda(), 0);
  EXPECT_EQ(api().counts["cuEventRecord"], 1);
  EXPECT_EQ(api().counts["cuEventSynchronize"], 1);
}

TEST_F(CudaGlDriverTest, HandoffReportsCopyEventFailures) {
  api().fail_at["cuEventRecord"] = 1;
  api().fail_at["cuEventSynchronize"] = 1;
  ASSERT_EQ(create(stream_formats()[0]), 0);

  api().expected_stream = &api().stream;
  EXPECT_EQ(planes->record_copies(&api().stream), -1);
  EXPECT_EQ(planes->record_copies(&api().stream), 0);
  EXPECT_EQ(planes->wait_for_cuda(), -1);
}

TEST_F(CudaGlDriverTest, MappingFailureSkipsTheCopiesAndTheUnmap) {
  const auto format = stream_formats()[0];
  gl::ctx.EXT_memory_object_fd = 0;
  api().fail_at["cuGraphicsMapResources"] = 1;
  ASSERT_EQ(create(format), 0);

  EXPECT_EQ(run_frame(format, make_frame(format)), -1);

  EXPECT_TRUE(api().copies.empty());
  EXPECT_FALSE(api().counts.contains("cuGraphicsUnmapResources"));
}

TEST_F(CudaGlDriverTest, MappedArrayFailureStopsTheCopiesAndStillUnmaps) {
  const auto format = stream_formats()[0];
  gl::ctx.EXT_memory_object_fd = 0;
  api().fail_at["cuGraphicsSubResourceGetMappedArray"] = 2;
  ASSERT_EQ(create(format), 0);

  EXPECT_EQ(run_frame(format, make_frame(format)), -1);

  EXPECT_EQ(api().copies.size(), 1u);
  EXPECT_EQ(api().counts["cuGraphicsUnmapResources"], 1);
  EXPECT_EQ(api().mapped_count, 0);
}

TEST_F(CudaGlDriverTest, UnmapFailureIsReported) {
  const auto format = stream_formats()[0];
  gl::ctx.EXT_memory_object_fd = 0;
  api().fail_at["cuGraphicsUnmapResources"] = 1;
  ASSERT_EQ(create(format), 0);

  EXPECT_EQ(run_frame(format, make_frame(format)), -1);
}

TEST_F(CudaGlDriverTest, FailedCopyIsLoggedAndTheFrameContinues) {
  const auto format = stream_formats()[0];
  api().fail_at["cuMemcpy2DAsync"] = 1;
  ASSERT_EQ(create(format), 0);

  EXPECT_EQ(run_frame(format, make_frame(format)), 0);

  EXPECT_EQ(api().counts["cuMemcpy2DAsync"], 2);
  EXPECT_EQ(api().counts["cuEventRecord"], 1);
}

TEST_F(CudaGlDriverTest, GlFenceFailureSkipsTheCopiesAndReleasesNothingTwice) {
  const auto format = stream_formats()[0];
  api().fail_at["FenceSync"] = 1;
  ASSERT_EQ(create(format), 0);

  EXPECT_EQ(run_frame(format, make_frame(format)), -1);

  EXPECT_TRUE(api().copies.empty());
}

TEST_F(CudaGlDriverTest, PlaneTextureRegistration) {
  CUgraphicsResource resource = nullptr;

  EXPECT_EQ(cuda::register_plane_texture(api().cuda, &resource, 7), 0);
  EXPECT_EQ(resource, &api().resources[0]);
  EXPECT_EQ(api().registered_textures, std::vector<GLuint> {7});

  api().fail_at["cuGraphicsGLRegisterImage"] = 2;
  EXPECT_EQ(cuda::register_plane_texture(api().cuda, &resource, 8), -1);
}

namespace {
  /**
   * @brief CUDA error lookup behavior and the text that is safe to log.
   */
  struct cuda_error_lookup_case_t {
    tcuGetErrorName *lookup;  ///< Error name or description lookup with the shared CUDA signature.
    const char *expected;  ///< Text to log, or nullptr when the appropriate fallback is required.
  };
}  // namespace

TEST(CudaErrorLoggingTest, HandlesLookupFailuresAndNullTextIndependently) {
  const std::array cases {
    cuda_error_lookup_case_t {[](CUresult, const char **text) {
                                *text = "resolved";
                                return CUDA_SUCCESS;
                              },
                              "resolved"},
    cuda_error_lookup_case_t {[](CUresult, const char **) {
                                return cu_failed;
                              },
                              nullptr},
    cuda_error_lookup_case_t {[](CUresult, const char **text) {
                                *text = reinterpret_cast<const char *>(std::uintptr_t {1});
                                return cu_failed;
                              },
                              nullptr},
    cuda_error_lookup_case_t {[](CUresult, const char **text) {
                                *text = nullptr;
                                return CUDA_SUCCESS;
                              },
                              nullptr},
  };

  for (std::size_t name_case = 0; name_case < cases.size(); ++name_case) {
    for (std::size_t description_case = 0; description_case < cases.size(); ++description_case) {
      SCOPED_TRACE(testing::Message() << "name=" << name_case << ", description=" << description_case);
      api() = fake_api_t {};
      auto functions = fake_cuda_functions();
      functions.cuGetErrorName = cases[name_case].lookup;
      functions.cuGetErrorString = cases[description_case].lookup;
      functions.cuGraphicsGLRegisterImage = [](CUgraphicsResource *, GLuint, GLenum, unsigned int) {
        return static_cast<CUresult>(100);  // CUDA_ERROR_NO_DEVICE is not named in nv-codec-headers.
      };
      CUgraphicsResource resource = nullptr;
      logging::log_flush();
      std::ifstream log_file {"test_sunshine.log", std::ios::ate};
      ASSERT_TRUE(log_file);
      const auto log_position = log_file.tellg();

      EXPECT_EQ(cuda::register_plane_texture(functions, &resource, 7), -1);

      logging::log_flush();
      log_file.seekg(log_position);
      std::string line;
      ASSERT_TRUE(static_cast<bool>(std::getline(log_file, line)));
      const std::string expected = std::string {"Couldn't register plane texture: "} +
                                   (cases[name_case].expected ? cases[name_case].expected : "Unknown CUDA error") +
                                   " (100): " +
                                   (cases[description_case].expected ? cases[description_case].expected : "No error description available");
      EXPECT_NE(line.find(expected), std::string::npos) << line;
    }
  }
}

TEST(CudaErrorLoggingTest, SuccessfulCallsDoNotLookUpOrLogErrors) {
  api() = fake_api_t {};
  auto functions = fake_cuda_functions();
  functions.cuGetErrorName = functions.cuGetErrorString = [](CUresult, const char **) {
    ADD_FAILURE() << "Successful CUDA calls must not look up errors";
    return cu_failed;
  };
  CUgraphicsResource resource = nullptr;
  logging::log_flush();
  std::ifstream log_file {"test_sunshine.log", std::ios::ate};
  ASSERT_TRUE(log_file);
  const auto log_position = log_file.tellg();

  EXPECT_EQ(cuda::register_plane_texture(functions, &resource, 7), 0);

  logging::log_flush();
  log_file.seekg(log_position);
  EXPECT_EQ(log_file.peek(), std::ifstream::traits_type::eof());
  EXPECT_EQ(resource, &api().resources[0]);
}

TEST_F(CudaGlDriverTest, GlCommandsWaitDeletesItsFence) {
  EXPECT_EQ(cuda::wait_for_gl_commands(), 0);

  EXPECT_EQ(api().log, (std::vector<std::string> {"FenceSync", "ClientWaitSync", "DeleteSync"}));
  EXPECT_TRUE(api().live.empty());
}

TEST_F(CudaGlDriverTest, GlCommandsWaitReportsFailures) {
  api().fence_status = GL_TIMEOUT_EXPIRED;
  EXPECT_EQ(cuda::wait_for_gl_commands(), -1);
  EXPECT_TRUE(api().live.empty());

  api().fail_at["FenceSync"] = 2;
  EXPECT_EQ(cuda::wait_for_gl_commands(), -1);
  EXPECT_TRUE(api().live.empty());
}
    #endif  // SUNSHINE_BUILD_VULKAN

namespace {
  /**
   * @brief What the fake CUDA driver used by the `cuda::init()` tests has been asked to do.
   *
   * The entries of a CUDA function table are plain function pointers, so the fakes share this state.
   */
  struct fake_init_driver_t {
    std::atomic<int> table_loads;  ///< Times a table was loaded.
    std::atomic<int> cu_inits;  ///< Times `cuInit` was called.
    std::atomic<int> failing_cu_inits;  ///< How many of the next `cuInit` calls fail.
    std::atomic<int> error_name_lookups;  ///< Times `cuGetErrorName` was called.
    std::atomic<int> error_description_lookups;  ///< Times `cuGetErrorString` was called.
    std::atomic<CUresult> looked_up_error;  ///< Result the last lookup was asked about.
  };

  constexpr auto init_failure = CUDA_ERROR_UNKNOWN;  ///< What a failing fake `cuInit` returns.

  /**
   * @brief The fake driver's state, reset for every test.
   *
   * @return The state.
   */
  fake_init_driver_t &init_driver() {
    static fake_init_driver_t driver;
    return driver;
  }

  /**
   * @brief Forget everything the fake driver recorded.
   */
  void reset_init_driver() {
    auto &driver = init_driver();
    driver.table_loads = 0;
    driver.cu_inits = 0;
    driver.failing_cu_inits = 0;
    driver.error_name_lookups = 0;
    driver.error_description_lookups = 0;
    driver.looked_up_error = CUDA_SUCCESS;
  }

  /**
   * @brief Fill a table with the fake driver's functions.
   *
   * @param table Table to fill.
   */
  void fill_fake_table(CudaFunctions &table) {
    table.cuInit = [](unsigned int) {
      auto &driver = init_driver();
      ++driver.cu_inits;
      if (driver.failing_cu_inits > 0) {
        --driver.failing_cu_inits;
        return init_failure;
      }
      return CUDA_SUCCESS;
    };
    table.cuGetErrorName = [](CUresult result, const char **text) {
      ++init_driver().error_name_lookups;
      init_driver().looked_up_error = result;
      *text = "fake";
      return CUDA_SUCCESS;
    };
    table.cuGetErrorString = [](CUresult result, const char **text) {
      ++init_driver().error_description_lookups;
      init_driver().looked_up_error = result;
      *text = "failure";
      return CUDA_SUCCESS;
    };
  }

  /**
   * @brief Load a table the way `cuda_load_functions` does, without needing the CUDA driver.
   *
   * Like the real loader, it frees the table it is given and starts a new one.
   *
   * @param table Table to load.
   * @return 0.
   */
  int load_fake_table(CudaFunctions **table) {
    ++init_driver().table_loads;
    cuda_free_functions(table);
    *table = static_cast<CudaFunctions *>(std::calloc(1, sizeof(CudaFunctions)));
    fill_fake_table(**table);
    return 0;
  }

  /**
   * @brief Stand-in for a loader on a machine where the CUDA driver cannot be loaded.
   *
   * @return -1.
   */
  int failing_load_table(CudaFunctions **) {
    ++init_driver().table_loads;
    return -1;
  }

  /**
   * @brief Load a table slowly enough for any caller not waiting its turn to reach the loader too.
   *
   * The first load waits until a second load starts, or 200 ms at most. Every load hands out the same
   * table and frees nothing, so callers loading at the same time are counted rather than crashing.
   *
   * @param table Table to load.
   * @return 0.
   */
  int load_fake_table_slowly(CudaFunctions **table) {
    using namespace std::chrono_literals;
    static CudaFunctions shared_table = [] {
      CudaFunctions functions {};
      fill_fake_table(functions);
      return functions;
    }();

    if (++init_driver().table_loads == 1) {
      const auto deadline = std::chrono::steady_clock::now() + 200ms;
      while (init_driver().table_loads < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
      }
    }

    *table = &shared_table;
    return 0;
  }
}  // namespace

TEST(CudaInitTest, LoadsTheFunctionTableOnlyOnce) {
  CudaFunctions *table = nullptr;
  auto free_table = util::fail_guard([&table] {
    cuda_free_functions(&table);
  });
  reset_init_driver();

  ASSERT_EQ(cuda::init(&table, load_fake_table), 0);
  // Every new stream calls cuda::init() again, while the streams already running keep calling
  // through the table. Loading it again would empty it under them.
  ASSERT_EQ(cuda::init(&table, load_fake_table), 0);

  EXPECT_EQ(init_driver().table_loads.load(), 1);
}

TEST(CudaInitTest, RetriesLoadingAfterAFailure) {
  CudaFunctions *table = nullptr;
  auto free_table = util::fail_guard([&table] {
    cuda_free_functions(&table);
  });
  reset_init_driver();

  EXPECT_NE(cuda::init(&table, failing_load_table), 0);
  EXPECT_EQ(cuda::init(&table, load_fake_table), 0);

  EXPECT_EQ(init_driver().table_loads.load(), 2);
}

TEST(CudaInitTest, ReportsACuInitFailureThroughTheTableAndRetriesItWithTheSameTable) {
  CudaFunctions *table = nullptr;
  auto free_table = util::fail_guard([&table] {
    cuda_free_functions(&table);
  });
  reset_init_driver();
  init_driver().failing_cu_inits = 1;

  EXPECT_NE(cuda::init(&table, load_fake_table), 0);
  EXPECT_EQ(init_driver().error_name_lookups.load(), 1);
  EXPECT_EQ(init_driver().error_description_lookups.load(), 1);
  EXPECT_EQ(init_driver().looked_up_error.load(), init_failure);

  const auto *loaded_table = table;
  EXPECT_EQ(cuda::init(&table, load_fake_table), 0);

  EXPECT_EQ(table, loaded_table);
  EXPECT_EQ(init_driver().table_loads.load(), 1);
  EXPECT_EQ(init_driver().cu_inits.load(), 2);
}

TEST(CudaInitTest, CallersStartingTogetherLoadTheTableOnce) {
  constexpr int callers = 8;
  CudaFunctions *table = nullptr;
  reset_init_driver();

  // Streams that start at the same time all call cuda::init() at once.
  std::array<int, callers> results {};
  std::latch start {callers};
  std::vector<std::jthread> threads;
  for (auto &result : results) {
    threads.emplace_back([&result, &start, &table] {
      start.arrive_and_wait();
      result = cuda::init(&table, load_fake_table_slowly);
    });
  }
  threads.clear();

  EXPECT_EQ(results, (std::array<int, callers> {}));
  EXPECT_EQ(init_driver().table_loads.load(), 1);
  EXPECT_EQ(init_driver().cu_inits.load(), callers);
}
  #endif  // SUNSHINE_BUILD_CUDA

#endif  // __linux__
