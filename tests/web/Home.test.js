import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))
vi.mock('../../src_assets/common/assets/web/ResourceCard.vue', () => ({
  default: { template: '<div />' },
}))

import Home from '../../src_assets/common/assets/web/Home.vue'

async function mountHome(platform, { developmentVersion = true, licensed = true, serviceAvailable = true, gamepadDriver = 'virtualhid', permissions = [] } = {}) {
  vi.stubGlobal('fetch', vi.fn(async url => {
    if (url === './api/config') {
      return { json: async () => ({ platform, controller: 'enabled', gamepad_driver: gamepadDriver, version: '2026.927.1200' }) }
    }
    if (url === './api/virtual-input/status') {
      return {
        json: async () => ({
          virtualhid: { installed: true, development_version: developmentVersion, version_compatible: true },
          vigembus: { installed: false, version_compatible: false },
        }),
      }
    }
    if (url === './api/permissions') {
      return { json: async () => ({ permissions }) }
    }
    if (url === './api/virtual-input/license') {
      return { json: async () => ({ licensed, service_available: serviceAvailable }) }
    }
    if (url === './api/logs') {
      return { text: async () => '' }
    }
    if (url === 'https://api.github.com/repos/SunshineStream/Sunshine/releases/latest') {
      return { json: async () => ({ tag_name: 'v2026.927.1200' }) }
    }
    if (url === 'https://api.github.com/repos/SunshineStream/Sunshine/releases') {
      return { json: async () => [{ tag_name: 'v2026.927.1200', prerelease: true }] }
    }
    throw new Error(`Unexpected fetch: ${url}`)
  }))

  const wrapper = mount(Home, {
    global: {
      mocks: { $t: key => key },
      stubs: { RouterLink: true },
    },
  })
  await flushPromises()
  return wrapper
}

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('development broker home notice', () => {
  it('links to permission controls when macOS is missing required access', async () => {
    const wrapper = await mountHome('macos', { permissions: [
      { id: 'screen_recording', status: 'denied', required: true, verifiable: true },
      { id: 'notifications', status: 'denied', required: false, verifiable: true },
      { id: 'local_network', status: 'on_use', required: true, verifiable: false },
    ] })
    expect(wrapper.text()).toContain('index.permissions_missing_title')
    expect(wrapper.findAll('.alert-warning a, .alert-warning router-link-stub').length).toBeGreaterThan(0)
    wrapper.unmount()
  })

  it('links to permission guidance when Windows cannot access its config directory', async () => {
    const wrapper = await mountHome('windows', { permissions: [
      { id: 'config_directory', status: 'denied', required: true, verifiable: true },
    ] })
    expect(wrapper.text()).toContain('index.permissions_missing_title')
    wrapper.unmount()
  })

  it.each(['windows', 'macos'])('shows the development card on %s', async platform => {
    const wrapper = await mountHome(platform)

    expect(fetch).toHaveBeenCalledWith('./api/virtual-input/status')
    expect(wrapper.get('.alert.my-4').text()).toContain('index.virtualhid_development_title')
    expect(wrapper.get('.alert.my-4').text()).toContain('index.virtualhid_development_desc')
    wrapper.unmount()
  })

  it('prioritizes a macOS license warning over the development card', async () => {
    const wrapper = await mountHome('macos', { licensed: false })

    expect(wrapper.get('.alert.my-4').text()).toContain('index.virtualhid_macos_license_title')
    expect(wrapper.get('.alert.my-4').text()).not.toContain('index.virtualhid_development_title')
    wrapper.unmount()
  })

  it('shows the macOS broker warning when the license service is unavailable', async () => {
    const wrapper = await mountHome('macos', { licensed: false, serviceAvailable: false })

    expect(wrapper.get('.alert.my-4').text()).toContain('index.virtualhid_broker_unavailable_title')
    expect(wrapper.get('.alert.my-4').text()).not.toContain('index.virtualhid_development_title')
    wrapper.unmount()
  })

  it('does not show a development card for a stable macOS broker', async () => {
    const wrapper = await mountHome('macos', { developmentVersion: false })

    expect(wrapper.find('.alert.my-4').exists()).toBe(false)
    wrapper.unmount()
  })

  it('does not show a broker card when gamepads are disabled', async () => {
    const wrapper = await mountHome('macos', { gamepadDriver: 'none' })

    expect(wrapper.find('.alert.my-4').exists()).toBe(false)
    wrapper.unmount()
  })
})
