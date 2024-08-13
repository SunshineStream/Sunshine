/**
 * @file src/platform/linux/input/legacy_input.cpp
 * @brief Implementation of input handling, prior to migration to inputtino
 * @todo Remove this file after the next stable release
 */
#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>

extern "C" {
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
}

#ifdef SUNSHINE_BUILD_X11
  #include <X11/Xutil.h>
  #include <X11/extensions/XTest.h>
  #include <X11/keysym.h>
  #include <X11/keysymdef.h>
#endif

#include <boost/locale.hpp>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>

#include "src/config.h"
#include "src/input/processor.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

#include "src/platform/common.h"

#include "src/platform/linux/misc.h"

// Support older versions
#ifndef REL_HWHEEL_HI_RES
  #define REL_HWHEEL_HI_RES 0x0c
#endif

#ifndef REL_WHEEL_HI_RES
  #define REL_WHEEL_HI_RES 0x0b
#endif

using namespace std::literals;

namespace platf {
  static bool has_uinput = false;

#ifdef SUNSHINE_BUILD_X11
  namespace x11 {
  #define _FN(x, ret, args)    \
    typedef ret(*x##_fn) args; \
    static x##_fn x

    _FN(OpenDisplay, Display *, (_Xconst char *display_name));
    _FN(CloseDisplay, int, (Display * display));
    _FN(InitThreads, Status, (void) );
    _FN(Flush, int, (Display *) );

    namespace tst {
      _FN(FakeMotionEvent, int, (Display * dpy, int screen_numer, int x, int y, unsigned long delay));
      _FN(FakeRelativeMotionEvent, int, (Display * dpy, int deltaX, int deltaY, unsigned long delay));
      _FN(FakeButtonEvent, int, (Display * dpy, unsigned int button, Bool is_press, unsigned long delay));
      _FN(FakeKeyEvent, int, (Display * dpy, unsigned int keycode, Bool is_press, unsigned long delay));

      static int
      init() {
        static void *handle { nullptr };
        static bool funcs_loaded = false;

        if (funcs_loaded) return 0;

        if (!handle) {
          handle = dyn::handle({ "libXtst.so.6", "libXtst.so" });
          if (!handle) {
            return -1;
          }
        }

        std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
          { (dyn::apiproc *) &FakeMotionEvent, "XTestFakeMotionEvent" },
          { (dyn::apiproc *) &FakeRelativeMotionEvent, "XTestFakeRelativeMotionEvent" },
          { (dyn::apiproc *) &FakeButtonEvent, "XTestFakeButtonEvent" },
          { (dyn::apiproc *) &FakeKeyEvent, "XTestFakeKeyEvent" },
        };

        if (dyn::load(handle, funcs)) {
          return -1;
        }

        funcs_loaded = true;
        return 0;
      }
    }  // namespace tst

    static int
    init() {
      static void *handle { nullptr };
      static bool funcs_loaded = false;

      if (funcs_loaded) return 0;

      if (!handle) {
        handle = dyn::handle({ "libX11.so.6", "libX11.so" });
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        { (dyn::apiproc *) &OpenDisplay, "XOpenDisplay" },
        { (dyn::apiproc *) &CloseDisplay, "XCloseDisplay" },
        { (dyn::apiproc *) &InitThreads, "XInitThreads" },
        { (dyn::apiproc *) &Flush, "XFlush" },
      };

      if (dyn::load(handle, funcs)) {
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }
  }  // namespace x11
#endif

  constexpr auto mail_evdev = "platf::evdev"sv;

  using evdev_t = util::safe_ptr<libevdev, libevdev_free>;
  using uinput_t = util::safe_ptr<libevdev_uinput, libevdev_uinput_destroy>;

  constexpr pollfd read_pollfd { -1, 0, 0 };
  KITTY_USING_MOVE_T(pollfd_t, pollfd, read_pollfd, {
    if (el.fd >= 0) {
      ioctl(el.fd, EVIOCGRAB, (void *) 0);

      close(el.fd);
    }
  });

  using mail_evdev_t = std::tuple<int, uinput_t::pointer, feedback_queue_t, pollfd_t>;

  struct keycode_t {
    std::uint32_t keycode;
    std::uint32_t scancode;

#ifdef SUNSHINE_BUILD_X11
    KeySym keysym;
#endif
  };

  constexpr auto UNKNOWN = 0;

