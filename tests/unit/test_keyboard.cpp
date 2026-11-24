/**
 * @file tests/unit/test_keyboard.cpp
 * @brief Tests for keyboard packet handling in src/input.*.
 *
 * Moonlight always sends Windows virtual-key codes on the wire, whatever platform the client
 * runs on. Sunshine is responsible for the platform-independent half of the translation:
 * applying the `keybindings` remap, tracking which modifiers the client holds, and injecting
 * synthetic modifier presses when the client reports a modifier Sunshine does not believe is
 * held. Translating a virtual-key code into a native keycode is libvirtualhid's job, and each
 * of its backend tables is covered by that project's own per-platform test job (see
 * `TranslatesKeyboardKeys` in libvirtualhid's `tests/unit/test_*_backend.cpp`). The tables are
 * not reachable from here: they live in anonymous namespaces inside backend translation units
 * that only compile on their own platform.
 *
 * These tests record keyboard output instead of delivering it, so they never type into the
 * machine running the suite. Tests that clear the recorder exercise the real delivery path
 * through the platform adapter to a fake virtual keyboard, including F17/F18 volume remapping.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// local includes
#include "src/config.h"
#include "src/input.h"
#include "src/platform/virtualhid_input.h"

namespace input {
  /**
   * @brief Execute one repeat callback directly for a deterministic keyboard test.
   *
   * @param key_code Client key code being repeated.
   * @param flags Client packet flags.
   * @param synthetic_modifiers Byte mask of synthetic modifiers around the repeated key.
   */
  void repeat_key(std::uint16_t key_code, std::uint8_t flags, std::byte synthetic_modifiers);
}  // namespace input

namespace {
  using namespace std::chrono_literals;

  // Windows virtual-key codes as they arrive from the client.
  constexpr std::uint16_t VKEY_BACK = 0x08;
  constexpr std::uint16_t VKEY_TAB = 0x09;
  constexpr std::uint16_t VKEY_RETURN = 0x0D;
  constexpr std::uint16_t VKEY_SHIFT = 0x10;
  constexpr std::uint16_t VKEY_CONTROL = 0x11;
  constexpr std::uint16_t VKEY_MENU = 0x12;
  constexpr std::uint16_t VKEY_ESCAPE = 0x1B;
  constexpr std::uint16_t VKEY_SPACE = 0x20;
  constexpr std::uint16_t VKEY_A = 0x41;
  constexpr std::uint16_t VKEY_B = 0x42;
  constexpr std::uint16_t VKEY_Z = 0x5A;
  constexpr std::uint16_t VKEY_LWIN = 0x5B;
  constexpr std::uint16_t VKEY_RWIN = 0x5C;
  constexpr std::uint16_t VKEY_F1 = 0x70;
  constexpr std::uint16_t VKEY_LSHIFT = 0xA0;
  constexpr std::uint16_t VKEY_RSHIFT = 0xA1;
  constexpr std::uint16_t VKEY_LCONTROL = 0xA2;
  constexpr std::uint16_t VKEY_RCONTROL = 0xA3;
  constexpr std::uint16_t VKEY_LMENU = 0xA4;
  constexpr std::uint16_t VKEY_RMENU = 0xA5;

  /**
   * @brief A key on the US QWERTY layout and the two ASCII characters it produces.
   */
  struct ascii_key_t {
    std::uint16_t key_code;  ///< Windows virtual-key code Moonlight sends for this key.
    char plain;  ///< Character produced without shift.
    char shifted;  ///< Character produced with shift.
  };

  /**
   * @brief Every US QWERTY key that produces a printable ASCII character.
   */
  constexpr std::array<ascii_key_t, 48> qwerty_ascii_keys {{
    {VKEY_SPACE, ' ', ' '},
    {0x30, '0', ')'},
    {0x31, '1', '!'},
    {0x32, '2', '@'},
    {0x33, '3', '#'},
    {0x34, '4', '$'},
    {0x35, '5', '%'},
    {0x36, '6', '^'},
    {0x37, '7', '&'},
    {0x38, '8', '*'},
    {0x39, '9', '('},
    {0x41, 'a', 'A'},
    {0x42, 'b', 'B'},
    {0x43, 'c', 'C'},
    {0x44, 'd', 'D'},
    {0x45, 'e', 'E'},
    {0x46, 'f', 'F'},
    {0x47, 'g', 'G'},
    {0x48, 'h', 'H'},
    {0x49, 'i', 'I'},
    {0x4A, 'j', 'J'},
    {0x4B, 'k', 'K'},
    {0x4C, 'l', 'L'},
    {0x4D, 'm', 'M'},
    {0x4E, 'n', 'N'},
    {0x4F, 'o', 'O'},
    {0x50, 'p', 'P'},
    {0x51, 'q', 'Q'},
    {0x52, 'r', 'R'},
    {0x53, 's', 'S'},
    {0x54, 't', 'T'},
    {0x55, 'u', 'U'},
    {0x56, 'v', 'V'},
    {0x57, 'w', 'W'},
    {0x58, 'x', 'X'},
    {0x59, 'y', 'Y'},
    {0x5A, 'z', 'Z'},
    {0xBA, ';', ':'},
    {0xBB, '=', '+'},
    {0xBC, ',', '<'},
    {0xBD, '-', '_'},
    {0xBE, '.', '>'},
    {0xBF, '/', '?'},
    {0xC0, '`', '~'},
    {0xDB, '[', '{'},
    {0xDC, '\\', '|'},
    {0xDD, ']', '}'},
    {0xDE, '\'', '"'},
  }};

