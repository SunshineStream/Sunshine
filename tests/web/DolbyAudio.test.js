import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import Config from '../../src_assets/common/assets/web/Config.vue'

const globalOptions = {
  mocks: { $t: key => key, $tp: key => key },
  stubs: {
    Navbar: true, General: true, Inputs: true, Network: true, Files: true,
    Advanced: true, ContainerEncoders: true,
    PlatformLayout: { template: '<div><slot /><slot name="windows" /></div>' },
    AdapterNameSelector: true, DisplayOutputSelector: true,
    DisplayDeviceOptions: true, DisplayModesSettings: true,
  },
}

let wrapper
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  vi.unstubAllGlobals()
})

async function mountConfig({ platform, audioFormat }) {
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue({
    json: () => Promise.resolve({
      status: true,
      platform,
      version: 'test',
      audio_format: audioFormat,
      high_quality_audio: false,
    }),
  }))
  wrapper = mount(Config, { global: globalOptions })
  await flushPromises()
  wrapper.vm.currentTab = 'av'
  await wrapper.vm.$nextTick()
  return wrapper
}

describe('Dolby audio configuration', () => {
  it.each(['windows', 'linux', 'macos', 'freebsd'])('offers the Dolby selector on %s', async platform => {
    const page = await mountConfig({ platform, audioFormat: 'eac3' })
    const format = page.get('#audio_format')

    expect(format.element.value).toBe('eac3')
    expect(format.element.disabled).toBe(false)
    expect(format.findAll('option').map(option => option.element.value)).toEqual(['opus', 'ac3', 'eac3'])
    expect(format.findAll('option').every(option => !option.element.disabled)).toBe(true)
    expect(page.get('#high_quality_audio').element.disabled).toBe(false)
    expect(page.vm.config).not.toHaveProperty('ac3_available')
    expect(page.vm.config).not.toHaveProperty('eac3_available')
  })

  it('preserves a saved Dolby selection when capability metadata is absent', async () => {
    const page = await mountConfig({ platform: 'linux', audioFormat: 'ac3' })
    const format = page.get('#audio_format')

    expect(format.element.value).toBe('ac3')
    expect(format.element.disabled).toBe(false)
    expect(format.get('option[value="ac3"]').element.disabled).toBe(false)
    expect(format.get('option[value="eac3"]').element.disabled).toBe(false)
  })

  it('only disables high quality audio while Opus is selected', async () => {
    const page = await mountConfig({ platform: 'macos', audioFormat: 'opus' })
    const quality = page.get('#high_quality_audio')

    expect(quality.element.disabled).toBe(true)
    await page.get('#audio_format').setValue('ac3')
    await page.vm.$nextTick()
    expect(quality.element.disabled).toBe(false)
  })
})