  /**
   * @brief Initializes the keycode constants for translating
   *        moonlight keycodes to linux/X11 keycodes.
   */
  static constexpr std::array<keycode_t, 0xE3>
  init_keycodes() {
    std::array<keycode_t, 0xE3> keycodes {};

#ifdef SUNSHINE_BUILD_X11
  #define __CONVERT_UNSAFE(wincode, linuxcode, scancode, keysym) \
    keycodes[wincode] = keycode_t { linuxcode, scancode, keysym };
#else
  #define __CONVERT_UNSAFE(wincode, linuxcode, scancode, keysym) \
    keycodes[wincode] = keycode_t { linuxcode, scancode };
#endif

#define __CONVERT(wincode, linuxcode, scancode, keysym)                               \
  static_assert(wincode < keycodes.size(), "Keycode doesn't fit into keycode array"); \
  static_assert(wincode >= 0, "Are you mad?, keycode needs to be greater than zero"); \
  __CONVERT_UNSAFE(wincode, linuxcode, scancode, keysym)

    __CONVERT(0x08 /* VKEY_BACK */, KEY_BACKSPACE, 0x7002A, XK_BackSpace);
    __CONVERT(0x09 /* VKEY_TAB */, KEY_TAB, 0x7002B, XK_Tab);
    __CONVERT(0x0C /* VKEY_CLEAR */, KEY_CLEAR, UNKNOWN, XK_Clear);
    __CONVERT(0x0D /* VKEY_RETURN */, KEY_ENTER, 0x70028, XK_Return);
    __CONVERT(0x10 /* VKEY_SHIFT */, KEY_LEFTSHIFT, 0x700E1, XK_Shift_L);
    __CONVERT(0x11 /* VKEY_CONTROL */, KEY_LEFTCTRL, 0x700E0, XK_Control_L);
    __CONVERT(0x12 /* VKEY_MENU */, KEY_LEFTALT, UNKNOWN, XK_Alt_L);
    __CONVERT(0x13 /* VKEY_PAUSE */, KEY_PAUSE, UNKNOWN, XK_Pause);
    __CONVERT(0x14 /* VKEY_CAPITAL */, KEY_CAPSLOCK, 0x70039, XK_Caps_Lock);
    __CONVERT(0x15 /* VKEY_KANA */, KEY_KATAKANAHIRAGANA, UNKNOWN, XK_Kana_Shift);
    __CONVERT(0x16 /* VKEY_HANGUL */, KEY_HANGEUL, UNKNOWN, XK_Hangul);
    __CONVERT(0x17 /* VKEY_JUNJA */, KEY_HANJA, UNKNOWN, XK_Hangul_Jeonja);
    __CONVERT(0x19 /* VKEY_KANJI */, KEY_KATAKANA, UNKNOWN, XK_Kanji);
    __CONVERT(0x1B /* VKEY_ESCAPE */, KEY_ESC, 0x70029, XK_Escape);
    __CONVERT(0x20 /* VKEY_SPACE */, KEY_SPACE, 0x7002C, XK_space);
    __CONVERT(0x21 /* VKEY_PRIOR */, KEY_PAGEUP, 0x7004B, XK_Page_Up);
    __CONVERT(0x22 /* VKEY_NEXT */, KEY_PAGEDOWN, 0x7004E, XK_Page_Down);
    __CONVERT(0x23 /* VKEY_END */, KEY_END, 0x7004D, XK_End);
    __CONVERT(0x24 /* VKEY_HOME */, KEY_HOME, 0x7004A, XK_Home);
    __CONVERT(0x25 /* VKEY_LEFT */, KEY_LEFT, 0x70050, XK_Left);
    __CONVERT(0x26 /* VKEY_UP */, KEY_UP, 0x70052, XK_Up);
    __CONVERT(0x27 /* VKEY_RIGHT */, KEY_RIGHT, 0x7004F, XK_Right);
    __CONVERT(0x28 /* VKEY_DOWN */, KEY_DOWN, 0x70051, XK_Down);
    __CONVERT(0x29 /* VKEY_SELECT */, KEY_SELECT, UNKNOWN, XK_Select);
    __CONVERT(0x2A /* VKEY_PRINT */, KEY_PRINT, UNKNOWN, XK_Print);
    __CONVERT(0x2C /* VKEY_SNAPSHOT */, KEY_SYSRQ, 0x70046, XK_Sys_Req);
    __CONVERT(0x2D /* VKEY_INSERT */, KEY_INSERT, 0x70049, XK_Insert);
    __CONVERT(0x2E /* VKEY_DELETE */, KEY_DELETE, 0x7004C, XK_Delete);
    __CONVERT(0x2F /* VKEY_HELP */, KEY_HELP, UNKNOWN, XK_Help);
    __CONVERT(0x30 /* VKEY_0 */, KEY_0, 0x70027, XK_0);
    __CONVERT(0x31 /* VKEY_1 */, KEY_1, 0x7001E, XK_1);
    __CONVERT(0x32 /* VKEY_2 */, KEY_2, 0x7001F, XK_2);
    __CONVERT(0x33 /* VKEY_3 */, KEY_3, 0x70020, XK_3);
    __CONVERT(0x34 /* VKEY_4 */, KEY_4, 0x70021, XK_4);
    __CONVERT(0x35 /* VKEY_5 */, KEY_5, 0x70022, XK_5);
    __CONVERT(0x36 /* VKEY_6 */, KEY_6, 0x70023, XK_6);
    __CONVERT(0x37 /* VKEY_7 */, KEY_7, 0x70024, XK_7);
    __CONVERT(0x38 /* VKEY_8 */, KEY_8, 0x70025, XK_8);
    __CONVERT(0x39 /* VKEY_9 */, KEY_9, 0x70026, XK_9);
    __CONVERT(0x41 /* VKEY_A */, KEY_A, 0x70004, XK_A);
    __CONVERT(0x42 /* VKEY_B */, KEY_B, 0x70005, XK_B);
    __CONVERT(0x43 /* VKEY_C */, KEY_C, 0x70006, XK_C);
    __CONVERT(0x44 /* VKEY_D */, KEY_D, 0x70007, XK_D);
    __CONVERT(0x45 /* VKEY_E */, KEY_E, 0x70008, XK_E);
    __CONVERT(0x46 /* VKEY_F */, KEY_F, 0x70009, XK_F);
    __CONVERT(0x47 /* VKEY_G */, KEY_G, 0x7000A, XK_G);
    __CONVERT(0x48 /* VKEY_H */, KEY_H, 0x7000B, XK_H);
    __CONVERT(0x49 /* VKEY_I */, KEY_I, 0x7000C, XK_I);
    __CONVERT(0x4A /* VKEY_J */, KEY_J, 0x7000D, XK_J);
    __CONVERT(0x4B /* VKEY_K */, KEY_K, 0x7000E, XK_K);
    __CONVERT(0x4C /* VKEY_L */, KEY_L, 0x7000F, XK_L);
    __CONVERT(0x4D /* VKEY_M */, KEY_M, 0x70010, XK_M);
    __CONVERT(0x4E /* VKEY_N */, KEY_N, 0x70011, XK_N);
    __CONVERT(0x4F /* VKEY_O */, KEY_O, 0x70012, XK_O);
    __CONVERT(0x50 /* VKEY_P */, KEY_P, 0x70013, XK_P);
    __CONVERT(0x51 /* VKEY_Q */, KEY_Q, 0x70014, XK_Q);
    __CONVERT(0x52 /* VKEY_R */, KEY_R, 0x70015, XK_R);
    __CONVERT(0x53 /* VKEY_S */, KEY_S, 0x70016, XK_S);
    __CONVERT(0x54 /* VKEY_T */, KEY_T, 0x70017, XK_T);
    __CONVERT(0x55 /* VKEY_U */, KEY_U, 0x70018, XK_U);
    __CONVERT(0x56 /* VKEY_V */, KEY_V, 0x70019, XK_V);
    __CONVERT(0x57 /* VKEY_W */, KEY_W, 0x7001A, XK_W);
    __CONVERT(0x58 /* VKEY_X */, KEY_X, 0x7001B, XK_X);
    __CONVERT(0x59 /* VKEY_Y */, KEY_Y, 0x7001C, XK_Y);
    __CONVERT(0x5A /* VKEY_Z */, KEY_Z, 0x7001D, XK_Z);
    __CONVERT(0x5B /* VKEY_LWIN */, KEY_LEFTMETA, 0x700E3, XK_Meta_L);
    __CONVERT(0x5C /* VKEY_RWIN */, KEY_RIGHTMETA, 0x700E7, XK_Meta_R);
    __CONVERT(0x5F /* VKEY_SLEEP */, KEY_SLEEP, UNKNOWN, UNKNOWN);
    __CONVERT(0x60 /* VKEY_NUMPAD0 */, KEY_KP0, 0x70062, XK_KP_0);
    __CONVERT(0x61 /* VKEY_NUMPAD1 */, KEY_KP1, 0x70059, XK_KP_1);
    __CONVERT(0x62 /* VKEY_NUMPAD2 */, KEY_KP2, 0x7005A, XK_KP_2);
    __CONVERT(0x63 /* VKEY_NUMPAD3 */, KEY_KP3, 0x7005B, XK_KP_3);
    __CONVERT(0x64 /* VKEY_NUMPAD4 */, KEY_KP4, 0x7005C, XK_KP_4);
    __CONVERT(0x65 /* VKEY_NUMPAD5 */, KEY_KP5, 0x7005D, XK_KP_5);
    __CONVERT(0x66 /* VKEY_NUMPAD6 */, KEY_KP6, 0x7005E, XK_KP_6);
    __CONVERT(0x67 /* VKEY_NUMPAD7 */, KEY_KP7, 0x7005F, XK_KP_7);
    __CONVERT(0x68 /* VKEY_NUMPAD8 */, KEY_KP8, 0x70060, XK_KP_8);
    __CONVERT(0x69 /* VKEY_NUMPAD9 */, KEY_KP9, 0x70061, XK_KP_9);
    __CONVERT(0x6A /* VKEY_MULTIPLY */, KEY_KPASTERISK, 0x70055, XK_KP_Multiply);
    __CONVERT(0x6B /* VKEY_ADD */, KEY_KPPLUS, 0x70057, XK_KP_Add);
    __CONVERT(0x6C /* VKEY_SEPARATOR */, KEY_KPCOMMA, UNKNOWN, XK_KP_Separator);
    __CONVERT(0x6D /* VKEY_SUBTRACT */, KEY_KPMINUS, 0x70056, XK_KP_Subtract);
    __CONVERT(0x6E /* VKEY_DECIMAL */, KEY_KPDOT, 0x70063, XK_KP_Decimal);
    __CONVERT(0x6F /* VKEY_DIVIDE */, KEY_KPSLASH, 0x70054, XK_KP_Divide);
    __CONVERT(0x70 /* VKEY_F1 */, KEY_F1, 0x70046, XK_F1);
    __CONVERT(0x71 /* VKEY_F2 */, KEY_F2, 0x70047, XK_F2);
    __CONVERT(0x72 /* VKEY_F3 */, KEY_F3, 0x70048, XK_F3);
    __CONVERT(0x73 /* VKEY_F4 */, KEY_F4, 0x70049, XK_F4);
    __CONVERT(0x74 /* VKEY_F5 */, KEY_F5, 0x7004a, XK_F5);
    __CONVERT(0x75 /* VKEY_F6 */, KEY_F6, 0x7004b, XK_F6);
    __CONVERT(0x76 /* VKEY_F7 */, KEY_F7, 0x7004c, XK_F7);
    __CONVERT(0x77 /* VKEY_F8 */, KEY_F8, 0x7004d, XK_F8);
    __CONVERT(0x78 /* VKEY_F9 */, KEY_F9, 0x7004e, XK_F9);
    __CONVERT(0x79 /* VKEY_F10 */, KEY_F10, 0x70044, XK_F10);
    __CONVERT(0x7A /* VKEY_F11 */, KEY_F11, 0x70044, XK_F11);
    __CONVERT(0x7B /* VKEY_F12 */, KEY_F12, 0x70045, XK_F12);
    __CONVERT(0x7C /* VKEY_F13 */, KEY_F13, 0x7003a, XK_F13);
    __CONVERT(0x7D /* VKEY_F14 */, KEY_F14, 0x7003b, XK_F14);
    __CONVERT(0x7E /* VKEY_F15 */, KEY_F15, 0x7003c, XK_F15);
    __CONVERT(0x7F /* VKEY_F16 */, KEY_F16, 0x7003d, XK_F16);
    __CONVERT(0x80 /* VKEY_F17 */, KEY_F17, 0x7003e, XK_F17);
    __CONVERT(0x81 /* VKEY_F18 */, KEY_F18, 0x7003f, XK_F18);
    __CONVERT(0x82 /* VKEY_F19 */, KEY_F19, 0x70040, XK_F19);
    __CONVERT(0x83 /* VKEY_F20 */, KEY_F20, 0x70041, XK_F20);
    __CONVERT(0x84 /* VKEY_F21 */, KEY_F21, 0x70042, XK_F21);
    __CONVERT(0x85 /* VKEY_F22 */, KEY_F12, 0x70043, XK_F12);
    __CONVERT(0x86 /* VKEY_F23 */, KEY_F23, 0x70044, XK_F23);
    __CONVERT(0x87 /* VKEY_F24 */, KEY_F24, 0x70045, XK_F24);
    __CONVERT(0x90 /* VKEY_NUMLOCK */, KEY_NUMLOCK, 0x70053, XK_Num_Lock);
    __CONVERT(0x91 /* VKEY_SCROLL */, KEY_SCROLLLOCK, 0x70047, XK_Scroll_Lock);
    __CONVERT(0xA0 /* VKEY_LSHIFT */, KEY_LEFTSHIFT, 0x700E1, XK_Shift_L);
    __CONVERT(0xA1 /* VKEY_RSHIFT */, KEY_RIGHTSHIFT, 0x700E5, XK_Shift_R);
    __CONVERT(0xA2 /* VKEY_LCONTROL */, KEY_LEFTCTRL, 0x700E0, XK_Control_L);
    __CONVERT(0xA3 /* VKEY_RCONTROL */, KEY_RIGHTCTRL, 0x700E4, XK_Control_R);
    __CONVERT(0xA4 /* VKEY_LMENU */, KEY_LEFTALT, 0x7002E, XK_Alt_L);
    __CONVERT(0xA5 /* VKEY_RMENU */, KEY_RIGHTALT, 0x700E6, XK_Alt_R);
    __CONVERT(0xBA /* VKEY_OEM_1 */, KEY_SEMICOLON, 0x70033, XK_semicolon);
    __CONVERT(0xBB /* VKEY_OEM_PLUS */, KEY_EQUAL, 0x7002E, XK_equal);
    __CONVERT(0xBC /* VKEY_OEM_COMMA */, KEY_COMMA, 0x70036, XK_comma);
    __CONVERT(0xBD /* VKEY_OEM_MINUS */, KEY_MINUS, 0x7002D, XK_minus);
    __CONVERT(0xBE /* VKEY_OEM_PERIOD */, KEY_DOT, 0x70037, XK_period);
    __CONVERT(0xBF /* VKEY_OEM_2 */, KEY_SLASH, 0x70038, XK_slash);
    __CONVERT(0xC0 /* VKEY_OEM_3 */, KEY_GRAVE, 0x70035, XK_grave);
    __CONVERT(0xDB /* VKEY_OEM_4 */, KEY_LEFTBRACE, 0x7002F, XK_braceleft);
    __CONVERT(0xDC /* VKEY_OEM_5 */, KEY_BACKSLASH, 0x70031, XK_backslash);
    __CONVERT(0xDD /* VKEY_OEM_6 */, KEY_RIGHTBRACE, 0x70030, XK_braceright);
    __CONVERT(0xDE /* VKEY_OEM_7 */, KEY_APOSTROPHE, 0x70034, XK_apostrophe);
    __CONVERT(0xE2 /* VKEY_NON_US_BACKSLASH */, KEY_102ND, 0x70064, XK_backslash);
#undef __CONVERT
#undef __CONVERT_UNSAFE

    return keycodes;
  }

  static constexpr auto keycodes = init_keycodes();

  constexpr touch_port_t target_touch_port {
    0, 0,
    19200, 12000
  };

  static std::pair<std::uint32_t, std::uint32_t>
  operator*(const std::pair<std::uint32_t, std::uint32_t> &l, int r) {
    return {
      l.first * r,
      l.second * r,
    };
  }

  static std::pair<std::uint32_t, std::uint32_t>
  operator/(const std::pair<std::uint32_t, std::uint32_t> &l, int r) {
    return {
      l.first / r,
      l.second / r,
    };
  }

  static std::pair<std::uint32_t, std::uint32_t> &
  operator+=(std::pair<std::uint32_t, std::uint32_t> &l, const std::pair<std::uint32_t, std::uint32_t> &r) {
    l.first += r.first;
    l.second += r.second;

    return l;
  }

  static inline void
  print(const ff_envelope &envelope) {
    BOOST_LOG(debug)
      << "Envelope:"sv << std::endl
      << "  attack_length: " << envelope.attack_length << std::endl
      << "  attack_level: " << envelope.attack_level << std::endl
      << "  fade_length: " << envelope.fade_length << std::endl
      << "  fade_level: " << envelope.fade_level;
  }

  static inline void
  print(const ff_replay &replay) {
    BOOST_LOG(debug)
      << "Replay:"sv << std::endl
      << "  length: "sv << replay.length << std::endl
      << "  delay: "sv << replay.delay;
  }

  static inline void
  print(const ff_trigger &trigger) {
    BOOST_LOG(debug)
      << "Trigger:"sv << std::endl
      << "  button: "sv << trigger.button << std::endl
      << "  interval: "sv << trigger.interval;
  }

  static inline void
  print(const ff_effect &effect) {
    BOOST_LOG(debug)
      << std::endl
      << std::endl
      << "Received rumble effect with id: ["sv << effect.id << ']';

    switch (effect.type) {
      case FF_CONSTANT:
        BOOST_LOG(debug)
          << "FF_CONSTANT:"sv << std::endl
          << "  direction: "sv << effect.direction << std::endl
          << "  level: "sv << effect.u.constant.level;

        print(effect.u.constant.envelope);
        break;

      case FF_PERIODIC:
        BOOST_LOG(debug)
          << "FF_CONSTANT:"sv << std::endl
          << "  direction: "sv << effect.direction << std::endl
          << "  waveform: "sv << effect.u.periodic.waveform << std::endl
          << "  period: "sv << effect.u.periodic.period << std::endl
          << "  magnitude: "sv << effect.u.periodic.magnitude << std::endl
          << "  offset: "sv << effect.u.periodic.offset << std::endl
          << "  phase: "sv << effect.u.periodic.phase;

        print(effect.u.periodic.envelope);
        break;

      case FF_RAMP:
        BOOST_LOG(debug)
          << "FF_RAMP:"sv << std::endl
          << "  direction: "sv << effect.direction << std::endl
          << "  start_level:" << effect.u.ramp.start_level << std::endl
          << "  end_level:" << effect.u.ramp.end_level;

        print(effect.u.ramp.envelope);
        break;

      case FF_RUMBLE:
        BOOST_LOG(debug)
          << "FF_RUMBLE:" << std::endl
          << "  direction: "sv << effect.direction << std::endl
          << "  strong_magnitude: " << effect.u.rumble.strong_magnitude << std::endl
          << "  weak_magnitude: " << effect.u.rumble.weak_magnitude;
        break;

      case FF_SPRING:
        BOOST_LOG(debug)
          << "FF_SPRING:" << std::endl
          << "  direction: "sv << effect.direction;
        break;

      case FF_FRICTION:
        BOOST_LOG(debug)
          << "FF_FRICTION:" << std::endl
          << "  direction: "sv << effect.direction;
        break;

      case FF_DAMPER:
        BOOST_LOG(debug)
          << "FF_DAMPER:" << std::endl
          << "  direction: "sv << effect.direction;
        break;

      case FF_INERTIA:
        BOOST_LOG(debug)
          << "FF_INERTIA:" << std::endl
          << "  direction: "sv << effect.direction;
        break;

      case FF_CUSTOM:
        BOOST_LOG(debug)
          << "FF_CUSTOM:" << std::endl
          << "  direction: "sv << effect.direction;
        break;

      default:
        BOOST_LOG(debug)
          << "FF_UNKNOWN:" << std::endl
          << "  direction: "sv << effect.direction;
        break;
    }

    print(effect.replay);
    print(effect.trigger);
  }

