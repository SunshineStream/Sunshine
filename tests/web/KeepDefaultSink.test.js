import { mount } from '@vue/test-utils'
import { afterEach, describe, expect, it } from 'vitest'
import { ref } from 'vue'

import AudioVideo from '../../src_assets/common/assets/web/configs/tabs/AudioVideo.vue'
import configTabs from '../../src_assets/common/assets/web/configs/config_tabs.json'

function mountAudioVideo(platform) {
  const config = structuredClone(configTabs.find(tab => tab.id === 'av').options)
  const wrapper = mount(AudioVideo, {
    props: { platform, config },
    global: {
      mocks: { $t: key => key },
      provide: {
        platform: ref(platform),
        i18n: { t: key => key },
      },
      stubs: {
        AdapterNameSelector: true,
        DisplayOutputSelector: true,
        DisplayDeviceOptions: true,
        DisplayModesSettings: true,
      },
    },
  })
  return { wrapper, config }
}

afterEach(() => {
  document.body.innerHTML = ''
})

describe('keep default sink configuration', () => {
  it.each(['windows', 'linux', 'macos'])('renders the shared checkbox on %s and preserves its saved value', async platform => {
    const { wrapper, config } = mountAudioVideo(platform)
    expect(wrapper.find('#keep_default_sink').exists()).toBe(true)
    expect(config.keep_default_sink).toBe('disabled')
    expect(wrapper.get('#keep_default_sink').element.checked).toBe(false)
    await wrapper.get('#keep_default_sink').setValue(true)
    expect(config.keep_default_sink).toBe('enabled')
    await wrapper.get('#keep_default_sink').setValue(false)
    expect(config.keep_default_sink).toBe('disabled')
    wrapper.unmount()
  })
})