  /**
   * @brief Non-character keys a client can send that Sunshine must forward verbatim.
   */
  constexpr std::array<std::uint16_t, 43> non_character_keys {{
    VKEY_BACK,
    VKEY_TAB,
    0x0C,  // clear
    VKEY_RETURN,
    VKEY_ESCAPE,
    0x14,  // caps lock
    0x21,  // page up
    0x22,  // page down
    0x23,  // end
    0x24,  // home
    0x25,  // left
    0x26,  // up
    0x27,  // right
    0x28,  // down
    0x2C,  // print screen
    0x2D,  // insert
    0x2E,  // delete
    0x5D,  // apps
    0x60,  // numpad 0
    0x61,  // numpad 1
    0x62,  // numpad 2
    0x63,  // numpad 3
    0x64,  // numpad 4
    0x65,  // numpad 5
    0x66,  // numpad 6
    0x67,  // numpad 7
    0x68,  // numpad 8
    0x69,  // numpad 9
    0x6A,  // numpad multiply
    0x6B,  // numpad add
    0x6D,  // numpad subtract
    0x6E,  // numpad decimal
    0x6F,  // numpad divide
    0x70,  // f1
    0x71,  // f2
    0x72,  // f3
    0x73,  // f4
    0x74,  // f5
    0x75,  // f6
    0x7B,  // f12
    0x90,  // num lock
    0x91,  // scroll lock
    0xFE,  // clear (OEM)
  }};

  /**
   * @brief Every virtual-key code Sunshine recognizes as a modifier key.
   */
  constexpr std::array<std::uint16_t, 9> modifier_keys {{
    VKEY_SHIFT,
    VKEY_CONTROL,
    VKEY_MENU,
    VKEY_LSHIFT,
    VKEY_RSHIFT,
    VKEY_LCONTROL,
    VKEY_RCONTROL,
    VKEY_LMENU,
    VKEY_RMENU,
  }};

  /**
   * @brief One modifier the client can report, with the keys that hold it down.
   */
  struct modifier_t {
    unsigned bit;  ///< Modifier bit carried in the keyboard packet.
    std::uint16_t left;  ///< Left-hand virtual-key code.
    std::uint16_t right;  ///< Right-hand virtual-key code.
    std::uint16_t generic;  ///< Side-agnostic virtual-key code.
    const char *name;  ///< Human-readable modifier name.
  };

  constexpr std::array<modifier_t, 4> modifiers {{
    {MODIFIER_SHIFT, VKEY_LSHIFT, VKEY_RSHIFT, VKEY_SHIFT, "shift"},
    {MODIFIER_CTRL, VKEY_LCONTROL, VKEY_RCONTROL, VKEY_CONTROL, "ctrl"},
    {MODIFIER_ALT, VKEY_LMENU, VKEY_RMENU, VKEY_MENU, "alt"},
    // Meta has no side-agnostic code and Sunshine never synthesizes it.
    {MODIFIER_META, VKEY_LWIN, VKEY_RWIN, VKEY_LWIN, "meta"},
  }};

  /**
   * @brief Which virtual-key code holds a modifier down for a given side.
   */
  enum class side_e {
    left,  ///< Left-hand modifier keys.
    right,  ///< Right-hand modifier keys.
    generic  ///< Side-agnostic modifier keys.
  };

  /**
   * @brief Select the virtual-key code for a modifier and side.
   *
   * @param modifier Modifier being held.
   * @param side Which physical side the client reports.
   * @return Virtual-key code the client sends.
   */
  std::uint16_t key_for_side(const modifier_t &modifier, side_e side) {
    using enum side_e;
    switch (side) {
      case left:
        return modifier.left;
      case right:
        return modifier.right;
      case generic:
      default:
        return modifier.generic;
    }
  }

  /**
   * @brief One modifier combination the client holds down, in press order.
   */
  struct held_modifiers_t {
    std::vector<std::pair<std::uint16_t, unsigned>> keys;  ///< Key code and the mask reported with it.
    unsigned claimed = 0;  ///< Full mask once every key is down.
    std::string label;  ///< Combination name for assertion traces.
  };

  /**
   * @brief Select the modifier keys named by a combination bitmask.
   *
   * Each key carries the mask accumulated so far, which matches a client that presses the
   * modifiers one at a time.
   *
   * @param combination Bit per entry of the modifier table.
   * @param side Which physical side the client reports.
   * @return Keys to hold, the resulting mask, and a trace label.
   */
  held_modifiers_t select_modifiers(unsigned combination, side_e side) {
    held_modifiers_t held;
    for (std::size_t index = 0; index < modifiers.size(); ++index) {
      if (!(combination & (1u << index))) {
        continue;
      }
      held.claimed |= modifiers[index].bit;
      held.keys.emplace_back(key_for_side(modifiers[index], side), held.claimed);
      held.label += (held.label.empty() ? "" : "+");
      held.label += modifiers[index].name;
    }
    return held;
  }

  /**
   * @brief Render a value as a hexadecimal literal for assertion traces.
   *
   * @param value Value to render.
   * @return Hexadecimal literal.
   */
  std::string hex(unsigned value) {
    return std::format("0x{:X}", value);
  }

  /**
   * @brief Render a recorded keyboard event as a compact token.
   *
   * `+0x41` is a press of virtual-key 0x41, `-0x41` its release. Comparing vectors of these
   * tokens keeps assertion failures readable, which matters most for the synthetic modifier
   * regressions: a spurious real Alt shows up as an unexpected `+0x12` in the diff.
   *
   * @param event Recorded keyboard event.
   * @return Token describing the event.
   */
  std::string describe(const input::testing::keyboard_event_t &event) {
    return std::format("{}0x{:02X}", event.release ? '-' : '+', event.key_code);
  }