  // Emulate rumble effects
  class effect_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE(effect_t)

    effect_t(std::uint8_t gamepadnr, uinput_t::pointer dev, feedback_queue_t &&q):
        gamepadnr { gamepadnr }, dev { dev }, rumble_queue { std::move(q) }, gain { 0xFFFF }, id_to_data {} {}

    class data_t {
    public:
      KITTY_DEFAULT_CONSTR(data_t)

      data_t(const ff_effect &effect):
          delay { effect.replay.delay },
          length { effect.replay.length },
          end_point { std::chrono::steady_clock::time_point::min() },
          envelope {},
          start {},
          end {} {
        switch (effect.type) {
          case FF_CONSTANT:
            start.weak = effect.u.constant.level;
            start.strong = effect.u.constant.level;
            end.weak = effect.u.constant.level;
            end.strong = effect.u.constant.level;

            envelope = effect.u.constant.envelope;
            break;
          case FF_PERIODIC:
            start.weak = effect.u.periodic.magnitude;
            start.strong = effect.u.periodic.magnitude;
            end.weak = effect.u.periodic.magnitude;
            end.strong = effect.u.periodic.magnitude;

            envelope = effect.u.periodic.envelope;
            break;

          case FF_RAMP:
            start.weak = effect.u.ramp.start_level;
            start.strong = effect.u.ramp.start_level;
            end.weak = effect.u.ramp.end_level;
            end.strong = effect.u.ramp.end_level;

            envelope = effect.u.ramp.envelope;
            break;

          case FF_RUMBLE:
            start.weak = effect.u.rumble.weak_magnitude;
            start.strong = effect.u.rumble.strong_magnitude;
            end.weak = effect.u.rumble.weak_magnitude;
            end.strong = effect.u.rumble.strong_magnitude;
            break;

          default:
            BOOST_LOG(warning) << "Effect type ["sv << effect.id << "] not implemented"sv;
        }
      }

      std::uint32_t
      magnitude(std::chrono::milliseconds time_left, std::uint32_t start, std::uint32_t end) {
        auto rel = end - start;

        return start + (rel * time_left.count() / length.count());
      }

      std::pair<std::uint32_t, std::uint32_t>
      rumble(std::chrono::steady_clock::time_point tp) {
        if (end_point < tp) {
          return {};
        }

        auto time_left =
          std::chrono::duration_cast<std::chrono::milliseconds>(
            end_point - tp);

        // If it needs to be delayed'
        if (time_left > length) {
          return {};
        }

        auto t = length - time_left;

        auto weak = magnitude(t, start.weak, end.weak);
        auto strong = magnitude(t, start.strong, end.strong);

        if (t.count() < envelope.attack_length) {
          weak = (envelope.attack_level * t.count() + weak * (envelope.attack_length - t.count())) / envelope.attack_length;
          strong = (envelope.attack_level * t.count() + strong * (envelope.attack_length - t.count())) / envelope.attack_length;
        }
        else if (time_left.count() < envelope.fade_length) {
          auto dt = (t - length).count() + envelope.fade_length;

          weak = (envelope.fade_level * dt + weak * (envelope.fade_length - dt)) / envelope.fade_length;
          strong = (envelope.fade_level * dt + strong * (envelope.fade_length - dt)) / envelope.fade_length;
        }

        return {
          weak, strong
        };
      }

      void
      activate() {
        end_point = std::chrono::steady_clock::now() + delay + length;
      }

      void
      deactivate() {
        end_point = std::chrono::steady_clock::time_point::min();
      }

      std::chrono::milliseconds delay;
      std::chrono::milliseconds length;

      std::chrono::steady_clock::time_point end_point;

      ff_envelope envelope;
      struct {
        std::uint32_t weak, strong;
      } start;

      struct {
        std::uint32_t weak, strong;
      } end;
    };

    std::pair<std::uint32_t, std::uint32_t>
    rumble(std::chrono::steady_clock::time_point tp) {
      std::pair<std::uint32_t, std::uint32_t> weak_strong {};
      for (auto &[_, data] : id_to_data) {
        weak_strong += data.rumble(tp);
      }

      weak_strong.first = std::clamp<std::uint32_t>(weak_strong.first, 0, 0xFFFF);
      weak_strong.second = std::clamp<std::uint32_t>(weak_strong.second, 0, 0xFFFF);

      old_rumble = weak_strong * gain / 0xFFFF;
      return old_rumble;
    }

    void
    upload(const ff_effect &effect) {
      print(effect);

      auto it = id_to_data.find(effect.id);

      if (it == std::end(id_to_data)) {
        id_to_data.emplace(effect.id, effect);
        return;
      }

      data_t data { effect };

      data.end_point = it->second.end_point;
      it->second = data;
    }

    void
    activate(int id) {
      auto it = id_to_data.find(id);

      if (it != std::end(id_to_data)) {
        it->second.activate();
      }
    }

    void
    deactivate(int id) {
      auto it = id_to_data.find(id);

      if (it != std::end(id_to_data)) {
        it->second.deactivate();
      }
    }

    void
    erase(int id) {
      id_to_data.erase(id);
      BOOST_LOG(debug) << "Removed rumble effect id ["sv << id << ']';
    }

    // Client-relative gamepad index for rumble notifications
    std::uint8_t gamepadnr;

    // Used as ID for adding/removinf devices from evdev notifications
    uinput_t::pointer dev;

    feedback_queue_t rumble_queue;

    int gain;

    // No need to send rumble data when old values equals the new values
    std::pair<std::uint32_t, std::uint32_t> old_rumble;

    std::unordered_map<int, data_t> id_to_data;
  };

  struct rumble_ctx_t {
    std::thread rumble_thread;

    safe::queue_t<mail_evdev_t> rumble_queue_queue;
  };

  void
  broadcastRumble(safe::queue_t<mail_evdev_t> &ctx);
  int
  startRumble(rumble_ctx_t &ctx) {
    ctx.rumble_thread = std::thread { broadcastRumble, std::ref(ctx.rumble_queue_queue) };

    return 0;
  }

  void
  stopRumble(rumble_ctx_t &ctx) {
    ctx.rumble_queue_queue.stop();

    BOOST_LOG(debug) << "Waiting for Gamepad notifications to stop..."sv;
    ctx.rumble_thread.join();
    BOOST_LOG(debug) << "Gamepad notifications stopped"sv;
  }

  static auto notifications = safe::make_shared<rumble_ctx_t>(startRumble, stopRumble);

  struct input_raw_t {
  public:
    void
    clear_mouse_rel() {
      std::filesystem::path mouse_path { appdata() / "sunshine_mouse_rel"sv };

      if (std::filesystem::is_symlink(mouse_path)) {
        std::filesystem::remove(mouse_path);
      }

      mouse_rel_input.reset();
    }

    void
    clear_keyboard() {
      std::filesystem::path key_path { appdata() / "sunshine_keyboard"sv };

      if (std::filesystem::is_symlink(key_path)) {
        std::filesystem::remove(key_path);
      }

      keyboard_input.reset();
    }

    void
    clear_mouse_abs() {
      std::filesystem::path mouse_path { appdata() / "sunshine_mouse_abs"sv };

      if (std::filesystem::is_symlink(mouse_path)) {
        std::filesystem::remove(mouse_path);
      }

      mouse_abs_input.reset();
    }

    void
    clear_gamepad(int nr) {
      auto &[dev, _] = gamepads[nr];

      if (!dev) {
        return;
      }

      // Remove this gamepad from notifications
      rumble_ctx->rumble_queue_queue.raise(nr, dev.get(), nullptr, pollfd_t {});

      std::stringstream ss;

      ss << "sunshine_gamepad_"sv << nr;

      auto gamepad_path = platf::appdata() / ss.str();
      if (std::filesystem::is_symlink(gamepad_path)) {
        std::filesystem::remove(gamepad_path);
      }

      gamepads[nr] = std::make_pair(uinput_t {}, gamepad_state_t {});
    }

    int
    create_mouse_abs() {
      int err = libevdev_uinput_create_from_device(mouse_abs_dev.get(), LIBEVDEV_UINPUT_OPEN_MANAGED, &mouse_abs_input);

      if (err) {
        BOOST_LOG(error) << "Could not create Sunshine Mouse (Absolute): "sv << strerror(-err);
        return -1;
      }

      std::filesystem::create_symlink(libevdev_uinput_get_devnode(mouse_abs_input.get()), appdata() / "sunshine_mouse_abs"sv);

      return 0;
    }

    int
    create_mouse_rel() {
      int err = libevdev_uinput_create_from_device(mouse_rel_dev.get(), LIBEVDEV_UINPUT_OPEN_MANAGED, &mouse_rel_input);

      if (err) {
        BOOST_LOG(error) << "Could not create Sunshine Mouse (Relative): "sv << strerror(-err);
        return -1;
      }

      std::filesystem::create_symlink(libevdev_uinput_get_devnode(mouse_rel_input.get()), appdata() / "sunshine_mouse_rel"sv);

      return 0;
    }

    int
    create_keyboard() {
      int err = libevdev_uinput_create_from_device(keyboard_dev.get(), LIBEVDEV_UINPUT_OPEN_MANAGED, &keyboard_input);

      if (err) {
        BOOST_LOG(error) << "Could not create Sunshine Keyboard: "sv << strerror(-err);
        return -1;
      }

      std::filesystem::create_symlink(libevdev_uinput_get_devnode(keyboard_input.get()), appdata() / "sunshine_keyboard"sv);

      return 0;
    }

    int
    alloc_gamepad(const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t &&feedback_queue) {
      TUPLE_2D_REF(input, gamepad_state, gamepads[id.globalIndex]);

      int err = libevdev_uinput_create_from_device(gamepad_dev.get(), LIBEVDEV_UINPUT_OPEN_MANAGED, &input);

      gamepad_state = gamepad_state_t {};

      if (err) {
        BOOST_LOG(error) << "Could not create Sunshine Gamepad: "sv << strerror(-err);
        return -1;
      }

      std::stringstream ss;
      ss << "sunshine_gamepad_"sv << id.globalIndex;
      auto gamepad_path = platf::appdata() / ss.str();

      if (std::filesystem::is_symlink(gamepad_path)) {
        std::filesystem::remove(gamepad_path);
      }

      auto dev_node = libevdev_uinput_get_devnode(input.get());

      rumble_ctx->rumble_queue_queue.raise(
        id.clientRelativeIndex,
        input.get(),
        std::move(feedback_queue),
        pollfd_t {
          dup(libevdev_uinput_get_fd(input.get())),
          (std::int16_t) POLLIN,
          (std::int16_t) 0,
        });

      std::filesystem::create_symlink(dev_node, gamepad_path);
      return 0;
    }

    void
    clear() {
      clear_keyboard();
      clear_mouse_abs();
      clear_mouse_rel();
      for (int x = 0; x < gamepads.size(); ++x) {
        clear_gamepad(x);
      }

#ifdef SUNSHINE_BUILD_X11
      if (display) {
        x11::CloseDisplay(display);
        display = nullptr;
      }
#endif
    }

    ~input_raw_t() {
      clear();
    }

    safe::shared_t<rumble_ctx_t>::ptr_t rumble_ctx;

    std::vector<std::pair<uinput_t, gamepad_state_t>> gamepads;
    uinput_t mouse_rel_input;
    uinput_t mouse_abs_input;
    uinput_t keyboard_input;

    uint8_t mouse_rel_buttons_down = 0;
    uint8_t mouse_abs_buttons_down = 0;

    uinput_t::pointer last_mouse_device_used = nullptr;
    uint8_t *last_mouse_device_buttons_down = nullptr;

    evdev_t gamepad_dev;
    evdev_t mouse_rel_dev;
    evdev_t mouse_abs_dev;
    evdev_t keyboard_dev;
    evdev_t touchscreen_dev;
    evdev_t pen_dev;

