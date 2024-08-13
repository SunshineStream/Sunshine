/**
 * @file src/input/keyboard.cpp
 * @brief Definitions for common keyboard input.
 */
#include "src/input/keyboard.h"
#include "src/input/init.h"
#include "src/input/platform_input.h"
#include "src/input/processor.h"

namespace input::keyboard {

  constexpr auto VKEY_SHIFT = 0x10;
  constexpr auto VKEY_LSHIFT = 0xA0;
  constexpr auto VKEY_RSHIFT = 0xA1;
  constexpr auto VKEY_CONTROL = 0x11;
  constexpr auto VKEY_LCONTROL = 0xA2;
  constexpr auto VKEY_RCONTROL = 0xA3;
  constexpr auto VKEY_MENU = 0x12;
  constexpr auto VKEY_LMENU = 0xA4;
  constexpr auto VKEY_RMENU = 0xA5;

  static task_pool_util::TaskPool::task_id_t key_press_repeat_id {};
  static std::unordered_map<key_press_id_t, bool> key_press {};

  namespace {
    key_press_id_t
    make_kpid(uint16_t vk, uint8_t flags) {
      return (key_press_id_t) vk << 8 | flags;
    }

    uint16_t
    vk_from_kpid(key_press_id_t kpid) {
      return kpid >> 8;
    }

    uint8_t
    flags_from_kpid(key_press_id_t kpid) {
      return kpid & 0xFF;
    }

    /**
     * @brief Apply shortcut based on VKEY.
     * @param keyCode The VKEY code.
     * @return 0 if no shortcut applied, > 0 if shortcut applied.
     */
    int
    apply_shortcut(short keyCode) {
      constexpr auto VK_F1 = 0x70;
      constexpr auto VK_F13 = 0x7C;

      BOOST_LOG(debug) << "Apply Shortcut: 0x"sv << util::hex((std::uint8_t) keyCode).to_string_view();

      if (keyCode >= VK_F1 && keyCode <= VK_F13) {
        mail::man->event<int>(mail::switch_display)->raise(keyCode - VK_F1);
        return 1;
      }

      switch (keyCode) {
        case 0x4E /* VKEY_N */:
          display_cursor = !display_cursor;
          return 1;
      }

      return 0;
    }

    short
    map_keycode(short keycode) {
      auto it = config::input.keybindings.find(keycode);
      if (it != std::end(config::input.keybindings)) {
        return it->second;
      }

      return keycode;
    }

    /**
     * @brief Update flags for keyboard shortcut combos
     */
    void
    update_shortcutFlags(int *flags, short keyCode, bool release) {
      switch (keyCode) {
        case VKEY_SHIFT:
        case VKEY_LSHIFT:
        case VKEY_RSHIFT:
          if (release) {
            *flags &= ~input_t::SHIFT;
          }
          else {
            *flags |= input_t::SHIFT;
          }
          break;
        case VKEY_CONTROL:
        case VKEY_LCONTROL:
        case VKEY_RCONTROL:
          if (release) {
            *flags &= ~input_t::CTRL;
          }
          else {
            *flags |= input_t::CTRL;
          }
          break;
        case VKEY_MENU:
        case VKEY_LMENU:
        case VKEY_RMENU:
          if (release) {
            *flags &= ~input_t::ALT;
          }
          else {
            *flags |= input_t::ALT;
          }
          break;
      }
    }

    bool
    is_modifier(uint16_t keyCode) {
      switch (keyCode) {
        case VKEY_SHIFT:
        case VKEY_LSHIFT:
        case VKEY_RSHIFT:
        case VKEY_CONTROL:
        case VKEY_LCONTROL:
        case VKEY_RCONTROL:
        case VKEY_MENU:
        case VKEY_LMENU:
        case VKEY_RMENU:
          return true;
        default:
          return false;
      }
    }

    void
    send_key_and_modifiers(uint16_t key_code, bool release, uint8_t flags, uint8_t synthetic_modifiers) {
      if (!release) {
        // Press any synthetic modifiers required for this key
        if (synthetic_modifiers & MODIFIER_SHIFT) {
          platf::keyboard_update(PlatformInput::getInstance(), VKEY_SHIFT, false, flags);
        }
        if (synthetic_modifiers & MODIFIER_CTRL) {
          platf::keyboard_update(PlatformInput::getInstance(), VKEY_CONTROL, false, flags);
        }
        if (synthetic_modifiers & MODIFIER_ALT) {
          platf::keyboard_update(PlatformInput::getInstance(), VKEY_MENU, false, flags);
        }
      }

      platf::keyboard_update(PlatformInput::getInstance(), map_keycode(key_code), release, flags);

      if (!release) {
        // Raise any synthetic modifier keys we pressed
        if (synthetic_modifiers & MODIFIER_SHIFT) {
          platf::keyboard_update(PlatformInput::getInstance(), VKEY_SHIFT, true, flags);
        }
        if (synthetic_modifiers & MODIFIER_CTRL) {
          platf::keyboard_update(PlatformInput::getInstance(), VKEY_CONTROL, true, flags);
        }
        if (synthetic_modifiers & MODIFIER_ALT) {
          platf::keyboard_update(PlatformInput::getInstance(), VKEY_MENU, true, flags);
        }
      }
    }