  /**
   * @brief Build the token for a press of a virtual-key code.
   *
   * @param key_code Virtual-key code emitted toward the platform.
   * @return Press token.
   */
  std::string pressed(std::uint16_t key_code) {
    return describe({key_code, false, 0});
  }

  /**
   * @brief Build the token for a release of a virtual-key code.
   *
   * @param key_code Virtual-key code emitted toward the platform.
   * @return Release token.
   */
  std::string released(std::uint16_t key_code) {
    return describe({key_code, true, 0});
  }

  /**
   * @brief Fixture that records keyboard output instead of delivering it to the host.
   */
  class KeyboardPassthroughTest: public ::testing::Test {
  protected:
    /**
     * @brief Install a fake platform backend, a keyboard recorder, and a stream.
     */
    void SetUp() override {
      original_input_ = config::input;
      config::input.keyboard = true;
      config::input.keybindings.clear();
      config::input.key_rightalt_to_key_win = false;
      // Keep every emission on the calling thread so the recorded order is deterministic.
      config::input.key_repeat_delay = 0ms;

      auto platform_input = platf::input();
      ASSERT_TRUE(platform_input);
      auto &context = platf::virtualhid::get_input_context(platform_input);
      context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      context_ = &context;
      ASSERT_NE(context_->keyboard, nullptr);
      input::testing::set_platform_input(std::move(platform_input));
      input::testing::reset_keyboard_state();

      input::testing::set_keyboard_sink([this](const input::testing::keyboard_event_t &event) {
        events_.push_back(event);
      });

      static int session = 0;
      stream_ = input::alloc(std::make_shared<safe::mail_raw_t>(), std::format("keyboard-test-{}", ++session));
      ASSERT_NE(stream_, nullptr);
    }

    /**
     * @brief Stop recording, drop tracked key state, and restore configuration.
     */
    void TearDown() override {
      input::testing::set_keyboard_sink({});
      input::testing::reset_keyboard_state();
      stream_.reset();
      context_ = nullptr;
      input::testing::set_platform_input({});
      config::input = std::move(original_input_);
    }

    /**
     * @brief Deliver a key press packet.
     *
     * @param key_code Windows virtual-key code sent by the client.
     * @param client_modifiers Modifier bitmask the client reports as held.
     * @param flags Bit flags carried by the packet.
     */
    void press(std::uint16_t key_code, unsigned client_modifiers = 0, unsigned flags = 0) {
      input::testing::send_keyboard_packet(
        stream_,
        key_code,
        static_cast<std::uint8_t>(client_modifiers),
        static_cast<std::uint8_t>(flags),
        false
      );
    }

    /**
     * @brief Deliver a key release packet.
     *
     * @param key_code Windows virtual-key code sent by the client.
     * @param client_modifiers Modifier bitmask the client reports as held.
     * @param flags Bit flags carried by the packet.
     */
    void release(std::uint16_t key_code, unsigned client_modifiers = 0, unsigned flags = 0) {
      input::testing::send_keyboard_packet(
        stream_,
        key_code,
        static_cast<std::uint8_t>(client_modifiers),
        static_cast<std::uint8_t>(flags),
        true
      );
    }

    /**
     * @brief Take the recorded events as comparable tokens.
     *
     * @return Tokens for everything recorded since the last call.
     */
    std::vector<std::string> taken() {
      std::vector<std::string> tokens;
      tokens.reserve(events_.size());
      for (const auto &event : events_) {
        tokens.push_back(describe(event));
      }
      events_.clear();
      return tokens;
    }

    /**
     * @brief Take the recorded events without converting them.
     *
     * @return Events recorded since the last call.
     */
    std::vector<input::testing::keyboard_event_t> taken_events() {
      return std::exchange(events_, {});
    }

    /**
     * @brief Access the fake virtual keyboard installed for the current test.
     *
     * @return Fake libvirtualhid keyboard.
     */
    lvh::Keyboard &fake_keyboard() const {
      return *context_->keyboard;
    }

  private:
    std::shared_ptr<input::input_t> stream_;  ///< Stream input state under test.
    std::vector<input::testing::keyboard_event_t> events_;  ///< Keyboard output recorded for the current test.
    platf::virtualhid::input_context_t *context_ = nullptr;  ///< Fake input context installed in the global backend.
    config::input_t original_input_;  ///< Input configuration restored after each test.
  };
}  // namespace

TEST_F(KeyboardPassthroughTest, ForwardsEveryUnmodifiedKeyUnchanged) {
  for (const auto &key : qwerty_ascii_keys) {
    SCOPED_TRACE("character key " + hex(key.key_code));
    press(key.key_code);
    release(key.key_code);
    EXPECT_EQ(taken(), (std::vector<std::string> {pressed(key.key_code), released(key.key_code)}));
  }

  for (const auto key_code : non_character_keys) {
    SCOPED_TRACE("non-character key " + hex(key_code));
    press(key_code);
    release(key_code);
    EXPECT_EQ(taken(), (std::vector<std::string> {pressed(key_code), released(key_code)}));
  }
}