    int accumulated_vscroll_delta = 0;
    int accumulated_hscroll_delta = 0;

#ifdef SUNSHINE_BUILD_X11
    Display *display;
#endif
  };

  inline void
  rumbleIterate(std::vector<effect_t> &effects, std::vector<pollfd_t> &polls, std::chrono::milliseconds to) {
    std::vector<pollfd> polls_recv;
    polls_recv.reserve(polls.size());
    for (auto &poll : polls) {
      polls_recv.emplace_back(poll.el);
    }

    auto res = poll(polls_recv.data(), polls_recv.size(), to.count());

    // If timed out
    if (!res) {
      return;
    }

    if (res < 0) {
      char err_str[1024];
      BOOST_LOG(error) << "Couldn't poll Gamepad file descriptors: "sv << strerror_r(errno, err_str, 1024);

      return;
    }

    for (int x = 0; x < polls.size(); ++x) {
      auto poll = std::begin(polls) + x;
      auto effect_it = std::begin(effects) + x;

      auto fd = (*poll)->fd;

      // TUPLE_2D_REF(dev, q, *dev_q_it);

      // on error
      if (polls_recv[x].revents & (POLLHUP | POLLRDHUP | POLLERR)) {
        BOOST_LOG(warning) << "Gamepad ["sv << x << "] file descriptor closed unexpectedly"sv;

        polls.erase(poll);
        effects.erase(effect_it);

        --x;
        continue;
      }

      if (!(polls_recv[x].revents & POLLIN)) {
        continue;
      }

      input_event events[64];

      // Read all available events
      auto bytes = read(fd, &events, sizeof(events));

      if (bytes < 0) {
        char err_str[1024];

        BOOST_LOG(error) << "Couldn't read evdev input ["sv << errno << "]: "sv << strerror_r(errno, err_str, 1024);

        polls.erase(poll);
        effects.erase(effect_it);

        --x;
        continue;
      }

      if (bytes < sizeof(input_event)) {
        BOOST_LOG(warning) << "Reading evdev input: Expected at least "sv << sizeof(input_event) << " bytes, got "sv << bytes << " instead"sv;
        continue;
      }

      auto event_count = bytes / sizeof(input_event);

      for (auto event = events; event != (events + event_count); ++event) {
        switch (event->type) {
          case EV_FF:
            // BOOST_LOG(debug) << "EV_FF: "sv << event->value << " aka "sv << util::hex(event->value).to_string_view();

            if (event->code == FF_GAIN) {
              BOOST_LOG(debug) << "EV_FF: code [FF_GAIN]: value: "sv << event->value << " aka "sv << util::hex(event->value).to_string_view();
              effect_it->gain = std::clamp(event->value, 0, 0xFFFF);

              break;
            }

            BOOST_LOG(debug) << "EV_FF: id ["sv << event->code << "]: value: "sv << event->value << " aka "sv << util::hex(event->value).to_string_view();

            if (event->value) {
              effect_it->activate(event->code);
            }
            else {
              effect_it->deactivate(event->code);
            }
            break;
          case EV_UINPUT:
            switch (event->code) {
              case UI_FF_UPLOAD: {
                uinput_ff_upload upload {};

                // *VERY* important, without this you break
                // the kernel and have to reboot due to dead
                // hanging process
                upload.request_id = event->value;

                ioctl(fd, UI_BEGIN_FF_UPLOAD, &upload);
                auto fg = util::fail_guard([&]() {
                  upload.retval = 0;
                  ioctl(fd, UI_END_FF_UPLOAD, &upload);
                });

                effect_it->upload(upload.effect);
              } break;
              case UI_FF_ERASE: {
                uinput_ff_erase erase {};

                // *VERY* important, without this you break
                // the kernel and have to reboot due to dead
                // hanging process
                erase.request_id = event->value;

                ioctl(fd, UI_BEGIN_FF_ERASE, &erase);
                auto fg = util::fail_guard([&]() {
                  erase.retval = 0;
                  ioctl(fd, UI_END_FF_ERASE, &erase);
                });

                effect_it->erase(erase.effect_id);
              } break;
            }
            break;
          default:
            BOOST_LOG(debug)
              << util::hex(event->type).to_string_view() << ": "sv
              << util::hex(event->code).to_string_view() << ": "sv
              << event->value << " aka "sv << util::hex(event->value).to_string_view();
        }
      }
    }
  }

  void
  broadcastRumble(safe::queue_t<mail_evdev_t> &rumble_queue_queue) {
    std::vector<effect_t> effects;
    std::vector<pollfd_t> polls;

    while (rumble_queue_queue.running()) {
      while (rumble_queue_queue.peek()) {
        auto dev_rumble_queue = rumble_queue_queue.pop();

        if (!dev_rumble_queue) {
          // rumble_queue_queue is no longer running
          return;
        }

        auto gamepadnr = std::get<0>(*dev_rumble_queue);
        auto dev = std::get<1>(*dev_rumble_queue);
        auto &rumble_queue = std::get<2>(*dev_rumble_queue);
        auto &pollfd = std::get<3>(*dev_rumble_queue);

        {
          auto effect_it = std::find_if(std::begin(effects), std::end(effects), [dev](auto &curr_effect) {
            return dev == curr_effect.dev;
          });

          if (effect_it != std::end(effects)) {
            auto poll_it = std::begin(polls) + (effect_it - std::begin(effects));

            polls.erase(poll_it);
            effects.erase(effect_it);

            BOOST_LOG(debug) << "Removed Gamepad device from notifications"sv;

            continue;
          }

          // There may be an attepmt to remove, that which not exists
          if (!rumble_queue) {
            BOOST_LOG(warning) << "Attempting to remove a gamepad device from notifications that isn't already registered"sv;
            continue;
          }
        }

        polls.emplace_back(std::move(pollfd));
        effects.emplace_back(gamepadnr, dev, std::move(rumble_queue));

        BOOST_LOG(debug) << "Added Gamepad device to notifications"sv;
      }

      if (polls.empty()) {
        std::this_thread::sleep_for(250ms);
      }
      else {
        rumbleIterate(effects, polls, 100ms);

        auto now = std::chrono::steady_clock::now();
        for (auto &effect : effects) {
          TUPLE_2D(old_weak, old_strong, effect.old_rumble);
          TUPLE_2D(weak, strong, effect.rumble(now));

          if (old_weak != weak || old_strong != strong) {
            BOOST_LOG(debug) << "Sending haptic feedback: lowfreq [0x"sv << util::hex(strong).to_string_view() << "]: highfreq [0x"sv << util::hex(weak).to_string_view() << ']';

            effect.rumble_queue->raise(gamepad_feedback_msg_t::make_rumble(effect.gamepadnr, strong, weak));
          }
        }
      }
    }
  }

  /**
   * @brief XTest absolute mouse move.
   * @param input The input_t instance to use.
   * @param x Absolute x position.
   * @param y Absolute y position.
   * @examples
   * x_abs_mouse(input, 0, 0);
   * @examples_end
   */
  static void
  x_abs_mouse(input_t &input, float x, float y) {
#ifdef SUNSHINE_BUILD_X11
    Display *xdisplay = ((input_raw_t *) input.get())->display;
    if (!xdisplay) {
      return;
    }
    x11::tst::FakeMotionEvent(xdisplay, -1, x, y, CurrentTime);
    x11::Flush(xdisplay);
#endif
  }

  util::point_t
  get_mouse_loc(input_t &input) {
#ifdef SUNSHINE_BUILD_X11
    Display *xdisplay = ((input_raw_t *) input.get())->display;
    if (!xdisplay) {
      return util::point_t {};
    }
    Window root, root_return, child_return;
    root = DefaultRootWindow(xdisplay);
    int root_x, root_y;
    int win_x, win_y;
    unsigned int mask_return;

    if (XQueryPointer(xdisplay, root, &root_return, &child_return, &root_x, &root_y, &win_x, &win_y, &mask_return)) {
      BOOST_LOG(debug)
        << "Pointer is at:"sv << std::endl
        << "  x: " << root_x << std::endl
        << "  y: " << root_y << std::endl;

      return util::point_t { (double) root_x, (double) root_y };
    }
    else {
      BOOST_LOG(debug) << "Unable to query x11 pointer"sv << std::endl;
    }
#else
    BOOST_LOG(debug) << "Unable to query wayland pointer"sv << std::endl;
#endif
    return util::point_t {};
  }

