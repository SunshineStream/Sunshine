/**
 * @file src/platform/macos/sc_capture.h
 * @brief Declarations for ScreenCaptureKit-based display capture on macOS.
 */
#pragma once

// platform includes
#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

/**
 * @brief ScreenCaptureKit display capture controller used by the macOS backend.
 *
 * Frames are delivered by an SCStream and latched into a single latest-wins slot; the
 * capture loop consumes the newest frame and the encoder duplicates the previous frame
 * when the screen is static. Delivery reliability depends on configuration discipline
 * (matching OBS's): `minimumFrameInterval` must sit ~10% below the target interval or
 * frames get suppressed by clock beating, the pipeline must hold well fewer sample
 * buffers at once than `queueDepth` (capped at 8 by the API) or WindowServer stalls
 * delivery until one is returned, and frames must never be filtered by SCFrameStatus.
 */
API_AVAILABLE(macos(14.0))
@interface SCCapture: NSObject <SCStreamDelegate, SCStreamOutput>

/**
 * @def kMaxDisplays
 * @brief Maximum number of displays queried from CoreGraphics.
 */
#define kMaxDisplays 32

/**
 * @brief Display ID property.
 */
@property (nonatomic, assign) CGDirectDisplayID displayID;
/**
 * @brief Capture frame rate property.
 */
@property (nonatomic, assign) int frameRate;
/**
 * @brief Output pixel format property.
 */
@property (nonatomic, assign) OSType pixelFormat;
/**
 * @brief Output frame width property.
 */
@property (nonatomic, assign) int frameWidth;
/**
 * @brief Output frame height property.
 */
@property (nonatomic, assign) int frameHeight;

/**
 * @brief Content filter selecting the captured display.
 */
@property (nonatomic, strong) SCContentFilter *contentFilter;
/**
 * @brief Stream configuration used for the capture stream.
 */
@property (nonatomic, strong) SCStreamConfiguration *streamConfiguration;
/**
 * @brief Active capture stream delivering frames, or nil when stopped.
 */
@property (nonatomic, strong) SCStream *stream;
/**
 * @brief Cached shareable content used for display lookup.
 */
@property (nonatomic, strong) SCShareableContent *shareableContent;

/**
 * @brief Semaphore signalled when capture stops.
 */
@property (nonatomic, assign) dispatch_semaphore_t captureSignal;
/**
 * @brief Semaphore signalled when a new frame is available.
 */
@property (nonatomic, assign) dispatch_semaphore_t frameSignal;
/**
 * @brief Whether the capture is shutting down.
 */
@property (nonatomic, assign) BOOL stopping;
/**
 * @brief Whether the stream stopped on its own since capture started.
 */
@property (nonatomic, assign) BOOL streamFailed;
/**
 * @brief Latest captured sample buffer, replaced as newer frames arrive.
 */
@property (nonatomic, assign) CMSampleBufferRef latestSampleBuffer;

/**
 * @brief Check whether ScreenCaptureKit screenshot capture is available on this system.
 *
 * @return `YES` when the running macOS version supports the capture backend.
 */
+ (BOOL)isAvailable;
/**
 * @brief Enumerate active displays.
 *
 * @return Array of dictionaries describing each active display.
 */
+ (NSArray<NSDictionary *> *)displayNames;
/**
 * @brief Look up the localized name of a display.
 *
 * @param displayID Display to look up.
 * @return Localized display name, or nil when unknown.
 */
+ (NSString *)getDisplayName:(CGDirectDisplayID)displayID;

/**
 * @brief Initialize capture for a display.
 *
 * @param displayID Display to capture.
 * @param frameRate Requested capture frame rate.
 * @return Initialized instance, or nil on failure.
 */
- (instancetype)initWithDisplay:(CGDirectDisplayID)displayID
                      frameRate:(int)frameRate;

/**
 * @brief Set the capture output dimensions.
 *
 * @param frameWidth Output frame width in pixels.
 * @param frameHeight Output frame height in pixels.
 */
- (void)setFrameWidth:(int)frameWidth frameHeight:(int)frameHeight;
/**
 * @brief Create, configure, and start the capture stream.
 *
 * When a 10-bit capture format is rejected by the runtime, capture falls back to
 * 8-bit and the encoder continues the negotiated session with 8-bit-sourced frames.
 *
 * @return Semaphore signalled when capture stops, or nil on failure.
 */
- (dispatch_semaphore_t)captureVideo;
/**
 * @brief Take ownership of the most recent captured frame.
 *
 * @return Retained sample buffer, or NULL when no new frame is available.
 */
- (CMSampleBufferRef)copyLatestSampleBuffer;
/**
 * @brief Stop capture and release capture state.
 */
- (void)stopCapture;

@end