TEST_F(KeyboardPassthroughTest, PreservesExtendedEnterOnReleaseAndDisconnect) {
  press(VKEY_RETURN);
  release(VKEY_RETURN);
  auto events = taken_events();
  ASSERT_EQ(events.size(), 2U);
  EXPECT_FALSE(events[0].extended);
  EXPECT_FALSE(events[1].extended);

  press(VKEY_RETURN, MODIFIER_EXTENDED | MODIFIER_SHIFT);
  events = taken_events();
  ASSERT_EQ(events.size(), 3U);
  EXPECT_FALSE(events[0].extended);
  EXPECT_TRUE(events[1].extended);
  EXPECT_FALSE(events[2].extended);

  // The bit may be absent on key-up; release the key as it was pressed.
  release(VKEY_RETURN);
  events = taken_events();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_TRUE(events[0].extended);

  press(VKEY_RETURN, MODIFIER_EXTENDED);
  taken_events();
  input::testing::release_held_keys();
  events = taken_events();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_TRUE(events[0].extended);
}

TEST_F(KeyboardPassthroughTest, CoversTheWholePrintableAsciiRange) {
  // Guard the table itself: every printable ASCII character must be reachable, exactly once.
  std::set<char> covered;
  for (const auto &key : qwerty_ascii_keys) {
    if (key.plain != key.shifted) {
      EXPECT_TRUE(covered.insert(key.plain).second) << "duplicate plain character " << key.plain;
    }
    EXPECT_TRUE(covered.insert(key.shifted).second) << "duplicate shifted character " << key.shifted;
  }
  for (char character = 0x20; character < 0x7F; ++character) {
    EXPECT_TRUE(covered.contains(character)) << "no key produces '" << character << "'";
  }
  EXPECT_EQ(covered.size(), 0x7Fu - 0x20u);

  // A client types a shifted character by holding a real shift key, so Sunshine must forward
  // the shift key it was given and must not add a synthetic one on top.
  for (const auto &key : qwerty_ascii_keys) {
    SCOPED_TRACE(std::string {"'"} + key.plain + "' / '" + key.shifted + "'");

    press(key.key_code);
    release(key.key_code);
    EXPECT_EQ(taken(), (std::vector<std::string> {pressed(key.key_code), released(key.key_code)}));

    press(VKEY_LSHIFT, MODIFIER_SHIFT);
    press(key.key_code, MODIFIER_SHIFT);
    release(key.key_code, MODIFIER_SHIFT);
    release(VKEY_LSHIFT, 0);
    EXPECT_EQ(
      taken(),
      (std::vector<std::string> {
        pressed(VKEY_LSHIFT),
        pressed(key.key_code),
        released(key.key_code),
        released(VKEY_LSHIFT)
      })
    );
  }
}

TEST_F(KeyboardPassthroughTest, ForwardsModifierKeysWithoutSyntheticInjection) {
  // A modifier key event never injects a second modifier around itself, even when the client
  // claims modifiers Sunshine does not think are held.
  for (const auto key_code : modifier_keys) {
    SCOPED_TRACE("modifier key " + hex(key_code));
    const std::uint8_t claimed = MODIFIER_SHIFT | MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_META;
    press(key_code, claimed);
    release(key_code, claimed);
    EXPECT_EQ(taken(), (std::vector<std::string> {pressed(key_code), released(key_code)}));
  }
}

TEST_F(KeyboardPassthroughTest, HoldsEveryModifierCombinationWithoutSyntheticInjection) {
  constexpr std::array<side_e, 3> sides {side_e::left, side_e::right, side_e::generic};

  for (const auto side : sides) {
    // Bit 0 selects shift, bit 1 ctrl, bit 2 alt, bit 3 meta: all 15 non-empty combinations of
    // single modifiers, pairs, triples, and the full quad.
    for (unsigned combination = 1; combination < (1u << modifiers.size()); ++combination) {
      const auto held = select_modifiers(combination, side);
      SCOPED_TRACE(std::format("{} (side {})", held.label, static_cast<int>(side)));

      std::vector<std::string> expected;
      for (const auto &[key_code, reported] : held.keys) {
        press(key_code, reported);
        expected.push_back(pressed(key_code));
      }

      press(VKEY_A, held.claimed);
      release(VKEY_A, held.claimed);
      expected.push_back(pressed(VKEY_A));
      expected.push_back(released(VKEY_A));

      for (auto key = held.keys.rbegin(); key != held.keys.rend(); ++key) {
        release(key->first, held.claimed);
        expected.push_back(released(key->first));
      }

      EXPECT_EQ(taken(), expected);
    }
  }
}

TEST_F(KeyboardPassthroughTest, InjectsSyntheticModifiersTheClientHoldsWithoutSendingKeys) {
  // Moonlight can report a modifier without ever sending its key event, for example when the
  // client's own compositor swallowed it. Sunshine wraps the key in a real modifier press.
  struct injectable_t {
    unsigned bit;
    std::uint16_t key_code;
  };

  constexpr std::array<injectable_t, 3> injectable {{
    {MODIFIER_SHIFT, VKEY_SHIFT},
    {MODIFIER_CTRL, VKEY_CONTROL},
    {MODIFIER_ALT, VKEY_MENU},
  }};

  for (unsigned combination = 0; combination < (1u << injectable.size()); ++combination) {
    std::vector<std::string> expected;
    unsigned claimed = 0;
    for (std::size_t index = 0; index < injectable.size(); ++index) {
      if (combination & (1u << index)) {
        claimed |= injectable[index].bit;
        expected.push_back(pressed(injectable[index].key_code));
      }
    }
    SCOPED_TRACE("claimed modifiers " + hex(claimed));

    expected.push_back(pressed(VKEY_B));
    for (std::size_t index = 0; index < injectable.size(); ++index) {
      if (combination & (1u << index)) {
        expected.push_back(released(injectable[index].key_code));
      }
    }

    press(VKEY_B, claimed);
    EXPECT_EQ(taken(), expected);

    // Releases are never wrapped, and the release clears the tracked press.
    release(VKEY_B, claimed);
    EXPECT_EQ(taken(), (std::vector<std::string> {released(VKEY_B)}));
  }
}

