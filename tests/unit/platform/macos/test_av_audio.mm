/**
 * @file tests/unit/platform/macos/test_av_audio.mm
 * @brief Unit tests for src/platform/macos/av_audio.*.
 */

// Only compile these tests on macOS
#ifdef __APPLE__

  // test includes
  #include "../../../tests_common.h"

  // platform includes
  #import <AVFoundation/AVFoundation.h>
  #import <CoreAudio/CATapDescription.h>
  #import <CoreAudio/CoreAudio.h>
  #import <Foundation/Foundation.h>
  #import <objc/runtime.h>

  // local includes
  #import <src/platform/macos/av_audio.h>
  #include <src/platform/macos/misc.h>

namespace platf::tests {
  bool keep_default_supported();
  int capture_sink_status(const std::string &sink);
}  // namespace platf::tests

namespace {
  bool permission_probe_unmuted = false;  ///< Whether the test tap leaves host audio audible.
  int permission_probe_result = 0;  ///< Simulated Core Audio tap setup result.

  /**
   * @brief Replace native tap setup while testing the startup permission probe.
   *
   * @param audio Temporary capture object created by the permission request.
   * @param selector Objective-C selector for the intercepted setup call.
   * @param sample_rate Requested capture sample rate.
   * @param frame_size Requested audio frame size.
   * @param channels Requested channel count.
   * @return Simulated tap setup result.
   */
  int test_permission_tap_setup(AVAudio *audio, [[maybe_unused]] SEL selector, UInt32 sample_rate, UInt32 frame_size, UInt8 channels) {
    permission_probe_unmuted = audio.hostAudioEnabled == YES;
    EXPECT_EQ(sample_rate, 48000U);
    EXPECT_EQ(frame_size, 512U);
    EXPECT_EQ(channels, 2U);
    return permission_probe_result;
  }
}  // namespace

/**
 * @brief Test parameters for processSystemAudioIOProc tests.
 * Contains various audio configuration parameters to test different scenarios.
 */
struct ProcessSystemAudioIOProcTestParams {
  UInt32 frameCount;  ///< Number of audio frames to process
  UInt32 channels;  ///< Number of audio channels (1=mono, 2=stereo)
  UInt32 sampleRate;  ///< Sample rate in Hz
  bool useNilInput;  ///< Whether to test with nil input data
  const char *testName;  ///< Descriptive name for the test case
};

/**
 * @brief Test suite for AVAudio class functionality.
 * Parameterized test class for testing Core Audio system tap functionality.
 */
class AVAudioTest: public PlatformTestSuite, public ::testing::WithParamInterface<ProcessSystemAudioIOProcTestParams> {};

/**
 * @brief Test that findMicrophone handles nil input gracefully.
 * Verifies the method returns nil when passed a nil microphone name.
 */
TEST_F(AVAudioTest, FindMicrophoneWithNilReturnsNil) {
  @try {
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wnonnull"
    AVCaptureDevice *device = [AVAudio findMicrophone:nil];
  #pragma clang diagnostic pop
    EXPECT_EQ(device, nil);
  }
  @catch (NSException *exception) {
    FAIL() << "Caught NSException: " << ([exception.reason UTF8String] ?: "unknown reason");
  }
}

/**
 * @brief Test that findMicrophone handles empty string input gracefully.
 * Verifies the method returns nil when passed an empty microphone name.
 */
TEST_F(AVAudioTest, FindMicrophoneWithEmptyStringReturnsNil) {
  @try {
    AVCaptureDevice *device = [AVAudio findMicrophone:@""];
    EXPECT_EQ(device, nil);  // Should return nil for empty string
  }
  @catch (NSException *exception) {
    FAIL() << "Caught NSException: " << ([exception.reason UTF8String] ?: "unknown reason");
  }
}

/**
 * @brief Test that existing microphone permission is accepted without another request.
 */
TEST_F(AVAudioTest, AuthorizedMicrophonePermissionDoesNotRequestAccess) {
  bool request_called = false;

  EXPECT_TRUE(platf::request_microphone_permission(AVAuthorizationStatusAuthorized, [&](platf::microphone_permission_callback_t callback) {
    request_called = true;
    callback(false);
  }));
  EXPECT_FALSE(request_called);
}

/**
 * @brief Test that denied, restricted, and unknown microphone states are rejected without another request.
 */
