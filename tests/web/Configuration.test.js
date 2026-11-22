import { mount } from '@vue/test-utils'
import { describe, expect, it } from 'vitest'

import DisplayDeviceOptions from '../../src_assets/common/assets/web/configs/tabs/audiovideo/DisplayDeviceOptions.vue'
import Advanced from '../../src_assets/common/assets/web/configs/tabs/Advanced.vue'
import General from '../../src_assets/common/assets/web/configs/tabs/General.vue'
import Inputs from '../../src_assets/common/assets/web/configs/tabs/Inputs.vue'
import Network from '../../src_assets/common/assets/web/configs/tabs/Network.vue'

const globalOptions = {
  mocks: {
    $t: key => key,
  },
  stubs: {
    Checkbox: true,
    Info: true,
    PlatformLayout: {
      template: '<div><slot name="windows" /></div>',
    },
    Play: true,
    Plus: true,
    Shield: true,
    Trash2: true,
    TriangleAlert: true,
    Undo: true,
  },
}

describe('configuration accessibility', () => {
  it('gives every global prep command input an accessible label', () => {
    const wrapper = mount(General, {
      props: {
        platform: 'windows',
        config: {
          global_prep_cmd: [{ do: 'start', elevated: false, undo: 'stop' }],
          notify_pre_releases: false,
          system_tray: true,
        },
      },
      global: globalOptions,
    })

    for (const input of wrapper.findAll('#global_prep_cmd tbody input')) {
      expect(wrapper.get(`label[for="${input.attributes('id')}"]`).classes()).toContain('visually-hidden')
    }
  })

  it('associates labels with manual display settings and remapping inputs', () => {
    const manualWrapper = mount(DisplayDeviceOptions, {
      props: {
        platform: 'windows',
        config: {
          dd_configuration_option: 'ensure_active',
          dd_config_revert_on_disconnect: false,
          dd_hdr_option: 'disabled',
          dd_mode_remapping: {
            mixed: [],
            refresh_rate_only: [],
            resolution_only: [],
          },
          dd_refresh_rate_option: 'manual',
          dd_resolution_option: 'manual',
        },
      },
      global: globalOptions,
    })

    expect(manualWrapper.get('label[for="dd_manual_resolution"]')).toBeDefined()
    expect(manualWrapper.get('label[for="dd_manual_refresh_rate"]')).toBeDefined()

    const remappingWrapper = mount(DisplayDeviceOptions, {
      props: {
        platform: 'windows',
        config: {
          dd_configuration_option: 'ensure_active',
          dd_config_revert_on_disconnect: false,
          dd_hdr_option: 'disabled',
          dd_mode_remapping: {
            mixed: [{
              final_refresh_rate: '',
              final_resolution: '',
              requested_fps: '',
              requested_resolution: '',
            }],
            refresh_rate_only: [],
            resolution_only: [],
          },
          dd_refresh_rate_option: 'auto',
          dd_resolution_option: 'auto',
        },
      },
      global: globalOptions,
    })

    for (const input of remappingWrapper.findAll('tbody input')) {
      expect(remappingWrapper.get(`label[for="${input.attributes('id')}"]`).classes()).toContain('visually-hidden')
    }
  })
})

describe('network configuration', () => {
  it('uses the default Moonlight port when the configured port is absent', () => {
    const wrapper = mount(Network, {
      props: {
        platform: 'linux',
        config: { upnp: false, dscp_tagging: 'enabled' },
      },
      global: globalOptions,
    })

    const tableText = wrapper.get('table').text()
    expect(tableText).toContain('47984')
    expect(tableText).toContain('47989')
    expect(tableText).toContain('48010')
    expect(tableText).not.toContain('NaN')
  })

  it('toggles DSCP tagging between enabled and disabled', async () => {
    const config = { upnp: false, dscp_tagging: 'enabled' }
    const wrapper = mount(Network, {
      props: { platform: 'linux', config },
      global: { ...globalOptions, stubs: { ...globalOptions.stubs, Checkbox: false } },
    })
    const input = wrapper.get('#dscp_tagging')

    expect(input.element.checked).toBe(true)

    await input.setValue(false)
    expect(config.dscp_tagging).toBe('disabled')

    await input.setValue(true)
    expect(config.dscp_tagging).toBe('enabled')
  })
})

describe('FEC configuration', () => {
  it.each([0, 1, 20, 99, 100])('accepts %i percent and updates the configuration', async percentage => {
    const config = { fec_percentage: 20 }
    const wrapper = mount(Advanced, {
      props: { platform: 'macos', config },
      global: globalOptions,
    })
    const input = wrapper.get('#fec_percentage')

    await input.setValue(String(percentage))

    expect(input.element.checkValidity()).toBe(true)
    expect(config.fec_percentage).toBe(percentage)
  })

  it.each([-1, 101, 255, 0.5])('marks %s percent as invalid', async percentage => {
    const wrapper = mount(Advanced, {
      props: { platform: 'macos', config: { fec_percentage: 20 } },
      global: globalOptions,
    })
    const input = wrapper.get('#fec_percentage')

    await input.setValue(String(percentage))

    expect(input.element.checkValidity()).toBe(false)
  })
})