    void
    repeat_key(uint16_t key_code, uint8_t flags, uint8_t synthetic_modifiers) {
      // If key no longer pressed, stop repeating
      if (!key_press[make_kpid(key_code, flags)]) {
        key_press_repeat_id = nullptr;
        return;
      }

      send_key_and_modifiers(key_code, false, flags, synthetic_modifiers);

      key_press_repeat_id = task_pool.pushDelayed(repeat_key, config::input.key_repeat_period, key_code, flags, synthetic_modifiers).task_id;
    }
  }  // namespace

  void
  print(PNV_KEYBOARD_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin keyboard packet--"sv << std::endl
      << "keyAction ["sv << util::hex(packet->header.magic).to_string_view() << ']' << std::endl
      << "keyCode ["sv << util::hex(packet->keyCode).to_string_view() << ']' << std::endl
      << "modifiers ["sv << util::hex(packet->modifiers).to_string_view() << ']' << std::endl
      << "flags ["sv << util::hex(packet->flags).to_string_view() << ']' << std::endl
      << "--end keyboard packet--"sv;
  }

  void
  print(PNV_UNICODE_PACKET packet) {
    std::string text(packet->text, util::endian::big(packet->header.size) - sizeof(packet->header.magic));
    BOOST_LOG(debug)
      << "--begin unicode packet--"sv << std::endl
      << "text ["sv << text << ']' << std::endl
      << "--end unicode packet--"sv;
  }

  void
  passthrough(std::shared_ptr<input_t> &input, PNV_KEYBOARD_PACKET packet) {
    if (!config::input.keyboard) {
      return;
    }

    auto release = util::endian::little(packet->header.magic) == KEY_UP_EVENT_MAGIC;
    auto keyCode = packet->keyCode & 0x00FF;

    // Set synthetic modifier flags if the keyboard packet is requesting modifier
    // keys that are not current pressed.
    uint8_t synthetic_modifiers = 0;
    if (!release && !is_modifier(keyCode)) {
      if (!(input->shortcutFlags & input_t::SHIFT) && (packet->modifiers & MODIFIER_SHIFT)) {
        synthetic_modifiers |= MODIFIER_SHIFT;
      }
      if (!(input->shortcutFlags & input_t::CTRL) && (packet->modifiers & MODIFIER_CTRL)) {
        synthetic_modifiers |= MODIFIER_CTRL;
      }
      if (!(input->shortcutFlags & input_t::ALT) && (packet->modifiers & MODIFIER_ALT)) {
        synthetic_modifiers |= MODIFIER_ALT;
      }
    }

    auto &pressed = key_press[make_kpid(keyCode, packet->flags)];
    if (!pressed) {
      if (!release) {
        // A new key has been pressed down, we need to check for key combo's
        // If a key-combo has been pressed down, don't pass it through
        if (input->shortcutFlags == input_t::SHORTCUT && apply_shortcut(keyCode) > 0) {
          return;
        }

        if (key_press_repeat_id) {
          task_pool.cancel(key_press_repeat_id);
        }

        if (config::input.key_repeat_delay.count() > 0) {
          key_press_repeat_id = task_pool.pushDelayed(repeat_key, config::input.key_repeat_delay, keyCode, packet->flags, synthetic_modifiers).task_id;
        }
      }
      else {
        // Already released
        return;
      }
    }
    else if (!release) {
      // Already pressed down key
      return;
    }

    pressed = !release;

    send_key_and_modifiers(keyCode, release, packet->flags, synthetic_modifiers);

    update_shortcutFlags(&input->shortcutFlags, map_keycode(keyCode), release);
  }

  void
  passthrough(PNV_UNICODE_PACKET packet) {
    if (!config::input.keyboard) {
      return;
    }

    auto size = util::endian::big(packet->header.size) - sizeof(packet->header.magic);
    platf::unicode(PlatformInput::getInstance(), packet->text, size);
  }

  void
  reset(platf::input_t &platf_input) {
    for (auto &kp : key_press) {
      if (!kp.second) {
        // already released
        continue;
      }
      platf::keyboard_update(platf_input, vk_from_kpid(kp.first) & 0x00FF, true, flags_from_kpid(kp.first));
      key_press[kp.first] = false;
    }
  }

  void
  cancel() {
    task_pool.cancel(key_press_repeat_id);
  }
}  // namespace input::keyboard