TEST_F(AVAudioTest, UnavailableMicrophonePermissionDoesNotRequestAccess) {
  bool request_called = false;
  const auto request_access = [&](platf::microphone_permission_callback_t callback) {
    request_called = true;
    callback(true);
  };

  EXPECT_FALSE(platf::request_microphone_permission(AVAuthorizationStatusDenied, request_access));
  EXPECT_FALSE(platf::request_microphone_permission(AVAuthorizationStatusRestricted, request_access));
  EXPECT_FALSE(platf::request_microphone_permission(static_cast<AVAuthorizationStatus>(-1), request_access));
  EXPECT_FALSE(request_called);
}

/**
 * @brief Test that an undetermined microphone permission returns the user's decision.
 */
TEST_F(AVAudioTest, UndeterminedMicrophonePermissionRequestsAccess) {
  bool request_called = false;

  EXPECT_TRUE(platf::request_microphone_permission(AVAuthorizationStatusNotDetermined, [&](platf::microphone_permission_callback_t callback) {
    request_called = true;
    callback(true);
  }));
  EXPECT_TRUE(request_called);

  request_called = false;
  EXPECT_FALSE(platf::request_microphone_permission(AVAuthorizationStatusNotDetermined, [&](platf::microphone_permission_callback_t callback) {
    request_called = true;
    callback(false);
  }));
  EXPECT_TRUE(request_called);
}

/**
 * @brief Verify the startup probe never mutes host audio and reports tap failure.
 */
TEST(MacosAudioPermissionTest, SystemAudioPermissionProbeUsesUnmutedTemporaryTap) {
  Method method = class_getInstanceMethod([AVAudio class], @selector(setupSystemTap:frameSize:channels:));
  ASSERT_NE(method, nullptr);
  IMP original = method_setImplementation(method, reinterpret_cast<IMP>(test_permission_tap_setup));

  permission_probe_result = 0;
  permission_probe_unmuted = false;
  EXPECT_TRUE(platf::request_system_audio_permission());
  EXPECT_TRUE(permission_probe_unmuted);

  permission_probe_result = -1;
  permission_probe_unmuted = false;
  EXPECT_FALSE(platf::request_system_audio_permission());
  EXPECT_TRUE(permission_probe_unmuted);

  method_setImplementation(method, original);
}

/**
 * @brief Test that setupMicrophone handles nil device input properly.
 * Verifies the method returns an error code when passed a nil device.
 */
TEST_F(AVAudioTest, SetupMicrophoneWithNilDeviceReturnsError) {
  @try {
    AVAudio *avAudio = [[AVAudio alloc] init];
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wnonnull"
    int result = [avAudio setupMicrophone:nil sampleRate:48000 frameSize:512 channels:2];
  #pragma clang diagnostic pop
    [avAudio release];
    EXPECT_EQ(result, -1);  // Should fail with nil device
  }
  @catch (NSException *exception) {
    FAIL() << "Caught NSException: " << ([exception.reason UTF8String] ?: "unknown reason");
  }
}

/**
 * @brief Test basic AVAudio object lifecycle.
 * Verifies that AVAudio objects can be created and destroyed without issues.
 */
TEST_F(AVAudioTest, ObjectLifecycle) {
  @try {
    AVAudio *avAudio = [[AVAudio alloc] init];
    EXPECT_NE(avAudio, nil);  // Should create successfully
    [avAudio release];  // Should not crash
  }
  @catch (NSException *exception) {
    FAIL() << "Caught NSException: " << ([exception.reason UTF8String] ?: "unknown reason");
  }
}

/**
 * @brief Test that multiple AVAudio objects can coexist.
 * Verifies that multiple instances can be created simultaneously.
 */
TEST_F(AVAudioTest, MultipleObjectsCoexist) {
  @try {
    AVAudio *avAudio1 = [[AVAudio alloc] init];
    AVAudio *avAudio2 = [[AVAudio alloc] init];

    EXPECT_NE(avAudio1, nil);
    EXPECT_NE(avAudio2, nil);
    EXPECT_NE(avAudio1, avAudio2);  // Should be different objects

    [avAudio1 release];
    [avAudio2 release];
  }
  @catch (NSException *exception) {
    FAIL() << "Caught NSException: " << ([exception.reason UTF8String] ?: "unknown reason");
  }
}

/**
 * @brief Test audio buffer initialization with various channel configurations.
 * Verifies that the audio buffer can be initialized with different channel counts.
 */
