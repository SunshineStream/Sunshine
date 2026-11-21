import { afterEach, describe, expect, it } from 'vitest'

import { mountAudioVideo } from './audioVideoMount.js'

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
