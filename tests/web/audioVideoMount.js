import { mount } from '@vue/test-utils'
import { ref } from 'vue'

import AudioVideo from '../../src_assets/common/assets/web/configs/tabs/AudioVideo.vue'
import configTabs from '../../src_assets/common/assets/web/configs/config_tabs.json'

export function mountAudioVideo(platform) {
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