TEST_F(KeyboardPassthroughTest, NeverSynthesizesTheMetaModifier) {
  // MODIFIER_META has no synthetic path, so a client-reported meta must not add any key.
  press(VKEY_A, MODIFIER_META);
  release(VKEY_A, MODIFIER_META);
  EXPECT_EQ(taken(), (std::vector<std::string> {pressed(VKEY_A), released(VKEY_A)}));
}

TEST_F(KeyboardPassthroughTest, RepeatsByteMaskedModifiersAndStopsAfterStateIsCleared) {
  config::input.key_repeat_period = 1h;
  constexpr auto synthetic_modifiers = std::byte {MODIFIER_SHIFT} | std::byte {MODIFIER_CTRL} | std::byte {MODIFIER_ALT};
  constexpr std::uint8_t flags = SS_KBE_FLAG_NON_NORMALIZED;
  press(VKEY_A, std::to_integer<unsigned>(synthetic_modifiers) | MODIFIER_EXTENDED, flags);
  taken_events();

  input::repeat_key(VKEY_A, flags, synthetic_modifiers);
  const auto events = taken_events();
  ASSERT_EQ(events.size(), 7U);
  const std::array<std::uint16_t, 7> expected_keys {VKEY_SHIFT, VKEY_CONTROL, VKEY_MENU, VKEY_A, VKEY_SHIFT, VKEY_CONTROL, VKEY_MENU};
  for (std::size_t index = 0; index < events.size(); ++index) {
    EXPECT_EQ(events[index].key_code, expected_keys[index]);
    EXPECT_EQ(events[index].flags, flags);
    EXPECT_EQ(events[index].release, index > 3);
  }
  EXPECT_TRUE(events[3].extended);

  input::testing::reset_keyboard_state();
  input::repeat_key(VKEY_A, flags, synthetic_modifiers);
  EXPECT_TRUE(taken_events().empty());
}

TEST_F(KeyboardPassthroughTest, PropagatesPacketFlagsToEverySyntheticEvent) {
  constexpr std::uint8_t flags = SS_KBE_FLAG_NON_NORMALIZED;

  press(VKEY_A, MODIFIER_SHIFT | MODIFIER_ALT, flags);
  const auto events = taken_events();
  ASSERT_EQ(events.size(), 5u);
  for (const auto &event : events) {
    EXPECT_EQ(event.flags, flags);
  }
  EXPECT_EQ(events[0].key_code, VKEY_SHIFT);
  EXPECT_EQ(events[1].key_code, VKEY_MENU);
  EXPECT_EQ(events[2].key_code, VKEY_A);
  EXPECT_EQ(events[3].key_code, VKEY_SHIFT);
  EXPECT_EQ(events[4].key_code, VKEY_MENU);
}

TEST_F(KeyboardPassthroughTest, IgnoresRepeatedPressesAndUnmatchedReleases) {
  release(VKEY_Z);
  EXPECT_TRUE(taken().empty());

  press(VKEY_Z);
  EXPECT_EQ(taken(), (std::vector<std::string> {pressed(VKEY_Z)}));

  press(VKEY_Z);
  EXPECT_TRUE(taken().empty());

  release(VKEY_Z);
  EXPECT_EQ(taken(), (std::vector<std::string> {released(VKEY_Z)}));

  release(VKEY_Z);
  EXPECT_TRUE(taken().empty());
}

TEST_F(KeyboardPassthroughTest, EmitsRemappedKeycodesFromKeybindings) {
  config::input.keybindings = {
    {VKEY_LWIN, VKEY_LMENU},
    {VKEY_RWIN, VKEY_RMENU},
    {VKEY_LMENU, VKEY_LWIN},
    {VKEY_RMENU, VKEY_RWIN},
  };

  const std::array<std::pair<std::uint16_t, std::uint16_t>, 4> expected {{
    {VKEY_LWIN, VKEY_LMENU},
    {VKEY_RWIN, VKEY_RMENU},
    {VKEY_LMENU, VKEY_LWIN},
    {VKEY_RMENU, VKEY_RWIN},
  }};

  for (const auto &[client, host] : expected) {
    SCOPED_TRACE(hex(client));
    press(client);
    release(client);
    EXPECT_EQ(taken(), (std::vector<std::string> {pressed(host), released(host)}));
  }

  // Unmapped keys are untouched.
  press(VKEY_A);
  release(VKEY_A);
  EXPECT_EQ(taken(), (std::vector<std::string> {pressed(VKEY_A), released(VKEY_A)}));
}

TEST_F(KeyboardPassthroughTest, RemapsFunctionKeysToVolumeKeysThroughThePlatformAdapter) {
  // Issue #4981: F17/F18 must reach the backend as volume keys, with no client extended bit.
  config::input.keybindings = {{0x80, 0xAE}, {0x81, 0xAF}};
  input::testing::set_keyboard_sink({});

  for (const auto flags : {0U, static_cast<unsigned>(SS_KBE_FLAG_NON_NORMALIZED)}) {
    for (const auto &[client, host] : config::input.keybindings) {
      SCOPED_TRACE(hex(client));
      SCOPED_TRACE(flags);
      const auto prior_count = fake_keyboard().submit_count();
      press(client, 0, flags);
      auto event = fake_keyboard().last_submitted_event();
      EXPECT_EQ(event.key_code, host);
      EXPECT_TRUE(event.pressed);
      release(client, 0, flags);
      event = fake_keyboard().last_submitted_event();
      EXPECT_EQ(event.key_code, host);
      EXPECT_FALSE(event.pressed);
      EXPECT_EQ(fake_keyboard().submit_count(), prior_count + 2U);
    }
  }
}