TEST_F(AVAudioTest, InitializeAudioBuffer) {
  @try {
    AVAudio *avAudio = [[AVAudio alloc] init];
    uint32_t avail = 0;

    // Test with various channel counts
    [avAudio initializeAudioBuffer:1];  // Mono
    EXPECT_NE(avAudio->audioSemaphore, nullptr);
    TPCircularBufferHead(&avAudio->audioSampleBuffer, &avail);
    EXPECT_GE(avail, 5760);
    [avAudio cleanupAudioBuffer];

    [avAudio initializeAudioBuffer:2];  // Stereo
    EXPECT_NE(avAudio->audioSemaphore, nullptr);
    TPCircularBufferHead(&avAudio->audioSampleBuffer, &avail);
    EXPECT_GE(avail, 11520);
    [avAudio cleanupAudioBuffer];

    [avAudio initializeAudioBuffer:8];  // 7.1 Surround
    EXPECT_NE(avAudio->audioSemaphore, nullptr);
    TPCircularBufferHead(&avAudio->audioSampleBuffer, &avail);
    EXPECT_GE(avail, 46080);
    [avAudio cleanupAudioBuffer];

    [avAudio release];
  }
  @catch (NSException *exception) {
    FAIL() << "Caught NSException: " << ([exception.reason UTF8String] ?: "unknown reason");
  }
}

/**
 * @brief Test audio buffer cleanup functionality.
 * Verifies that cleanup works correctly even with uninitialized buffers.
 */
TEST_F(AVAudioTest, CleanupUninitializedBuffer) {
  @try {
    AVAudio *avAudio = [[AVAudio alloc] init];

    // Should not crash even if buffer was never initialized
    [avAudio cleanupAudioBuffer];

    // Initialize then cleanup
    [avAudio initializeAudioBuffer:2];
    EXPECT_NE(avAudio->audioSemaphore, nullptr);
    [avAudio cleanupAudioBuffer];
    EXPECT_EQ(avAudio->audioSemaphore, nullptr);

    [avAudio release];
  }
  @catch (NSException *exception) {
    FAIL() << "Caught NSException: " << ([exception.reason UTF8String] ?: "unknown reason");
  }
}

/**
 * @brief Test audio converter complex input callback with valid data.
 * Verifies that the audio converter callback properly processes valid audio data.
 */
TEST_F(AVAudioTest, AudioConverterComplexInputProc) {
  @try {
    AVAudio *avAudio = [[AVAudio alloc] init];

    // Create test input data
    UInt32 frameCount = 256;
    UInt32 channels = 2;
    float *testData = (float *) calloc(frameCount * channels, sizeof(float));

    // Fill with deterministic ramp data (channel-encoded constants)
    for (UInt32 frame = 0; frame < frameCount; frame++) {
      for (UInt32 channel = 0; channel < channels; channel++) {
        testData[frame * channels + channel] = channel + frame * 0.001f;
      }
    }

    AudioConverterInputData inputInfo = {};
    inputInfo.inputData = testData;
    inputInfo.inputFrames = frameCount;
    inputInfo.framesProvided = 0;
    inputInfo.deviceChannels = channels;
    inputInfo.avAudio = avAudio;

    // Test the method
    UInt32 requestedPackets = 128;
    AudioBufferList bufferList = {};
    // Use a dummy AudioConverterRef (can be null for our test since our implementation doesn't use it)
    AudioConverterRef dummyConverter = nullptr;
    OSStatus result = platf::audioConverterComplexInputProc(dummyConverter, &requestedPackets, &bufferList, nullptr, &inputInfo);

    EXPECT_EQ(result, noErr);
    EXPECT_EQ(requestedPackets, 128);  // Should provide requested frames
    EXPECT_EQ(inputInfo.framesProvided, 128);  // Should update frames provided

    free(testData);
    [avAudio release];
  }
  @catch (NSException *exception) {
    FAIL() << "Caught NSException: " << ([exception.reason UTF8String] ?: "unknown reason");
  }
}

/**
 * @brief Test audio converter callback when no more data is available.
 * Verifies that the callback handles end-of-data scenarios correctly.
 */
TEST_F(AVAudioTest, AudioConverterInputProcNoMoreData) {
  @try {
    AVAudio *avAudio = [[AVAudio alloc] init];

    UInt32 frameCount = 256;
    UInt32 channels = 2;
    float *testData = (float *) calloc(frameCount * channels, sizeof(float));

    AudioConverterInputData inputInfo = {};
    inputInfo.inputData = testData;
    inputInfo.inputFrames = frameCount;
    inputInfo.framesProvided = frameCount;  // Already provided all frames
    inputInfo.deviceChannels = channels;
    inputInfo.avAudio = avAudio;

    UInt32 requestedPackets = 128;
    AudioBufferList bufferList = {};
    // Use a dummy AudioConverterRef (can be null for our test since our implementation doesn't use it)
    AudioConverterRef dummyConverter = nullptr;
    OSStatus result = platf::audioConverterComplexInputProc(dummyConverter, &requestedPackets, &bufferList, nullptr, &inputInfo);

    EXPECT_EQ(result, noErr);
    EXPECT_EQ(requestedPackets, 0);  // Should return 0 packets when no more data

    free(testData);
    [avAudio release];
  }
  @catch (NSException *exception) {
    FAIL() << "Caught NSException: " << ([exception.reason UTF8String] ?: "unknown reason");
  }
}