describe('gamepad input configuration', () => {
  function mountInputs(platform, gamepadDriver = '', gamepad = 'auto', controller = 'enabled') {
    return mount(Inputs, {
      props: {
        platform,
        config: {
          controller,
          gamepad,
          gamepad_driver: gamepadDriver,
          keybindings: '[]',
          motion_as_ds4: 'enabled',
          touchpad_as_ds4: 'enabled',
          ds4_back_as_touchpad_click: 'enabled',
          virtualhid_randomize_mac: 'enabled',
          keyboard: 'disabled',
          mouse: 'disabled',
        },
      },
      global: {
        mocks: { $t: key => key },
        stubs: { Checkbox: true, VirtualKeyCodeSelect: true },
      },
    })
  }

  it.each(['freebsd', 'linux', 'macos', 'windows'])('offers all profiles and automatic detection on %s', platform => {
    const wrapper = mountInputs(platform, platform === 'windows' ? 'virtualhid' : '')

    expect(wrapper.find('#gamepad_driver').exists()).toBe(platform === 'windows' || platform === 'macos')
    expect(wrapper.get('#gamepad').findAll('option').map(option => option.attributes('value'))).toEqual([
      'auto', 'generic', 'x360', 'xone', 'xseries', 'ds4', 'ds5', 'switch',
    ])
    expect(wrapper.find('#motion_as_ds4').exists()).toBe(true)
    expect(wrapper.find('#touchpad_as_ds4').exists()).toBe(true)
  })

  it('shows the existing Enable Gamepad Input control on macOS', () => {
    const enabled = mountInputs('macos')
    expect(enabled.find('#controller').exists()).toBe(true)
    expect(enabled.find('#gamepad').exists()).toBe(true)

    const disabled = mountInputs('macos', '', 'auto', 'disabled')
    expect(disabled.find('#controller').exists()).toBe(true)
    expect(disabled.find('#gamepad_driver').exists()).toBe(true)
    expect(disabled.find('#gamepad').exists()).toBe(false)
    expect(disabled.find('#keyboard').exists()).toBe(true)
    expect(disabled.find('#mouse').exists()).toBe(true)
  })

  it.each(['macos', 'windows'])('offers None to disable gamepads on %s', async platform => {
    const wrapper = mountInputs(platform)

    const backend = wrapper.get('#gamepad_driver')
    expect(backend.findAll('option').map(option => option.attributes('value'))).toContain('none')
    if (platform === 'macos') {
      expect(backend.element.value).toBe('virtualhid')
    }

    await backend.setValue('none')
    expect(wrapper.find('#gamepad').exists()).toBe(false)
    expect(wrapper.find('#motion_as_ds4').exists()).toBe(false)
    expect(wrapper.find('#back_button_timeout').exists()).toBe(false)
    expect(wrapper.find('#keyboard').exists()).toBe(true)
    expect(wrapper.find('#mouse').exists()).toBe(true)
  })

  it('limits Windows ViGEmBus to Xbox 360 and DualShock 4', async () => {
    const wrapper = mountInputs('windows', 'virtualhid', 'xone')

    await wrapper.get('#gamepad_driver').setValue('vigembus')

    expect(wrapper.get('#gamepad').findAll('option').map(option => option.attributes('value'))).toEqual([
      'auto', 'x360', 'ds4',
    ])
    expect(wrapper.get('#gamepad').element.value).toBe('auto')
  })

  it('toggles input only mode between enabled and disabled', async () => {
    const config = {
      controller: 'enabled',
      gamepad: 'auto',
      gamepad_driver: '',
      keybindings: '[]',
      motion_as_ds4: 'enabled',
      touchpad_as_ds4: 'enabled',
      ds4_back_as_touchpad_click: 'enabled',
      virtualhid_randomize_mac: 'enabled',
      keyboard: 'enabled',
      key_repeat_delay: 500,
      key_repeat_frequency: 24.9,
      always_send_scancodes: 'enabled',
      key_rightalt_to_key_win: 'disabled',
      mouse: 'enabled',
      high_resolution_scrolling: 'enabled',
      native_pen_touch: 'enabled',
      enable_input_only_mode: 'disabled',
    }

    const wrapper = mount(Inputs, {
      props: { platform: 'linux', config },
      global: {
        mocks: { $t: key => key },
        stubs: { Checkbox: false, VirtualKeyCodeSelect: true },
      },
    })

    const input = wrapper.get('#enable_input_only_mode')

    expect(input.element.checked).toBe(false)

    await input.setValue(true)
    expect(config.enable_input_only_mode).toBe('enabled')

    await input.setValue(false)
    expect(config.enable_input_only_mode).toBe('disabled')
  })
})