TEST_F(KeyboardPassthroughTest, DoesNotInjectSyntheticAltWhenAltIsRemappedToMeta) {
  // Regression test for the Cmd+Space report: a macOS host swaps Super and Alt so a PC keyboard
  // gets positional Option and Command. shortcutFlags is compared against the client's modifier
  // bitmask, so it has to track the client keycode. Tracking the remapped host keycode instead
  // left the ALT bit clear and wrapped every following key in a real VKEY_MENU press.
  config::input.keybindings = {
    {VKEY_LWIN, VKEY_LMENU},
    {VKEY_RWIN, VKEY_RMENU},
    {VKEY_LMENU, VKEY_LWIN},
    {VKEY_RMENU, VKEY_RWIN},
  };

  press(VKEY_LMENU, MODIFIER_ALT);
  EXPECT_EQ(taken(), (std::vector<std::string> {pressed(VKEY_LWIN)}));

  // Cmd+Space must stay Cmd+Space; a spurious real Alt shows up here as +0x12 / -0x12.
  press(VKEY_SPACE, MODIFIER_ALT);
  release(VKEY_SPACE, MODIFIER_ALT);
  EXPECT_EQ(taken(), (std::vector<std::string> {pressed(VKEY_SPACE), released(VKEY_SPACE)}));

  // Every other key while the remapped modifier is held behaves the same way.
  for (const auto &key : qwerty_ascii_keys) {
    SCOPED_TRACE("character key " + hex(key.key_code));
    press(key.key_code, MODIFIER_ALT);
    release(key.key_code, MODIFIER_ALT);
    EXPECT_EQ(taken(), (std::vector<std::string> {pressed(key.key_code), released(key.key_code)}));
  }

  release(VKEY_LMENU, 0);
  EXPECT_EQ(taken(), (std::vector<std::string> {released(VKEY_LWIN)}));

  // Releasing the remapped Alt clears the tracked ALT bit, so a client that still claims Alt
  // gets the synthetic modifier back.
  press(VKEY_A, MODIFIER_ALT);
  EXPECT_EQ(
    taken(),
    (std::vector<std::string> {pressed(VKEY_MENU), pressed(VKEY_A), released(VKEY_MENU)})
  );
}

TEST_F(KeyboardPassthroughTest, DoesNotInjectSyntheticAltWhenRightAltIsRemappedToMeta) {
  config::input.keybindings = {{VKEY_RMENU, VKEY_RWIN}};

  press(VKEY_RMENU, MODIFIER_ALT);
  press(VKEY_SPACE, MODIFIER_ALT);
  release(VKEY_SPACE, MODIFIER_ALT);
  release(VKEY_RMENU, 0);

  EXPECT_EQ(
    taken(),
    (std::vector<std::string> {
      pressed(VKEY_RWIN),
      pressed(VKEY_SPACE),
      released(VKEY_SPACE),
      released(VKEY_RWIN)
    })
  );
}

TEST_F(KeyboardPassthroughTest, DoesNotInjectSyntheticAltForKeyRightaltToKeyWin) {
  // config.cpp installs this keybinding when the option is enabled.
  config::input.key_rightalt_to_key_win = true;
  config::input.keybindings = {{VKEY_RMENU, VKEY_LWIN}};

  press(VKEY_RMENU, MODIFIER_ALT);
  press(VKEY_SPACE, MODIFIER_ALT);
  release(VKEY_SPACE, MODIFIER_ALT);
  release(VKEY_RMENU, 0);

  EXPECT_EQ(
    taken(),
    (std::vector<std::string> {
      pressed(VKEY_LWIN),
      pressed(VKEY_SPACE),
      released(VKEY_SPACE),
      released(VKEY_LWIN)
    })
  );
}

TEST_F(KeyboardPassthroughTest, KeepsRealAltWhileRightAltMapsToKeyWin) {
  config::input.key_rightalt_to_key_win = true;
  config::input.keybindings = {{VKEY_RMENU, VKEY_LWIN}};

  // Left Alt is a real Alt, so the ALT bit must not be masked away while it is held.
  press(VKEY_LMENU, MODIFIER_ALT);
  press(VKEY_RMENU, MODIFIER_ALT);
  press(VKEY_SPACE, MODIFIER_ALT);
  release(VKEY_SPACE, MODIFIER_ALT);
  EXPECT_EQ(
    taken(),
    (std::vector<std::string> {
      pressed(VKEY_LMENU),
      pressed(VKEY_LWIN),
      pressed(VKEY_SPACE),
      released(VKEY_SPACE)
    })
  );

  // Right Alt (mapped to key_win) goes up while left Alt is still held. The shared ALT bit
  // is only shared, not owned by either side, so it must stay set: the next key must reach
  // the host as-is, with no synthetic Alt press/release wrapped around it.
  release(VKEY_RMENU, MODIFIER_ALT);
  press(VKEY_B, MODIFIER_ALT);
  release(VKEY_B, MODIFIER_ALT);
  EXPECT_EQ(
    taken(),
    (std::vector<std::string> {released(VKEY_LWIN), pressed(VKEY_B), released(VKEY_B)})
  );

  release(VKEY_LMENU, 0);
  EXPECT_EQ(taken(), (std::vector<std::string> {released(VKEY_LMENU)}));

  // With right Alt released and left Alt gone too, the client's ALT claim is honored again.
  press(VKEY_A, MODIFIER_ALT);
  EXPECT_EQ(
    taken(),
    (std::vector<std::string> {pressed(VKEY_MENU), pressed(VKEY_A), released(VKEY_MENU)})
  );
}