/**
 * @brief Test that audio buffer cleanup can be called multiple times safely.
 * Verifies that repeated cleanup calls don't cause crashes or issues.
 */
TEST_F(AVAudioTest, CleanupAudioBufferMultipleTimes) {
  @try {
    AVAudio *avAudio = [[AVAudio alloc] init];

    [avAudio initializeAudioBuffer:2];
    EXPECT_NE(avAudio->audioSemaphore, nullptr);

    // Multiple cleanup calls should not crash
    [avAudio cleanupAudioBuffer];
    EXPECT_EQ(avAudio->audioSemaphore, nullptr);

    [avAudio cleanupAudioBuffer];  // Second call should be safe
    [avAudio cleanupAudioBuffer];  // Third call should be safe

    [avAudio release];
  }
  @catch (NSException *exception) {
    FAIL() << "Caught NSException: " << ([exception.reason UTF8String] ?: "unknown reason");
  }
}

/**
 * @brief Test buffer management with edge case channel configurations.
 * Verifies that buffer management works with minimum and maximum channel counts.
 */
TEST_F(AVAudioTest, BufferManagementEdgeCases) {
  @try {
    AVAudio *avAudio = [[AVAudio alloc] init];

    // Test with minimum reasonable channel count (1 channel)
    [avAudio initializeAudioBuffer:1];
    EXPECT_NE(avAudio->audioSemaphore, nullptr);
    [avAudio cleanupAudioBuffer];

    // Test with very high channel count
    [avAudio initializeAudioBuffer:32];
    EXPECT_NE(avAudio->audioSemaphore, nullptr);
    [avAudio cleanupAudioBuffer];

    [avAudio release];
  }
  @catch (NSException *exception) {
    FAIL() << "Caught NSException: " << ([exception.reason UTF8String] ?: "unknown reason");
  }
}

// Type alias for parameterized audio processing tests
using ProcessSystemAudioIOProcTest = AVAudioTest;

// Test parameters - representative configurations to cover a range of scenarios
// Channels: 1 (mono), 2 (stereo), 6 (5.1), 8 (7.1)
// Sample rates: 48000 (common), 44100 (legacy), 192000 (edge)
// Frame counts: 64 (small), 256 (typical), 1024 (large)
INSTANTIATE_TEST_SUITE_P(
  AVAudioTest,
  ProcessSystemAudioIOProcTest,
  ::testing::Values(
    // Representative channel configurations at common sample rate
    ProcessSystemAudioIOProcTestParams {256, 1, 48000, false, "Mono48kHz"},
    ProcessSystemAudioIOProcTestParams {256, 2, 48000, false, "Stereo48kHz"},
    ProcessSystemAudioIOProcTestParams {256, 6, 48000, false, "Surround51_48kHz"},
    ProcessSystemAudioIOProcTestParams {256, 8, 48000, false, "Surround71_48kHz"},

    // Frame count variations (small, typical, large)
    ProcessSystemAudioIOProcTestParams {64, 2, 48000, false, "SmallFrameCount"},
    ProcessSystemAudioIOProcTestParams {1024, 2, 48000, false, "LargeFrameCount"},

    // Sample rate edge cases
    ProcessSystemAudioIOProcTestParams {256, 2, 44100, false, "LegacySampleRate44kHz"},
    ProcessSystemAudioIOProcTestParams {256, 2, 192000, false, "HighSampleRate192kHz"},

    // Edge case: nil input handling
    ProcessSystemAudioIOProcTestParams {256, 2, 48000, true, "NilInputHandling"},

    // Combined edge case: max channels + large frames
    ProcessSystemAudioIOProcTestParams {1024, 8, 48000, false, "MaxChannelsLargeFrames"}
  ),
  [](const ::testing::TestParamInfo<ProcessSystemAudioIOProcTestParams> &info) {
    return std::string(info.param.testName);
  }
);