  /**
   * @brief Absolute mouse move.
   * @param input The input_t instance to use.
   * @param touch_port The touch_port instance to use.
   * @param x Absolute x position.
   * @param y Absolute y position.
   * @examples
   * abs_mouse(input, touch_port, 0, 0);
   * @examples_end
   */
  void
  abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y) {
    auto raw = (input_raw_t *) input.get();
    auto mouse_abs = raw->mouse_abs_input.get();
    if (!mouse_abs) {
      x_abs_mouse(input, x, y);
      return;
    }

    auto scaled_x = (int) std::lround((x + touch_port.offset_x) * ((float) target_touch_port.width / (float) touch_port.width));
    auto scaled_y = (int) std::lround((y + touch_port.offset_y) * ((float) target_touch_port.height / (float) touch_port.height));

    libevdev_uinput_write_event(mouse_abs, EV_ABS, ABS_X, scaled_x);
    libevdev_uinput_write_event(mouse_abs, EV_ABS, ABS_Y, scaled_y);
    libevdev_uinput_write_event(mouse_abs, EV_SYN, SYN_REPORT, 0);

    // Remember this was the last device we sent input on
    raw->last_mouse_device_used = mouse_abs;
    raw->last_mouse_device_buttons_down = &raw->mouse_abs_buttons_down;
  }

  /**
   * @brief XTest relative mouse move.
   * @param input The input_t instance to use.
   * @param deltaX Relative x position.
   * @param deltaY Relative y position.
   * @examples
   * x_move_mouse(input, 10, 10);  // Move mouse 10 pixels down and right
   * @examples_end
   */
  static void
  x_move_mouse(input_t &input, int deltaX, int deltaY) {
#ifdef SUNSHINE_BUILD_X11
    Display *xdisplay = ((input_raw_t *) input.get())->display;
    if (!xdisplay) {
      return;
    }
    x11::tst::FakeRelativeMotionEvent(xdisplay, deltaX, deltaY, CurrentTime);
    x11::Flush(xdisplay);
#endif
  }

  /**
   * @brief Relative mouse move.
   * @param input The input_t instance to use.
   * @param deltaX Relative x position.
   * @param deltaY Relative y position.
   * @examples
   * move_mouse(input, 10, 10); // Move mouse 10 pixels down and right
   * @examples_end
   */
  void
  move_mouse(input_t &input, int deltaX, int deltaY) {
    auto raw = (input_raw_t *) input.get();
    auto mouse_rel = raw->mouse_rel_input.get();
    if (!mouse_rel) {
      x_move_mouse(input, deltaX, deltaY);
      return;
    }

    if (deltaX) {
      libevdev_uinput_write_event(mouse_rel, EV_REL, REL_X, deltaX);
    }

    if (deltaY) {
      libevdev_uinput_write_event(mouse_rel, EV_REL, REL_Y, deltaY);
    }

    libevdev_uinput_write_event(mouse_rel, EV_SYN, SYN_REPORT, 0);

    // Remember this was the last device we sent input on
    raw->last_mouse_device_used = mouse_rel;
    raw->last_mouse_device_buttons_down = &raw->mouse_rel_buttons_down;
  }

  /**
   * @brief XTest mouse button press/release.
   * @param input The input_t instance to use.
   * @param button Which mouse button to emulate.
   * @param release Whether the event was a press (false) or a release (true)
   * @examples
   * x_button_mouse(input, 1, false); // Press left mouse button
   * @examples_end
   */
  static void
  x_button_mouse(input_t &input, int button, bool release) {
#ifdef SUNSHINE_BUILD_X11
    unsigned int x_button = 0;
    switch (button) {
      case BUTTON_LEFT:
        x_button = 1;
        break;
      case BUTTON_MIDDLE:
        x_button = 2;
        break;
      case BUTTON_RIGHT:
        x_button = 3;
        break;
      default:
        x_button = (button - 4) + 8;  // Button 4 (Moonlight) starts at index 8 (X11)
        break;
    }

    if (x_button < 1 || x_button > 31) {
      return;
    }

    Display *xdisplay = ((input_raw_t *) input.get())->display;
    if (!xdisplay) {
      return;
    }
    x11::tst::FakeButtonEvent(xdisplay, x_button, !release, CurrentTime);
    x11::Flush(xdisplay);
#endif
  }

  /**
   * @brief Mouse button press/release.
   * @param input The input_t instance to use.
   * @param button Which mouse button to emulate.
   * @param release Whether the event was a press (false) or a release (true)
   * @examples
   * button_mouse(input, 1, false);  // Press left mouse button
   * @examples_end
   */
  void
  button_mouse(input_t &input, int button, bool release) {
    auto raw = (input_raw_t *) input.get();

    // We mimic the Linux vmmouse driver here and prefer to send buttons
    // on the last mouse device we used. However, we make an exception
    // if it's a release event and the button is down on the other device.
    uinput_t::pointer chosen_mouse_dev = nullptr;
    uint8_t *chosen_mouse_dev_buttons_down = nullptr;
    if (release) {
      // Prefer to send the release on the mouse with the button down
      if (raw->mouse_rel_buttons_down & (1 << button)) {
        chosen_mouse_dev = raw->mouse_rel_input.get();
        chosen_mouse_dev_buttons_down = &raw->mouse_rel_buttons_down;
      }
      else if (raw->mouse_abs_buttons_down & (1 << button)) {
        chosen_mouse_dev = raw->mouse_abs_input.get();
        chosen_mouse_dev_buttons_down = &raw->mouse_abs_buttons_down;
      }
    }

    if (!chosen_mouse_dev) {
      if (raw->last_mouse_device_used) {
        // Prefer to use the last device we sent motion
        chosen_mouse_dev = raw->last_mouse_device_used;
        chosen_mouse_dev_buttons_down = raw->last_mouse_device_buttons_down;
      }
      else {
        // Send on the relative device if we have no preference yet
        chosen_mouse_dev = raw->mouse_rel_input.get();
        chosen_mouse_dev_buttons_down = &raw->mouse_rel_buttons_down;
      }
    }

    if (!chosen_mouse_dev) {
      x_button_mouse(input, button, release);
      return;
    }

    int btn_type;
    int scan;

    if (button == 1) {
      btn_type = BTN_LEFT;
      scan = 90001;
    }
    else if (button == 2) {
      btn_type = BTN_MIDDLE;
      scan = 90003;
    }
    else if (button == 3) {
      btn_type = BTN_RIGHT;
      scan = 90002;
    }
    else if (button == 4) {
      btn_type = BTN_SIDE;
      scan = 90004;
    }
    else {
      btn_type = BTN_EXTRA;
      scan = 90005;
    }

    libevdev_uinput_write_event(chosen_mouse_dev, EV_MSC, MSC_SCAN, scan);
    libevdev_uinput_write_event(chosen_mouse_dev, EV_KEY, btn_type, release ? 0 : 1);
    libevdev_uinput_write_event(chosen_mouse_dev, EV_SYN, SYN_REPORT, 0);

    if (release) {
      *chosen_mouse_dev_buttons_down &= ~(1 << button);
    }
    else {
      *chosen_mouse_dev_buttons_down |= (1 << button);
    }
  }

  /**
   * @brief XTest mouse scroll.
   * @param input The input_t instance to use.
   * @param distance How far to scroll.
   * @param button_pos Which mouse button to emulate for positive scroll.
   * @param button_neg Which mouse button to emulate for negative scroll.
   * @examples
   * x_scroll(input, 10, 4, 5);
   * @examples_end
   */
  static void
  x_scroll(input_t &input, int distance, int button_pos, int button_neg) {
#ifdef SUNSHINE_BUILD_X11
    Display *xdisplay = ((input_raw_t *) input.get())->display;
    if (!xdisplay) {
      return;
    }

    const int button = distance > 0 ? button_pos : button_neg;
    for (int i = 0; i < abs(distance); i++) {
      x11::tst::FakeButtonEvent(xdisplay, button, true, CurrentTime);
      x11::tst::FakeButtonEvent(xdisplay, button, false, CurrentTime);
    }
    x11::Flush(xdisplay);
#endif
  }

  /**
   * @brief Vertical mouse scroll.
   * @param input The input_t instance to use.
   * @param high_res_distance How far to scroll.
   * @examples
   * scroll(input, 1200);
   * @examples_end
   */
  void
  scroll(input_t &input, int high_res_distance) {
    auto raw = ((input_raw_t *) input.get());

    raw->accumulated_vscroll_delta += high_res_distance;
    int full_ticks = raw->accumulated_vscroll_delta / 120;

    // We mimic the Linux vmmouse driver and always send scroll events
    // via the relative pointing device for Xorg compatibility.
    auto mouse = raw->mouse_rel_input.get();
    if (mouse) {
      if (full_ticks) {
        libevdev_uinput_write_event(mouse, EV_REL, REL_WHEEL, full_ticks);
      }
      libevdev_uinput_write_event(mouse, EV_REL, REL_WHEEL_HI_RES, high_res_distance);
      libevdev_uinput_write_event(mouse, EV_SYN, SYN_REPORT, 0);
    }
    else if (full_ticks) {
      x_scroll(input, full_ticks, 4, 5);
    }

    raw->accumulated_vscroll_delta -= full_ticks * 120;
  }

  /**
   * @brief Horizontal mouse scroll.
   * @param input The input_t instance to use.
   * @param high_res_distance How far to scroll.
   * @examples
   * hscroll(input, 1200);
   * @examples_end
   */
  void
  hscroll(input_t &input, int high_res_distance) {
    auto raw = ((input_raw_t *) input.get());

    raw->accumulated_hscroll_delta += high_res_distance;
    int full_ticks = raw->accumulated_hscroll_delta / 120;

    // We mimic the Linux vmmouse driver and always send scroll events
    // via the relative pointing device for Xorg compatibility.
    auto mouse_rel = raw->mouse_rel_input.get();
    if (mouse_rel) {
      if (full_ticks) {
        libevdev_uinput_write_event(mouse_rel, EV_REL, REL_HWHEEL, full_ticks);
      }
      libevdev_uinput_write_event(mouse_rel, EV_REL, REL_HWHEEL_HI_RES, high_res_distance);
      libevdev_uinput_write_event(mouse_rel, EV_SYN, SYN_REPORT, 0);
    }
    else if (full_ticks) {
      x_scroll(input, full_ticks, 6, 7);
    }

    raw->accumulated_hscroll_delta -= full_ticks * 120;
  }

  static keycode_t
  keysym(std::uint16_t modcode) {
    if (modcode <= keycodes.size()) {
      return keycodes[modcode];
    }

    return {};
  }

  /**
   * @brief XTest keyboard emulation.
   * @param input The input_t instance to use.
   * @param modcode The moonlight key code.
   * @param release Whether the event was a press (false) or a release (true).
   * @param flags SS_KBE_FLAG_* values.
   * @examples
   * x_keyboard(input, 0x5A, false, 0);  // Press Z
   * @examples_end
   */
  static void
  x_keyboard(input_t &input, uint16_t modcode, bool release, uint8_t flags) {
#ifdef SUNSHINE_BUILD_X11
    auto keycode = keysym(modcode);
    if (keycode.keysym == UNKNOWN) {
      return;
    }

    Display *xdisplay = ((input_raw_t *) input.get())->display;
    if (!xdisplay) {
      return;
    }

    const auto keycode_x = XKeysymToKeycode(xdisplay, keycode.keysym);
    if (keycode_x == 0) {
      return;
    }

    x11::tst::FakeKeyEvent(xdisplay, keycode_x, !release, CurrentTime);
    x11::Flush(xdisplay);
#endif
  }

  /**
   * @brief Keyboard emulation.
   * @param input The input_t instance to use.
   * @param modcode The moonlight key code.
   * @param release Whether the event was a press (false) or a release (true).
   * @param flags SS_KBE_FLAG_* values.
   * @examples
   * keyboard(input, 0x5A, false, 0);  // Press Z
   * @examples_end
   */
  void
  keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags) {
    auto keyboard = ((input_raw_t *) input.get())->keyboard_input.get();
    if (!keyboard) {
      x_keyboard(input, modcode, release, flags);
      return;
    }

    auto keycode = keysym(modcode);
    if (keycode.keycode == UNKNOWN) {
      return;
    }

    if (keycode.scancode != UNKNOWN) {
      libevdev_uinput_write_event(keyboard, EV_MSC, MSC_SCAN, keycode.scancode);
    }

    libevdev_uinput_write_event(keyboard, EV_KEY, keycode.keycode, release ? 0 : 1);
    libevdev_uinput_write_event(keyboard, EV_SYN, SYN_REPORT, 0);
  }

  void
  keyboard_ev(libevdev_uinput *keyboard, int linux_code, int event_code = 1) {
    libevdev_uinput_write_event(keyboard, EV_KEY, linux_code, event_code);
    libevdev_uinput_write_event(keyboard, EV_SYN, SYN_REPORT, 0);
  }

  /**
   * Takes an UTF-32 encoded string and returns a hex string representation of the bytes (uppercase)
   *
   * ex: ['👱'] = "1F471" // see UTF encoding at https://www.compart.com/en/unicode/U+1F471
   *
   * adapted from: https://stackoverflow.com/a/7639754
   */
  std::string
  to_hex(const std::basic_string<char32_t> &str) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &ch : str) {
      ss << static_cast<uint_least32_t>(ch);
    }

    std::string hex_unicode(ss.str());
    std::transform(hex_unicode.begin(), hex_unicode.end(), hex_unicode.begin(), ::toupper);
    return hex_unicode;
  }

  /**
   * Here we receive a single UTF-8 encoded char at a time,
   * the trick is to convert it to UTF-32 then send CTRL+SHIFT+U+{HEXCODE} in order to produce any
   * unicode character, see: https://en.wikipedia.org/wiki/Unicode_input
   *
   * ex:
   * - when receiving UTF-8 [0xF0 0x9F 0x91 0xB1] (which is '👱')
   * - we'll convert it to UTF-32 [0x1F471]
   * - then type: CTRL+SHIFT+U+1F471
   * see the conversion at: https://www.compart.com/en/unicode/U+1F471
   */
  void
  unicode(input_t &input, char *utf8, int size) {
    auto kb = ((input_raw_t *) input.get())->keyboard_input.get();
    if (!kb) {
      return;
    }

    /* Reading input text as UTF-8 */
    auto utf8_str = boost::locale::conv::to_utf<wchar_t>(utf8, utf8 + size, "UTF-8");
    /* Converting to UTF-32 */
    auto utf32_str = boost::locale::conv::utf_to_utf<char32_t>(utf8_str);
    /* To HEX string */
    auto hex_unicode = to_hex(utf32_str);
    BOOST_LOG(debug) << "Unicode, typing U+"sv << hex_unicode;

    /* pressing <CTRL> + <SHIFT> + U */
    keyboard_ev(kb, KEY_LEFTCTRL, 1);
    keyboard_ev(kb, KEY_LEFTSHIFT, 1);
    keyboard_ev(kb, KEY_U, 1);
    keyboard_ev(kb, KEY_U, 0);

    /* input each HEX character */
    for (auto &ch : hex_unicode) {
      auto key_str = "KEY_"s + ch;
      auto keycode = libevdev_event_code_from_name(EV_KEY, key_str.c_str());
      if (keycode == -1) {
        BOOST_LOG(warning) << "Unicode, unable to find keycode for: "sv << ch;
      }
      else {
        keyboard_ev(kb, keycode, 1);
        keyboard_ev(kb, keycode, 0);
      }
    }

    /* releasing <SHIFT> and <CTRL> */
    keyboard_ev(kb, KEY_LEFTSHIFT, 0);
    keyboard_ev(kb, KEY_LEFTCTRL, 0);
  }

  int
  alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    return ((input_raw_t *) input.get())->alloc_gamepad(id, metadata, std::move(feedback_queue));
  }

  void
  free_gamepad(input_t &input, int nr) {
    ((input_raw_t *) input.get())->clear_gamepad(nr);
  }

  void
  gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state) {
    TUPLE_2D_REF(uinput, gamepad_state_old, ((input_raw_t *) input.get())->gamepads[nr]);

    auto bf = gamepad_state.buttonFlags ^ gamepad_state_old.buttonFlags;
    auto bf_new = gamepad_state.buttonFlags;

    if (bf) {
      // up pressed == -1, down pressed == 1, else 0
      if ((DPAD_UP | DPAD_DOWN) & bf) {
        int button_state = bf_new & DPAD_UP ? -1 : (bf_new & DPAD_DOWN ? 1 : 0);

        libevdev_uinput_write_event(uinput.get(), EV_ABS, ABS_HAT0Y, button_state);
      }

      if ((DPAD_LEFT | DPAD_RIGHT) & bf) {
        int button_state = bf_new & DPAD_LEFT ? -1 : (bf_new & DPAD_RIGHT ? 1 : 0);

        libevdev_uinput_write_event(uinput.get(), EV_ABS, ABS_HAT0X, button_state);
      }

      if (START & bf) libevdev_uinput_write_event(uinput.get(), EV_KEY, BTN_START, bf_new & START ? 1 : 0);
      if (BACK & bf) libevdev_uinput_write_event(uinput.get(), EV_KEY, BTN_SELECT, bf_new & BACK ? 1 : 0);
      if (LEFT_STICK & bf) libevdev_uinput_write_event(uinput.get(), EV_KEY, BTN_THUMBL, bf_new & LEFT_STICK ? 1 : 0);
      if (RIGHT_STICK & bf) libevdev_uinput_write_event(uinput.get(), EV_KEY, BTN_THUMBR, bf_new & RIGHT_STICK ? 1 : 0);
      if (LEFT_BUTTON & bf) libevdev_uinput_write_event(uinput.get(), EV_KEY, BTN_TL, bf_new & LEFT_BUTTON ? 1 : 0);
      if (RIGHT_BUTTON & bf) libevdev_uinput_write_event(uinput.get(), EV_KEY, BTN_TR, bf_new & RIGHT_BUTTON ? 1 : 0);
      if ((HOME | MISC_BUTTON) & bf) libevdev_uinput_write_event(uinput.get(), EV_KEY, BTN_MODE, bf_new & (HOME | MISC_BUTTON) ? 1 : 0);
      if (A & bf) libevdev_uinput_write_event(uinput.get(), EV_KEY, BTN_SOUTH, bf_new & A ? 1 : 0);
      if (B & bf) libevdev_uinput_write_event(uinput.get(), EV_KEY, BTN_EAST, bf_new & B ? 1 : 0);
      if (X & bf) libevdev_uinput_write_event(uinput.get(), EV_KEY, BTN_NORTH, bf_new & X ? 1 : 0);
      if (Y & bf) libevdev_uinput_write_event(uinput.get(), EV_KEY, BTN_WEST, bf_new & Y ? 1 : 0);
    }

    if (gamepad_state_old.lt != gamepad_state.lt) {
      libevdev_uinput_write_event(uinput.get(), EV_ABS, ABS_Z, gamepad_state.lt);
    }

    if (gamepad_state_old.rt != gamepad_state.rt) {
      libevdev_uinput_write_event(uinput.get(), EV_ABS, ABS_RZ, gamepad_state.rt);
    }

    if (gamepad_state_old.lsX != gamepad_state.lsX) {
      libevdev_uinput_write_event(uinput.get(), EV_ABS, ABS_X, gamepad_state.lsX);
    }

    if (gamepad_state_old.lsY != gamepad_state.lsY) {
      libevdev_uinput_write_event(uinput.get(), EV_ABS, ABS_Y, -gamepad_state.lsY);
    }

    if (gamepad_state_old.rsX != gamepad_state.rsX) {
      libevdev_uinput_write_event(uinput.get(), EV_ABS, ABS_RX, gamepad_state.rsX);
    }

    if (gamepad_state_old.rsY != gamepad_state.rsY) {
      libevdev_uinput_write_event(uinput.get(), EV_ABS, ABS_RY, -gamepad_state.rsY);
    }

    gamepad_state_old = gamepad_state;
    libevdev_uinput_write_event(uinput.get(), EV_SYN, SYN_REPORT, 0);
  }

  constexpr auto NUM_TOUCH_SLOTS = 10;
  constexpr auto DISTANCE_MAX = 1024;
  constexpr auto PRESSURE_MAX = 4096;
  constexpr int64_t INVALID_TRACKING_ID = -1;

  // HACK: Contacts with very small pressure values get discarded by libinput, but
  // we assume that the client has already excluded such errant touches. We enforce
  // a minimum pressure value to prevent our touches from being discarded.
  constexpr auto PRESSURE_MIN = 0.10f;

  struct client_input_raw_t: public client_input_t {
    client_input_raw_t(input_t &input) {
      global = (input_raw_t *) input.get();
      touch_slots.fill(INVALID_TRACKING_ID);
    }

    input_raw_t *global;

    // Device state and handles for pen and touch input must be stored in the per-client
    // input context, because each connected client may be sending their own independent
    // pen/touch events. To maintain separation, we expose separate pen and touch devices
    // for each client.

    // Mapping of ABS_MT_SLOT/ABS_MT_TRACKING_ID -> pointerId
    std::array<int64_t, NUM_TOUCH_SLOTS> touch_slots;
    uinput_t touch_input;
    uinput_t pen_input;
  };

  /**
   * @brief Allocates a context to store per-client input data.
   * @param input The global input context.
   * @return A unique pointer to a per-client input data context.
   */
  std::unique_ptr<client_input_t>
  allocate_client_input_context(input_t &input) {
    return std::make_unique<client_input_raw_t>(input);
  }

  /**
   * @brief Retrieves the slot index for a given pointer ID.
   * @param input The client-specific input context.
   * @param pointerId The pointer ID sent from the client.
   * @return Slot index or -1 if not found.
   */
  int
  slot_index_by_pointer_id(client_input_raw_t *input, uint32_t pointerId) {
    for (int i = 0; i < input->touch_slots.size(); i++) {
      if (input->touch_slots[i] == pointerId) {
        return i;
      }
    }
    return -1;
  }

  /**
   * @brief Reserves a slot index for a new pointer ID.
   * @param input The client-specific input context.
   * @param pointerId The pointer ID sent from the client.
   * @return Slot index or -1 if no unallocated slots remain.
   */
  int
  allocate_slot_index_for_pointer_id(client_input_raw_t *input, uint32_t pointerId) {
    int i = slot_index_by_pointer_id(input, pointerId);
    if (i >= 0) {
      BOOST_LOG(warning) << "Pointer "sv << pointerId << " already down. Did the client drop an up/cancel event?"sv;
      return i;
    }

    for (int i = 0; i < input->touch_slots.size(); i++) {
      if (input->touch_slots[i] == INVALID_TRACKING_ID) {
        input->touch_slots[i] = pointerId;
        return i;
      }
    }

    return -1;
  }

  /**
   * @brief Sends a touch event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param touch The touch event.
   */
  void
  touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch) {
    auto raw = (client_input_raw_t *) input;

    if (!raw->touch_input) {
      int err = libevdev_uinput_create_from_device(raw->global->touchscreen_dev.get(), LIBEVDEV_UINPUT_OPEN_MANAGED, &raw->touch_input);
      if (err) {
        BOOST_LOG(error) << "Could not create Sunshine Touchscreen: "sv << strerror(-err);
        return;
      }
    }

    auto touch_input = raw->touch_input.get();

    float pressure = std::max(PRESSURE_MIN, touch.pressureOrDistance);

    if (touch.eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
      for (int i = 0; i < raw->touch_slots.size(); i++) {
        libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_SLOT, i);
        libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_TRACKING_ID, -1);
      }
      raw->touch_slots.fill(INVALID_TRACKING_ID);

      libevdev_uinput_write_event(touch_input, EV_KEY, BTN_TOUCH, 0);
      libevdev_uinput_write_event(touch_input, EV_ABS, ABS_PRESSURE, 0);
      libevdev_uinput_write_event(touch_input, EV_SYN, SYN_REPORT, 0);
      return;
    }

    if (touch.eventType == LI_TOUCH_EVENT_CANCEL) {
      // Stop tracking this slot
      auto slot_index = slot_index_by_pointer_id(raw, touch.pointerId);
      if (slot_index >= 0) {
        libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_SLOT, slot_index);
        libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_TRACKING_ID, -1);

        raw->touch_slots[slot_index] = INVALID_TRACKING_ID;

        // Raise BTN_TOUCH if no touches are down
        if (std::all_of(raw->touch_slots.cbegin(), raw->touch_slots.cend(),
              [](uint64_t pointer_id) { return pointer_id == INVALID_TRACKING_ID; })) {
          libevdev_uinput_write_event(touch_input, EV_KEY, BTN_TOUCH, 0);

          // This may have been the final slot down which was also being emulated
          // through the single-touch axes. Reset ABS_PRESSURE to ensure code that
          // uses ABS_PRESSURE instead of BTN_TOUCH will work properly.
          libevdev_uinput_write_event(touch_input, EV_ABS, ABS_PRESSURE, 0);
        }
      }
    }
    else if (touch.eventType == LI_TOUCH_EVENT_DOWN ||
             touch.eventType == LI_TOUCH_EVENT_MOVE ||
             touch.eventType == LI_TOUCH_EVENT_UP) {
      int slot_index;
      if (touch.eventType == LI_TOUCH_EVENT_DOWN) {
        // Allocate a new slot for this new touch
        slot_index = allocate_slot_index_for_pointer_id(raw, touch.pointerId);
        if (slot_index < 0) {
          BOOST_LOG(error) << "No unused pointer entries! Cancelling all active touches!"sv;

          for (int i = 0; i < raw->touch_slots.size(); i++) {
            libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_SLOT, i);
            libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_TRACKING_ID, -1);
          }
          raw->touch_slots.fill(INVALID_TRACKING_ID);

          libevdev_uinput_write_event(touch_input, EV_KEY, BTN_TOUCH, 0);
          libevdev_uinput_write_event(touch_input, EV_ABS, ABS_PRESSURE, 0);
          libevdev_uinput_write_event(touch_input, EV_SYN, SYN_REPORT, 0);

          // All slots are clear, so this should never fail on the second try
          slot_index = allocate_slot_index_for_pointer_id(raw, touch.pointerId);
          assert(slot_index >= 0);
        }
      }
      else {
        // Lookup the slot of the previous touch with this pointer ID
        slot_index = slot_index_by_pointer_id(raw, touch.pointerId);
        if (slot_index < 0) {
          BOOST_LOG(warning) << "Pointer "sv << touch.pointerId << " is not down. Did the client drop a down event?"sv;
          return;
        }
      }

      libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_SLOT, slot_index);

      if (touch.eventType == LI_TOUCH_EVENT_UP) {
        // Stop tracking this touch
        libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_TRACKING_ID, -1);
        raw->touch_slots[slot_index] = INVALID_TRACKING_ID;

        // Raise BTN_TOUCH if no touches are down
        if (std::all_of(raw->touch_slots.cbegin(), raw->touch_slots.cend(),
              [](uint64_t pointer_id) { return pointer_id == INVALID_TRACKING_ID; })) {
          libevdev_uinput_write_event(touch_input, EV_KEY, BTN_TOUCH, 0);

          // This may have been the final slot down which was also being emulated
          // through the single-touch axes. Reset ABS_PRESSURE to ensure code that
          // uses ABS_PRESSURE instead of BTN_TOUCH will work properly.
          libevdev_uinput_write_event(touch_input, EV_ABS, ABS_PRESSURE, 0);
        }
      }
      else {
        float x = touch.x * touch_port.width;
        float y = touch.y * touch_port.height;

        auto scaled_x = (int) std::lround((x + touch_port.offset_x) * ((float) target_touch_port.width / (float) touch_port.width));
        auto scaled_y = (int) std::lround((y + touch_port.offset_y) * ((float) target_touch_port.height / (float) touch_port.height));

        libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_TRACKING_ID, slot_index);
        libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_POSITION_X, scaled_x);
        libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_POSITION_Y, scaled_y);

        if (touch.pressureOrDistance) {
          libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_PRESSURE, PRESSURE_MAX * pressure);
        }
        else if (touch.eventType == LI_TOUCH_EVENT_DOWN) {
          // Always report some moderate pressure value when down
          libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_PRESSURE, PRESSURE_MAX / 2);
        }

        if (touch.rotation != LI_ROT_UNKNOWN) {
          // Convert our 0..360 range to -90..90 relative to Y axis
          int adjusted_angle = touch.rotation;

          if (touch.rotation > 90 && touch.rotation < 270) {
            // Lower hemisphere
            adjusted_angle = 180 - adjusted_angle;
          }

          // Wrap the value if it's out of range
          if (adjusted_angle > 90) {
            adjusted_angle -= 360;
          }
          else if (adjusted_angle < -90) {
            adjusted_angle += 360;
          }

          libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_ORIENTATION, adjusted_angle);
        }

        if (touch.contactAreaMajor) {
          // Contact area comes from the input core scaled to the provided touch_port,
          // however we need it rescaled to target_touch_port instead.
          auto target_scaled_contact_area = input::scale_client_contact_area(
            { touch.contactAreaMajor * 65535.f, touch.contactAreaMinor * 65535.f },
            touch.rotation,
            { target_touch_port.width / (touch_port.width * 65535.f),
              target_touch_port.height / (touch_port.height * 65535.f) });

          libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_TOUCH_MAJOR, target_scaled_contact_area.first);

          // scale_client_contact_area() will treat the contact area as circular (major == minor)
          // if the minor axis wasn't specified, so we unconditionally report ABS_MT_TOUCH_MINOR.
          libevdev_uinput_write_event(touch_input, EV_ABS, ABS_MT_TOUCH_MINOR, target_scaled_contact_area.second);
        }

        // If this slot is the first active one, send our data through the single touch axes as well
        for (int i = 0; i <= slot_index; i++) {
          if (raw->touch_slots[i] != INVALID_TRACKING_ID) {
            if (i == slot_index) {
              libevdev_uinput_write_event(touch_input, EV_ABS, ABS_X, scaled_x);
              libevdev_uinput_write_event(touch_input, EV_ABS, ABS_Y, scaled_y);
              if (touch.pressureOrDistance) {
                libevdev_uinput_write_event(touch_input, EV_ABS, ABS_PRESSURE, PRESSURE_MAX * pressure);
              }
              else if (touch.eventType == LI_TOUCH_EVENT_DOWN) {
                libevdev_uinput_write_event(touch_input, EV_ABS, ABS_PRESSURE, PRESSURE_MAX / 2);
              }
            }
            break;
          }
        }
      }

      libevdev_uinput_write_event(touch_input, EV_SYN, SYN_REPORT, 0);
    }
  }

  /**
   * @brief Sends a pen event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param pen The pen event.
   */
  void
  pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen) {
    auto raw = (client_input_raw_t *) input;

    if (!raw->pen_input) {
      int err = libevdev_uinput_create_from_device(raw->global->pen_dev.get(), LIBEVDEV_UINPUT_OPEN_MANAGED, &raw->pen_input);
      if (err) {
        BOOST_LOG(error) << "Could not create Sunshine Pen: "sv << strerror(-err);
        return;
      }
    }

    auto pen_input = raw->pen_input.get();

    float x = pen.x * touch_port.width;
    float y = pen.y * touch_port.height;
    float pressure = std::max(PRESSURE_MIN, pen.pressureOrDistance);

    auto scaled_x = (int) std::lround((x + touch_port.offset_x) * ((float) target_touch_port.width / (float) touch_port.width));
    auto scaled_y = (int) std::lround((y + touch_port.offset_y) * ((float) target_touch_port.height / (float) touch_port.height));

    // First, process location updates for applicable events
    switch (pen.eventType) {
      case LI_TOUCH_EVENT_HOVER:
        libevdev_uinput_write_event(pen_input, EV_ABS, ABS_X, scaled_x);
        libevdev_uinput_write_event(pen_input, EV_ABS, ABS_Y, scaled_y);

        libevdev_uinput_write_event(pen_input, EV_ABS, ABS_PRESSURE, 0);
        if (pen.pressureOrDistance) {
          libevdev_uinput_write_event(pen_input, EV_ABS, ABS_DISTANCE, DISTANCE_MAX * pen.pressureOrDistance);
        }
        else {
          // Always report some moderate distance value when hovering to ensure hovering
          // can be detected properly by code that uses ABS_DISTANCE.
          libevdev_uinput_write_event(pen_input, EV_ABS, ABS_DISTANCE, DISTANCE_MAX / 2);
        }
        break;

      case LI_TOUCH_EVENT_DOWN:
        libevdev_uinput_write_event(pen_input, EV_ABS, ABS_X, scaled_x);
        libevdev_uinput_write_event(pen_input, EV_ABS, ABS_Y, scaled_y);

        libevdev_uinput_write_event(pen_input, EV_ABS, ABS_DISTANCE, 0);
        libevdev_uinput_write_event(pen_input, EV_ABS, ABS_PRESSURE, PRESSURE_MAX * pressure);
        break;

      case LI_TOUCH_EVENT_UP:
        libevdev_uinput_write_event(pen_input, EV_ABS, ABS_X, scaled_x);
        libevdev_uinput_write_event(pen_input, EV_ABS, ABS_Y, scaled_y);

        libevdev_uinput_write_event(pen_input, EV_ABS, ABS_PRESSURE, 0);
        break;

      case LI_TOUCH_EVENT_MOVE:
        libevdev_uinput_write_event(pen_input, EV_ABS, ABS_X, scaled_x);
        libevdev_uinput_write_event(pen_input, EV_ABS, ABS_Y, scaled_y);

        // Update the pressure value if it's present, otherwise leave the default/previous value alone
        if (pen.pressureOrDistance) {
          libevdev_uinput_write_event(pen_input, EV_ABS, ABS_PRESSURE, PRESSURE_MAX * pressure);
        }
        break;
    }

    if (pen.contactAreaMajor) {
      // Contact area comes from the input core scaled to the provided touch_port,
      // however we need it rescaled to target_touch_port instead.
      auto target_scaled_contact_area = input::scale_client_contact_area(
        { pen.contactAreaMajor * 65535.f, pen.contactAreaMinor * 65535.f },
        pen.rotation,
        { target_touch_port.width / (touch_port.width * 65535.f),
          target_touch_port.height / (touch_port.height * 65535.f) });

      // ABS_TOOL_WIDTH assumes a circular tool, so we just report the major axis
      libevdev_uinput_write_event(pen_input, EV_ABS, ABS_TOOL_WIDTH, target_scaled_contact_area.first);
    }

    // We require rotation and tilt to perform the conversion to X and Y tilt angles
    if (pen.tilt != LI_TILT_UNKNOWN && pen.rotation != LI_ROT_UNKNOWN) {
      auto rotation_rads = pen.rotation * (M_PI / 180.f);
      auto tilt_rads = pen.tilt * (M_PI / 180.f);
      auto r = std::sin(tilt_rads);
      auto z = std::cos(tilt_rads);

      // Convert polar coordinates into X and Y tilt angles
      libevdev_uinput_write_event(pen_input, EV_ABS, ABS_TILT_X, std::atan2(std::sin(-rotation_rads) * r, z) * 180.f / M_PI);
      libevdev_uinput_write_event(pen_input, EV_ABS, ABS_TILT_Y, std::atan2(std::cos(-rotation_rads) * r, z) * 180.f / M_PI);
    }

    // Don't update tool type if we're cancelling or ending a touch/hover
    if (pen.eventType != LI_TOUCH_EVENT_CANCEL &&
        pen.eventType != LI_TOUCH_EVENT_CANCEL_ALL &&
        pen.eventType != LI_TOUCH_EVENT_HOVER_LEAVE &&
        pen.eventType != LI_TOUCH_EVENT_UP) {
      // Update the tool type if it is known
      switch (pen.toolType) {
        default:
          // We need to have _some_ tool type set, otherwise there's no way to know a tool is in
          // range when hovering. If we don't know the type of tool, let's assume it's a pen.
          if (pen.eventType != LI_TOUCH_EVENT_DOWN && pen.eventType != LI_TOUCH_EVENT_HOVER) {
            break;
          }
          // fall-through
        case LI_TOOL_TYPE_PEN:
          libevdev_uinput_write_event(pen_input, EV_KEY, BTN_TOOL_RUBBER, 0);
          libevdev_uinput_write_event(pen_input, EV_KEY, BTN_TOOL_PEN, 1);
          break;
        case LI_TOOL_TYPE_ERASER:
          libevdev_uinput_write_event(pen_input, EV_KEY, BTN_TOOL_PEN, 0);
          libevdev_uinput_write_event(pen_input, EV_KEY, BTN_TOOL_RUBBER, 1);
          break;
      }
    }

    // Next, process touch state changes
    switch (pen.eventType) {
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_CANCEL_ALL:
      case LI_TOUCH_EVENT_HOVER_LEAVE:
      case LI_TOUCH_EVENT_UP:
        libevdev_uinput_write_event(pen_input, EV_KEY, BTN_TOUCH, 0);

        // Leaving hover range is detected by all BTN_TOOL_* being cleared
        libevdev_uinput_write_event(pen_input, EV_KEY, BTN_TOOL_PEN, 0);
        libevdev_uinput_write_event(pen_input, EV_KEY, BTN_TOOL_RUBBER, 0);
        break;

      case LI_TOUCH_EVENT_DOWN:
        libevdev_uinput_write_event(pen_input, EV_KEY, BTN_TOUCH, 1);
        break;
    }

    // Finally, process pen buttons
    libevdev_uinput_write_event(pen_input, EV_KEY, BTN_STYLUS, !!(pen.penButtons & LI_PEN_BUTTON_PRIMARY));
    libevdev_uinput_write_event(pen_input, EV_KEY, BTN_STYLUS2, !!(pen.penButtons & LI_PEN_BUTTON_SECONDARY));
    libevdev_uinput_write_event(pen_input, EV_KEY, BTN_STYLUS3, !!(pen.penButtons & LI_PEN_BUTTON_TERTIARY));

    libevdev_uinput_write_event(pen_input, EV_SYN, SYN_REPORT, 0);
  }

  /**
   * @brief Sends a gamepad touch event to the OS.
   * @param input The global input context.
   * @param touch The touch event.
   */
  void
  gamepad_touch(input_t &input, const gamepad_touch_t &touch) {
    // Unimplemented feature - platform_caps::controller_touch
  }

  /**
   * @brief Sends a gamepad motion event to the OS.
   * @param input The global input context.
   * @param motion The motion event.
   */
  void
  gamepad_motion(input_t &input, const gamepad_motion_t &motion) {
    // Unimplemented
  }

  /**
   * @brief Sends a gamepad battery event to the OS.
   * @param input The global input context.
   * @param battery The battery event.
   */
  void
  gamepad_battery(input_t &input, const gamepad_battery_t &battery) {
    // Unimplemented
  }

  /**
   * @brief Initialize a new keyboard and return it.
   * @examples
   * auto my_keyboard = keyboard();
   * @examples_end
   */
  evdev_t
  keyboard() {
    evdev_t dev { libevdev_new() };

    libevdev_set_uniq(dev.get(), "Sunshine Keyboard");
    libevdev_set_id_product(dev.get(), 0xDEAD);
    libevdev_set_id_vendor(dev.get(), 0xBEEF);
    libevdev_set_id_bustype(dev.get(), 0x3);
    libevdev_set_id_version(dev.get(), 0x111);
    libevdev_set_name(dev.get(), "Keyboard passthrough");

    libevdev_enable_event_type(dev.get(), EV_KEY);
    for (const auto &keycode : keycodes) {
      libevdev_enable_event_code(dev.get(), EV_KEY, keycode.keycode, nullptr);
    }
    libevdev_enable_event_type(dev.get(), EV_MSC);
    libevdev_enable_event_code(dev.get(), EV_MSC, MSC_SCAN, nullptr);

    return dev;
  }

  /**
   * @brief Initialize a new `uinput` virtual relative mouse and return it.
   * @examples
   * auto my_mouse = mouse_rel();
   * @examples_end
   */
  evdev_t
  mouse_rel() {
    evdev_t dev { libevdev_new() };

    libevdev_set_uniq(dev.get(), "Sunshine Mouse (Rel)");
    libevdev_set_id_product(dev.get(), 0x4038);
    libevdev_set_id_vendor(dev.get(), 0x46D);
    libevdev_set_id_bustype(dev.get(), 0x3);
    libevdev_set_id_version(dev.get(), 0x111);
    libevdev_set_name(dev.get(), "Logitech Wireless Mouse PID:4038");

    libevdev_enable_event_type(dev.get(), EV_KEY);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_LEFT, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_RIGHT, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_MIDDLE, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_SIDE, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_EXTRA, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_FORWARD, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_BACK, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_TASK, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, 280, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, 281, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, 282, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, 283, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, 284, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, 285, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, 286, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, 287, nullptr);

    libevdev_enable_event_type(dev.get(), EV_REL);
    libevdev_enable_event_code(dev.get(), EV_REL, REL_X, nullptr);
    libevdev_enable_event_code(dev.get(), EV_REL, REL_Y, nullptr);
    libevdev_enable_event_code(dev.get(), EV_REL, REL_WHEEL, nullptr);
    libevdev_enable_event_code(dev.get(), EV_REL, REL_WHEEL_HI_RES, nullptr);
    libevdev_enable_event_code(dev.get(), EV_REL, REL_HWHEEL, nullptr);
    libevdev_enable_event_code(dev.get(), EV_REL, REL_HWHEEL_HI_RES, nullptr);

    libevdev_enable_event_type(dev.get(), EV_MSC);
    libevdev_enable_event_code(dev.get(), EV_MSC, MSC_SCAN, nullptr);

    return dev;
  }

  /**
   * @brief Initialize a new `uinput` virtual absolute mouse and return it.
   * @examples
   * auto my_mouse = mouse_abs();
   * @examples_end
   */
  evdev_t
  mouse_abs() {
    evdev_t dev { libevdev_new() };

    libevdev_set_uniq(dev.get(), "Sunshine Mouse (Abs)");
    libevdev_set_id_product(dev.get(), 0xDEAD);
    libevdev_set_id_vendor(dev.get(), 0xBEEF);
    libevdev_set_id_bustype(dev.get(), 0x3);
    libevdev_set_id_version(dev.get(), 0x111);
    libevdev_set_name(dev.get(), "Mouse passthrough");

    libevdev_enable_property(dev.get(), INPUT_PROP_DIRECT);

    libevdev_enable_event_type(dev.get(), EV_KEY);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_LEFT, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_RIGHT, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_MIDDLE, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_SIDE, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_EXTRA, nullptr);

    libevdev_enable_event_type(dev.get(), EV_MSC);
    libevdev_enable_event_code(dev.get(), EV_MSC, MSC_SCAN, nullptr);

    input_absinfo absx {
      0,
      0,
      target_touch_port.width,
      1,
      0,
      28
    };

    input_absinfo absy {
      0,
      0,
      target_touch_port.height,
      1,
      0,
      28
    };
    libevdev_enable_event_type(dev.get(), EV_ABS);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_X, &absx);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_Y, &absy);

    return dev;
  }

  /**
   * @brief Initialize a new `uinput` virtual touchscreen and return it.
   * @examples
   * auto my_touchscreen = touchscreen();
   * @examples_end
   */
  evdev_t
  touchscreen() {
    evdev_t dev { libevdev_new() };

    libevdev_set_uniq(dev.get(), "Sunshine Touchscreen");
    libevdev_set_id_product(dev.get(), 0xDEAD);
    libevdev_set_id_vendor(dev.get(), 0xBEEF);
    libevdev_set_id_bustype(dev.get(), 0x3);
    libevdev_set_id_version(dev.get(), 0x111);
    libevdev_set_name(dev.get(), "Touch passthrough");

    libevdev_enable_property(dev.get(), INPUT_PROP_DIRECT);

    constexpr auto RESOLUTION = 28;

    input_absinfo abs_slot {
      0,
      0,
      NUM_TOUCH_SLOTS - 1,
      0,
      0,
      0
    };

    input_absinfo abs_tracking_id {
      0,
      0,
      NUM_TOUCH_SLOTS - 1,
      0,
      0,
      0
    };

    input_absinfo abs_x {
      0,
      0,
      target_touch_port.width,
      1,
      0,
      RESOLUTION
    };

    input_absinfo abs_y {
      0,
      0,
      target_touch_port.height,
      1,
      0,
      RESOLUTION
    };

    input_absinfo abs_pressure {
      0,
      0,
      PRESSURE_MAX,
      0,
      0,
      0
    };

    // Degrees of a half revolution
    input_absinfo abs_orientation {
      0,
      -90,
      90,
      0,
      0,
      0
    };

    // Fractions of the full diagonal
    input_absinfo abs_contact_area {
      0,
      0,
      (__s32) std::sqrt(std::pow(target_touch_port.width, 2) + std::pow(target_touch_port.height, 2)),
      1,
      0,
      RESOLUTION
    };

    libevdev_enable_event_type(dev.get(), EV_ABS);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_X, &abs_x);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_Y, &abs_y);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_PRESSURE, &abs_pressure);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_MT_SLOT, &abs_slot);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_MT_TRACKING_ID, &abs_tracking_id);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_MT_POSITION_X, &abs_x);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_MT_POSITION_Y, &abs_y);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_MT_PRESSURE, &abs_pressure);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_MT_ORIENTATION, &abs_orientation);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_MT_TOUCH_MAJOR, &abs_contact_area);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_MT_TOUCH_MINOR, &abs_contact_area);

    libevdev_enable_event_type(dev.get(), EV_KEY);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_TOUCH, nullptr);

    return dev;
  }

  /**
   * @brief Initialize a new `uinput` virtual pen pad and return it.
   * @examples
   * auto my_penpad = penpad();
   * @examples_end
   */
  evdev_t
  penpad() {
    evdev_t dev { libevdev_new() };

    libevdev_set_uniq(dev.get(), "Sunshine Pen");
    libevdev_set_id_product(dev.get(), 0xDEAD);
    libevdev_set_id_vendor(dev.get(), 0xBEEF);
    libevdev_set_id_bustype(dev.get(), 0x3);
    libevdev_set_id_version(dev.get(), 0x111);
    libevdev_set_name(dev.get(), "Pen passthrough");

    libevdev_enable_property(dev.get(), INPUT_PROP_DIRECT);

    constexpr auto RESOLUTION = 28;

    input_absinfo abs_x {
      0,
      0,
      target_touch_port.width,
      1,
      0,
      RESOLUTION
    };

    input_absinfo abs_y {
      0,
      0,
      target_touch_port.height,
      1,
      0,
      RESOLUTION
    };

    input_absinfo abs_pressure {
      0,
      0,
      PRESSURE_MAX,
      0,
      0,
      0
    };

    input_absinfo abs_distance {
      0,
      0,
      DISTANCE_MAX,
      0,
      0,
      0
    };

    // Degrees of tilt
    input_absinfo abs_tilt {
      0,
      -90,
      90,
      0,
      0,
      0
    };

    // Fractions of the full diagonal
    input_absinfo abs_contact_area {
      0,
      0,
      (__s32) std::sqrt(std::pow(target_touch_port.width, 2) + std::pow(target_touch_port.height, 2)),
      1,
      0,
      RESOLUTION
    };

    libevdev_enable_event_type(dev.get(), EV_ABS);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_X, &abs_x);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_Y, &abs_y);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_PRESSURE, &abs_pressure);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_DISTANCE, &abs_distance);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_TILT_X, &abs_tilt);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_TILT_Y, &abs_tilt);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_TOOL_WIDTH, &abs_contact_area);

    libevdev_enable_event_type(dev.get(), EV_KEY);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_TOUCH, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_TOOL_PEN, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_TOOL_RUBBER, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_STYLUS, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_STYLUS2, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_STYLUS3, nullptr);

    return dev;
  }

  /**
   * @brief Initialize a new `uinput` virtual X360 gamepad and return it.
   * @examples
   * auto my_x360 = x360();
   * @examples_end
   */
  evdev_t
  x360() {
    evdev_t dev { libevdev_new() };

    input_absinfo stick {
      0,
      -32768, 32767,
      16,
      128,
      0
    };

    input_absinfo trigger {
      0,
      0, 255,
      0,
      0,
      0
    };

    input_absinfo dpad {
      0,
      -1, 1,
      0,
      0,
      0
    };

    libevdev_set_uniq(dev.get(), "Sunshine Gamepad");
    libevdev_set_id_product(dev.get(), 0x28E);
    libevdev_set_id_vendor(dev.get(), 0x45E);
    libevdev_set_id_bustype(dev.get(), 0x3);
    libevdev_set_id_version(dev.get(), 0x110);
    libevdev_set_name(dev.get(), "Microsoft X-Box 360 pad");

    libevdev_enable_event_type(dev.get(), EV_KEY);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_WEST, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_EAST, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_NORTH, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_SOUTH, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_THUMBL, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_THUMBR, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_TR, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_TL, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_SELECT, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_MODE, nullptr);
    libevdev_enable_event_code(dev.get(), EV_KEY, BTN_START, nullptr);

    libevdev_enable_event_type(dev.get(), EV_ABS);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_HAT0Y, &dpad);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_HAT0X, &dpad);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_Z, &trigger);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_RZ, &trigger);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_X, &stick);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_RX, &stick);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_Y, &stick);
    libevdev_enable_event_code(dev.get(), EV_ABS, ABS_RY, &stick);

    libevdev_enable_event_type(dev.get(), EV_FF);
    libevdev_enable_event_code(dev.get(), EV_FF, FF_RUMBLE, nullptr);
    libevdev_enable_event_code(dev.get(), EV_FF, FF_CONSTANT, nullptr);
    libevdev_enable_event_code(dev.get(), EV_FF, FF_PERIODIC, nullptr);
    libevdev_enable_event_code(dev.get(), EV_FF, FF_SINE, nullptr);
    libevdev_enable_event_code(dev.get(), EV_FF, FF_RAMP, nullptr);
    libevdev_enable_event_code(dev.get(), EV_FF, FF_GAIN, nullptr);

    return dev;
  }

  /**
   * @brief Initialize the input system and return it.
   * @examples
   * auto my_input = input();
   * @examples_end
   */
  input_t
  input() {
    input_t result { new input_raw_t() };
    auto &gp = *(input_raw_t *) result.get();

    gp.rumble_ctx = notifications.ref();

    gp.gamepads.resize(MAX_GAMEPADS);

    // Ensure starting from clean slate
    gp.clear();
    gp.keyboard_dev = keyboard();
    gp.mouse_rel_dev = mouse_rel();
    gp.mouse_abs_dev = mouse_abs();
    gp.touchscreen_dev = touchscreen();
    gp.pen_dev = penpad();
    gp.gamepad_dev = x360();

    gp.create_mouse_rel();
    gp.create_mouse_abs();
    gp.create_keyboard();

    // If we do not have a keyboard or mouse, fall back to XTest
    if (!gp.mouse_rel_input || !gp.mouse_abs_input || !gp.keyboard_input) {
#ifdef SUNSHINE_BUILD_X11
      if (x11::init() || x11::tst::init()) {
        BOOST_LOG(fatal) << "Unable to create virtual input devices or use XTest fallback! Are you a member of the 'input' group?"sv;
      }
      else {
        BOOST_LOG(error) << "Falling back to XTest for virtual input! Are you a member of the 'input' group?"sv;
        x11::InitThreads();
        gp.display = x11::OpenDisplay(NULL);
      }
#else
      BOOST_LOG(fatal) << "Unable to create virtual input devices! Are you a member of the 'input' group?"sv;
#endif
    }
    else {
      has_uinput = true;
    }

    return result;
  }

  void
  freeInput(void *p) {
    auto *input = (input_raw_t *) p;
    delete input;
  }

  std::vector<supported_gamepad_t> &
  supported_gamepads(input_t *input) {
    static std::vector gamepads {
      supported_gamepad_t { "x360", true, "" },
    };

    return gamepads;
  }

  /**
   * @brief Returns the supported platform capabilities to advertise to the client.
   * @return Capability flags.
   */
  platform_caps::caps_t
  get_capabilities() {
    platform_caps::caps_t caps = 0;

    // Pen and touch emulation requires uinput
    if (has_uinput && config::input.native_pen_touch) {
      caps |= platform_caps::pen_touch;
    }

    return caps;
  }
}  // namespace platf