TEST_F(KeyboardPassthroughTest, KeepsShiftAndControlSetWhileTheirOtherSideRemainsHeld) {
  /**
   * @brief Modifier key codes and client bit used by one test case.
   */
  struct modifier_case_t {
    std::uint16_t generic;  ///< Side-less modifier key code used for synthetic events.
    std::uint16_t left;  ///< Left modifier key code that remains held.
    std::uint16_t right;  ///< Right modifier key code remapped away from the modifier.
    unsigned bit;  ///< Client modifier bit reported with keyboard packets.
    const char *name;  ///< Modifier name used in assertion traces.
  };

  constexpr std::array cases {
    modifier_case_t {VKEY_SHIFT, VKEY_LSHIFT, VKEY_RSHIFT, MODIFIER_SHIFT, "shift"},
    modifier_case_t {VKEY_CONTROL, VKEY_LCONTROL, VKEY_RCONTROL, MODIFIER_CTRL, "control"},
  };

  for (const auto &modifier : cases) {
    SCOPED_TRACE(modifier.name);
    config::input.keybindings = {{modifier.right, VKEY_LWIN}};

    press(modifier.left, modifier.bit);
    press(modifier.right, modifier.bit);
    EXPECT_EQ(taken(), (std::vector<std::string> {pressed(modifier.left), pressed(VKEY_LWIN)}));

    // Releasing the remapped right key must not clear the aggregate flag while the left key
    // remains held, otherwise the next key is wrapped in a synthetic modifier press/release.
    release(modifier.right, modifier.bit);
    press(VKEY_B, modifier.bit);
    release(VKEY_B, modifier.bit);
    EXPECT_EQ(
      taken(),
      (std::vector<std::string> {released(VKEY_LWIN), pressed(VKEY_B), released(VKEY_B)})
    );

    release(modifier.left, 0);
    EXPECT_EQ(taken(), (std::vector<std::string> {released(modifier.left)}));

    // Once every real modifier key is released, a client-only modifier claim is synthetic.
    press(VKEY_A, modifier.bit);
    EXPECT_EQ(
      taken(),
      (std::vector<std::string> {pressed(modifier.generic), pressed(VKEY_A), released(modifier.generic)})
    );
    release(VKEY_A, modifier.bit);
    EXPECT_EQ(taken(), (std::vector<std::string> {released(VKEY_A)}));
  }
}

TEST_F(KeyboardPassthroughTest, SwallowsDisplaySwitchShortcutWhileAllModifiersAreHeld) {
  press(VKEY_LCONTROL, MODIFIER_CTRL);
  press(VKEY_LMENU, MODIFIER_CTRL | MODIFIER_ALT);
  press(VKEY_LSHIFT, MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT);
  EXPECT_EQ(
    taken(),
    (std::vector<std::string> {pressed(VKEY_LCONTROL), pressed(VKEY_LMENU), pressed(VKEY_LSHIFT)})
  );

  const std::uint8_t all = MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT;

  // F1 is the display-switch shortcut and must not reach the host.
  press(VKEY_F1, all);
  EXPECT_TRUE(taken().empty());

  // A key with no shortcut still passes through.
  press(VKEY_A, all);
  release(VKEY_A, all);
  EXPECT_EQ(taken(), (std::vector<std::string> {pressed(VKEY_A), released(VKEY_A)}));
}

TEST_F(KeyboardPassthroughTest, DeliversKeysThroughTheVirtualKeyboard) {
  // Drop the recorder so the events travel the real path into libvirtualhid's fake backend.
  // This keeps the recorder honest: if platf::keyboard_update stopped being reached, every
  // other test here would still pass.
  input::testing::set_keyboard_sink({});

  auto &keyboard = fake_keyboard();
  const auto submitted = keyboard.submit_count();

  press(VKEY_A);
  EXPECT_EQ(keyboard.submit_count(), submitted + 1);
  EXPECT_EQ(keyboard.last_submitted_event().key_code, VKEY_A);
  EXPECT_TRUE(keyboard.last_submitted_event().pressed);

  release(VKEY_A);
  EXPECT_EQ(keyboard.submit_count(), submitted + 2);
  EXPECT_EQ(keyboard.last_submitted_event().key_code, VKEY_A);
  EXPECT_FALSE(keyboard.last_submitted_event().pressed);

  config::input.keybindings = {{VKEY_LMENU, VKEY_LWIN}};
  press(VKEY_LMENU, MODIFIER_ALT);
  EXPECT_EQ(keyboard.submit_count(), submitted + 3);
  EXPECT_EQ(keyboard.last_submitted_event().key_code, VKEY_LWIN);

  // While the remapped Alt is held, a spurious synthetic Alt would add a press and a release
  // around the key, so both the count and the last event change if the regression comes back.
  press(VKEY_SPACE, MODIFIER_ALT);
  EXPECT_EQ(keyboard.submit_count(), submitted + 4);
  EXPECT_EQ(keyboard.last_submitted_event().key_code, VKEY_SPACE);
  EXPECT_TRUE(keyboard.last_submitted_event().pressed);

  release(VKEY_SPACE, MODIFIER_ALT);
  release(VKEY_LMENU, 0);
  EXPECT_EQ(keyboard.submit_count(), submitted + 6);

  // With no Alt held, the client's Alt claim is honored with a real synthetic modifier.
  press(VKEY_B, MODIFIER_ALT);
  EXPECT_EQ(keyboard.submit_count(), submitted + 9);
  EXPECT_EQ(keyboard.last_submitted_event().key_code, VKEY_MENU);
  EXPECT_FALSE(keyboard.last_submitted_event().pressed);
}

