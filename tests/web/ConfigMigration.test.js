import { flushPromises, shallowMount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))

import Config from '../../src_assets/common/assets/web/Config.vue'

async function mountConfig(serverConfig) {
  const fetchMock = vi.fn(async (url, options) => {
    if (url === './api/config' && !options) {
      return { json: async () => ({ platform: 'windows', version: 'test', status: true, ...serverConfig }) }
    }
    if (url === './api/config' && options?.method === 'POST') {
      return { status: 200, json: async () => ({ status: true }) }
    }
    throw new Error(`Unexpected fetch: ${url}`)
  })
  vi.stubGlobal('fetch', fetchMock)

  const wrapper = shallowMount(Config, {
    global: {
      mocks: { $t: key => key },
    },
  })
  await flushPromises()
  return { wrapper, fetchMock }
}

afterEach(() => {
  vi.unstubAllGlobals()
  document.body.innerHTML = ''
})

describe('external_audio upgrade migration', () => {
  it.each(['enabled', 'true', 'on', 'yes', '1', 'TRUE', 'On'])('maps legacy external_audio=%s to keep_default_sink', async legacyValue => {
    const { wrapper } = await mountConfig({ external_audio: legacyValue })

    expect(wrapper.vm.config.keep_default_sink).toBe('enabled')
    expect(wrapper.vm.config).not.toHaveProperty('external_audio')
    wrapper.unmount()
  })

  it('keeps the migrated value disabled when the legacy key is disabled', async () => {
    const { wrapper } = await mountConfig({ external_audio: 'disabled' })

    expect(wrapper.vm.config.keep_default_sink).toBe('disabled')
    expect(wrapper.vm.config).not.toHaveProperty('external_audio')
    wrapper.unmount()
  })

  it('lets an explicit keep_default_sink win over the legacy key', async () => {
    const { wrapper } = await mountConfig({ external_audio: 'enabled', keep_default_sink: 'disabled' })

    expect(wrapper.vm.config.keep_default_sink).toBe('disabled')
    expect(wrapper.vm.config).not.toHaveProperty('external_audio')
    wrapper.unmount()
  })

  it('migrates when keep_default_sink is present but empty', async () => {
    const { wrapper } = await mountConfig({ external_audio: 'enabled', keep_default_sink: '' })

    expect(wrapper.vm.config.keep_default_sink).toBe('enabled')
    expect(wrapper.vm.config).not.toHaveProperty('external_audio')
    wrapper.unmount()
  })

  it('sends neither key when a migrated user unchecks and saves', async () => {
    const { wrapper, fetchMock } = await mountConfig({ external_audio: 'enabled' })

    wrapper.vm.config.keep_default_sink = 'disabled'
    await wrapper.vm.save()

    const postCall = fetchMock.mock.calls.find(([, options]) => options?.method === 'POST')
    expect(postCall).toBeDefined()
    const sent = JSON.parse(postCall[1].body)
    expect(sent).not.toHaveProperty('external_audio')
    expect(sent).not.toHaveProperty('keep_default_sink')
    wrapper.unmount()
  })
})
