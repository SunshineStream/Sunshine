import { flushPromises, mount } from '@vue/test-utils'
import { ref } from 'vue'
import { afterEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))

import Config from '../../src_assets/common/assets/web/Config.vue'

async function mountConfig(serverConfig = {}, platform = 'windows') {
  const fetchMock = vi.fn(async (url, options) => {
    if (url === './api/config' && !options) {
      return { json: async () => ({ platform, version: 'test', status: true, ...serverConfig }) }
    }
    if (url === './api/config' && options?.method === 'POST') {
      return { status: 200, json: async () => ({ status: true }) }
    }
    throw new Error(`Unexpected fetch: ${url}`)
  })
  vi.stubGlobal('fetch', fetchMock)
  window.HTMLElement.prototype.scrollIntoView = vi.fn()

  const wrapper = mount(Config, {
    attachTo: document.body,
    global: {
      mocks: { $t: key => key },
      provide: {
        platform: ref(platform),
        i18n: { t: key => key },
      },
      stubs: {
        General: true,
        Inputs: true,
        Network: true,
        Files: true,
        Advanced: true,
        ContainerEncoders: true,
        AdapterNameSelector: true,
        DisplayOutputSelector: true,
        DisplayDeviceOptions: true,
        DisplayModesSettings: true,
        Checkbox: false,
        Check: true,
        Plus: true,
      },
    },
  })
  await flushPromises()
  return { wrapper, fetchMock }
}

function postsToConfig(fetchMock) {
  return fetchMock.mock.calls.filter(([url, options]) => url === './api/config' && options?.method === 'POST')
}

afterEach(() => {
  vi.unstubAllGlobals()
  vi.restoreAllMocks()
  document.body.innerHTML = ''
  delete window.HTMLElement.prototype.scrollIntoView
})

describe('config search reveal', () => {
  it('reveals matches nested inside grouping fieldsets', async () => {
    const { wrapper } = await mountConfig()
    document.body.insertAdjacentHTML('beforeend',
      '<div class="config-page">' +
        '<fieldset id="outer-group" class="mb-3"><div><input id="adapter_name" /></div></fieldset>' +
        '<div id="unrelated-group" class="mb-3"><input id="unrelated" /></div>' +
        '</div>')

    const input = document.getElementById('adapter_name')
    document.querySelectorAll('.config-page .mb-3').forEach(el => {
      el.style.display = 'none'
      el.dataset.searchHidden = '1'
    })
    wrapper.vm.revealSearchMatch(input)

    expect(document.getElementById('outer-group').style.display).toBe('')
    expect(document.getElementById('outer-group').dataset.searchHidden).toBeUndefined()
    expect(document.getElementById('unrelated-group').style.display).toBe('none')
    wrapper.unmount()
  })

  it('stops revealing at the page boundary', async () => {
    const { wrapper } = await mountConfig()
    document.body.insertAdjacentHTML('beforeend',
      '<div class="config-page"><div id="solo-group" class="mb-3"><input id="solo" /></div></div>')
    document.querySelectorAll('.config-page .mb-3').forEach(el => {
      el.style.display = 'none'
      el.dataset.searchHidden = '1'
    })

    wrapper.vm.revealSearchMatch(document.getElementById('solo'))

    expect(document.getElementById('solo-group').style.display).toBe('')
    expect(document.querySelector('.config-page').style.display).not.toBe('none')
    wrapper.unmount()
  })

  it('leaves ancestors hidden by something other than search alone', async () => {
    const { wrapper } = await mountConfig()
    document.body.insertAdjacentHTML('beforeend',
      '<div class="config-page"><fieldset id="plain-group" class="mb-3"><input id="deep" /></fieldset></div>')
    // Hidden without the search flag: not this search's doing.
    document.getElementById('plain-group').style.display = 'none'

    wrapper.vm.revealSearchMatch(document.getElementById('deep'))

    expect(document.getElementById('plain-group').style.display).toBe('none')
    wrapper.unmount()
  })
})

describe('save validation gate', () => {
  it('blocks the POST and alerts when audio capture is invalid', async () => {
    const { wrapper, fetchMock } = await mountConfig({ audio_sink: '', keep_default_sink: 'disabled' })
    wrapper.vm.currentTab = 'av'
    await wrapper.vm.$nextTick()
    await wrapper.get('#audio_capture_target_specific').setValue(true)

    const saved = await wrapper.vm.save()

    expect(saved).toBe(false)
    expect(postsToConfig(fetchMock)).toHaveLength(0)
    expect(wrapper.vm.saveBlocked).toBe('audio_capture')
    expect(wrapper.find('.alert-danger').exists()).toBe(true)
    wrapper.unmount()
  })

  it('saves normally when audio capture is valid', async () => {
    const { wrapper, fetchMock } = await mountConfig({ audio_sink: 'some-sink' })
    wrapper.vm.currentTab = 'av'
    await wrapper.vm.$nextTick()
    await wrapper.get('#audio_capture_target_specific').setValue(true)
    await wrapper.get('#audio_sink').setValue('named-target')

    const saved = await wrapper.vm.save()

    expect(saved).toBe(true)
    expect(postsToConfig(fetchMock)).toHaveLength(1)
    expect(wrapper.vm.saveBlocked).toBeNull()
    wrapper.unmount()
  })

  it('saves normally when the audio tab is not mounted', async () => {
    const { wrapper, fetchMock } = await mountConfig({ audio_sink: 'some-sink' })
    expect(wrapper.vm.currentTab).not.toBe('av')

    const saved = await wrapper.vm.save()

    expect(saved).toBe(true)
    expect(postsToConfig(fetchMock)).toHaveLength(1)
    wrapper.unmount()
  })

  it('clears a previous block on the next save attempt', async () => {
    const { wrapper } = await mountConfig({ audio_sink: '', keep_default_sink: 'disabled' })
    wrapper.vm.currentTab = 'av'
    await wrapper.vm.$nextTick()
    await wrapper.get('#audio_capture_target_specific').setValue(true)
    await wrapper.vm.save()
    expect(wrapper.vm.saveBlocked).toBe('audio_capture')

    await wrapper.get('#audio_sink').setValue('named-target')
    await wrapper.vm.save()

    expect(wrapper.vm.saveBlocked).toBeNull()
    expect(wrapper.find('.alert-danger').exists()).toBe(false)
    wrapper.unmount()
  })
})