TEST_F(KeyboardPassthroughTest, ReleasesHeldKeysAsThemselvesWithoutKeybindings) {
  press(VKEY_LMENU, MODIFIER_ALT);
  press(VKEY_A, MODIFIER_ALT);
  EXPECT_EQ(taken(), (std::vector<std::string> {pressed(VKEY_LMENU), pressed(VKEY_A)}));

  input::testing::release_held_keys();

  // key_press is an unordered_map, so the release order is unspecified.
  auto released_keys = taken();
  std::ranges::sort(released_keys);
  std::vector expected {released(VKEY_LMENU), released(VKEY_A)};
  std::ranges::sort(expected);
  EXPECT_EQ(released_keys, expected);
}

TEST_F(KeyboardPassthroughTest, ReleasesTheRemappedHostKeyWhenTheClientDisconnects) {
  config::input.keybindings = {{VKEY_LMENU, VKEY_LWIN}};

  // The press goes down as the remapped host key.
  press(VKEY_LMENU, MODIFIER_ALT);
  EXPECT_EQ(taken(), (std::vector<std::string> {pressed(VKEY_LWIN)}));

  input::testing::release_held_keys();

  // key_press is keyed on the unmapped 0xA4, so releasing that would leave the host holding
  // the remapped key down forever.
  EXPECT_EQ(taken(), (std::vector<std::string> {released(VKEY_LWIN)}));
}

TEST_F(KeyboardPassthroughTest, ReleasesHeldEditingKeysWhenClientDisconnects) {
  constexpr unsigned non_normalized = 0x01;  // SS_KBE_FLAG_NON_NORMALIZED

  struct editing_key_case_t {
    std::uint16_t client_key;  ///< Keycode sent by the client.
    std::uint16_t host_key;  ///< Canonical keycode emitted toward the platform.
  };

  constexpr std::array<editing_key_case_t, 8> cases {{
    {0x7A, 0x0100},  // SDL scancode Undo
    {0x7B, 0x0101},  // SDL scancode Cut
    {0x7C, 0x0102},  // SDL scancode Copy
    {0x7D, 0x0103},  // SDL scancode Paste
    {0x0100, 0x0100},  // Dedicated Undo
    {0x0101, 0x0101},  // Dedicated Cut
    {0x0102, 0x0102},  // Dedicated Copy
    {0x0103, 0x0103},  // Dedicated Paste
  }};

  for (const auto &editing_key : cases) {
    SCOPED_TRACE(hex(editing_key.client_key));

    press(editing_key.client_key, 0, non_normalized);
    EXPECT_EQ(taken(), (std::vector<std::string> {pressed(editing_key.host_key)}));

    input::testing::release_held_keys();

    // The disconnect cleanup must release the full 16-bit code. Masking it to the low byte
    // would release 0x03 instead of 0x0103, leaving the host holding the key down.
    const auto events = taken_events();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].key_code, editing_key.host_key);
    EXPECT_TRUE(events[0].release);
    EXPECT_EQ(events[0].flags, non_normalized);
  }
}

TEST_F(KeyboardPassthroughTest, ReleasesTheRemappedHostKeyForHeldEditingKeysWhenClientDisconnects) {
  constexpr unsigned non_normalized = 0x01;  // SS_KBE_FLAG_NON_NORMALIZED
  config::input.keybindings = {{0x0103, VKEY_F1}, {0x0100, VKEY_F1 + 1}};

  // A dedicated keycode and a scancode alias both resolve to a canonical code before remapping.
  press(0x0103, 0, non_normalized);
  press(0x7A, 0, non_normalized);
  auto pressed_keys = taken();
  std::ranges::sort(pressed_keys);
  std::vector expected_pressed {pressed(VKEY_F1), pressed(VKEY_F1 + 1)};
  std::ranges::sort(expected_pressed);
  EXPECT_EQ(pressed_keys, expected_pressed);

  input::testing::release_held_keys();

  // key_press is an unordered_map, so the release order is unspecified.
  auto released_keys = taken();
  std::ranges::sort(released_keys);
  std::vector expected_released {released(VKEY_F1), released(VKEY_F1 + 1)};
  std::ranges::sort(expected_released);
  EXPECT_EQ(released_keys, expected_released);
}

TEST_F(KeyboardPassthroughTest, ReleasesTheRemappedRightAltForKeyRightaltToKeyWin) {
  // Mirrors what config parsing installs for the shipped key_rightalt_to_key_win option.
  config::input.key_rightalt_to_key_win = true;
  config::input.keybindings = {{VKEY_RMENU, VKEY_LWIN}};

  press(VKEY_RMENU, MODIFIER_ALT);
  EXPECT_EQ(taken(), (std::vector<std::string> {pressed(VKEY_LWIN)}));

  input::testing::release_held_keys();

  EXPECT_EQ(taken(), (std::vector<std::string> {released(VKEY_LWIN)}));
}
