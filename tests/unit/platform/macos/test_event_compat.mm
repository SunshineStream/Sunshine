/**
 * @file tests/unit/platform/macos/test_event_compat.mm
 * @brief Tests for macOS AppKit event compatibility fixes.
 */

#ifdef __APPLE__
  // platform includes
  #import <AppKit/AppKit.h>

  // test includes
  #include "../../../tests_common.h"

  // local includes
  #include "src/platform/macos/misc.h"

TEST(MacosEventCompatTest, IdempotentInstallation) {
  EXPECT_NO_THROW(platf::install_event_compatibility_fixes());
  EXPECT_NO_THROW(platf::install_event_compatibility_fixes());
}

TEST(MacosEventCompatTest, NonMouseEventClickCountReturnsFallbackWithoutThrowing) {
  platf::install_event_compatibility_fixes();

  @autoreleasepool {
    NSEvent *kit_event = [NSEvent otherEventWithType:NSEventTypeAppKitDefined
                                            location:NSMakePoint(0, 0)
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:2
                                               data1:0
                                               data2:0];

    NSInteger click_count = 0;
    EXPECT_NO_THROW({
      click_count = [kit_event clickCount];
    });
    EXPECT_EQ(click_count, 1);

    NSEvent *system_event = [NSEvent otherEventWithType:NSEventTypeSystemDefined
                                               location:NSMakePoint(0, 0)
                                          modifierFlags:0
                                              timestamp:0
                                           windowNumber:0
                                                context:nil
                                                subtype:0
                                                  data1:0
                                                  data2:0];

    NSInteger system_click_count = 0;
    EXPECT_NO_THROW({
      system_click_count = [system_event clickCount];
    });
    EXPECT_EQ(system_click_count, 1);
  }
}

TEST(MacosEventCompatTest, MouseEventClickCountPreserved) {
  platf::install_event_compatibility_fixes();

  @autoreleasepool {
    NSEvent *single_click = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                               location:NSMakePoint(100, 100)
                                          modifierFlags:0
                                              timestamp:1.0
                                           windowNumber:0
                                                context:nil
                                            eventNumber:1
                                             clickCount:1
                                               pressure:1.0];

    NSEvent *double_click = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                               location:NSMakePoint(100, 100)
                                          modifierFlags:0
                                              timestamp:2.0
                                           windowNumber:0
                                                context:nil
                                            eventNumber:2
                                             clickCount:2
                                               pressure:1.0];

    NSEvent *right_click = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown
                                              location:NSMakePoint(100, 100)
                                         modifierFlags:0
                                             timestamp:3.0
                                          windowNumber:0
                                               context:nil
                                           eventNumber:3
                                            clickCount:1
                                              pressure:1.0];

    EXPECT_EQ([single_click clickCount], 1);
    EXPECT_EQ([double_click clickCount], 2);
    EXPECT_EQ([right_click clickCount], 1);
  }
}
#endif