TEST_P(ProcessSystemAudioIOProcTest, ProcessAudioInput) {
  ProcessSystemAudioIOProcTestParams params = GetParam();

  @try {
    AVAudio *avAudio = [[AVAudio alloc] init];

    // Use the new buffer initialization method instead of manual setup
    [avAudio initializeAudioBuffer:params.channels];

    // Create timestamps
    AudioTimeStamp timeStamp = {};
    timeStamp.mFlags = kAudioTimeStampSampleTimeValid;
    timeStamp.mSampleTime = 0;

    AudioBufferList *inputBufferList = nullptr;
    float *testInputData = nullptr;
    UInt32 inputDataSize = 0;

    // Only create input data if not testing nil input
    if (!params.useNilInput) {
      inputDataSize = params.frameCount * params.channels * sizeof(float);
      testInputData = (float *) calloc(params.frameCount * params.channels, sizeof(float));

      // Fill with deterministic ramp data (channel-encoded constants)
      // This is faster than sine waves and provides channel separation + frame ordering
      for (UInt32 frame = 0; frame < params.frameCount; frame++) {
        for (UInt32 channel = 0; channel < params.channels; channel++) {
          testInputData[frame * params.channels + channel] = channel + frame * 0.001f;
        }
      }

      // Create AudioBufferList
      inputBufferList = (AudioBufferList *) malloc(sizeof(AudioBufferList));
      inputBufferList->mNumberBuffers = 1;
      inputBufferList->mBuffers[0].mNumberChannels = params.channels;
      inputBufferList->mBuffers[0].mDataByteSize = inputDataSize;
      inputBufferList->mBuffers[0].mData = testInputData;
    }

    // Get initial buffer state
    uint32_t initialAvailableBytes = 0;
    TPCircularBufferTail(&avAudio->audioSampleBuffer, &initialAvailableBytes);

    // Create IOProc data structure for the C++ function
    AVAudioIOProcData procData = {};
    procData.avAudio = avAudio;
    procData.clientRequestedChannels = params.channels;
    procData.clientRequestedFrameSize = params.frameCount;
    procData.clientRequestedSampleRate = params.sampleRate;
    procData.aggregateDeviceChannels = params.channels;  // For simplicity in tests
    procData.aggregateDeviceSampleRate = params.sampleRate;
    procData.audioConverter = nullptr;  // No conversion needed for most tests

    // Create a dummy output buffer (not used in our implementation but required by signature)
    AudioBufferList dummyOutputBufferList = {};

    // Test the systemAudioIOProcWrapper function
    OSStatus result = platf::systemAudioIOProc(0,  // device ID (not used in our logic)
                                               &timeStamp,
                                               inputBufferList,
                                               &timeStamp,
                                               &dummyOutputBufferList,
                                               &timeStamp,
                                               &procData);

    // Verify the method returns success
    EXPECT_EQ(result, noErr);

    if (!params.useNilInput) {
      // Verify data was written to the circular buffer
      uint32_t finalAvailableBytes = 0;
      void *bufferData = TPCircularBufferTail(&avAudio->audioSampleBuffer, &finalAvailableBytes);
      EXPECT_GT(finalAvailableBytes, initialAvailableBytes);  // Should have more data than before
      EXPECT_GT(finalAvailableBytes, 0);  // Should have data in buffer

      // Verify we wrote the expected amount of data (input size for direct passthrough)
      EXPECT_EQ(finalAvailableBytes, inputDataSize);

      // Verify the actual audio data matches what we put in (first few samples)
      // Limit validation to min(8, channels * 2) samples to keep test efficient
      UInt32 samplesToTest = std::min(8U, params.channels * 2);
      if (bufferData && finalAvailableBytes >= sizeof(float) * samplesToTest) {
        float *outputSamples = (float *) bufferData;
        for (UInt32 i = 0; i < samplesToTest; i++) {
          EXPECT_FLOAT_EQ(outputSamples[i], testInputData[i]) << "Sample " << i << " mismatch";
        }
      }
    }

    // Cleanup
    if (testInputData) {
      free(testInputData);
    }
    if (inputBufferList) {
      free(inputBufferList);
    }
    [avAudio cleanupAudioBuffer];
    [avAudio release];
  }
  @catch (NSException *exception) {
    FAIL() << "Caught NSException: " << ([exception.reason UTF8String] ?: "unknown reason");
  }
}

/**
 * @brief macOS capture never touches the default output, with or without keep-default.
 */
TEST(MacosAudioControlTest, KeepDefaultModeCapturesWithoutSwapping) {
  EXPECT_TRUE(platf::tests::keep_default_supported());
  EXPECT_EQ(platf::tests::capture_sink_status("any-sink"), 0);
}

#endif  // __APPLE__
