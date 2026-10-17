import { afterEach, describe, expect, it, vi } from 'vitest'

import configDocumentation from '../../docs/configuration.md?raw'
import configurationShortcuts from '../../docs/configuration.js?raw'
import { mountAudioVideo } from './audioVideoMount.js'

afterEach(() => {
  vi.restoreAllMocks()
  document.body.innerHTML = ''
})

describe('externally managed audio configuration', () => {
  it('renders after the existing Windows audio controls and preserves its saved value', async () => {
    const { wrapper, config } = mountAudioVideo('windows')
    expect(wrapper.findAll('input').map(input => input.attributes('id'))).toEqual([
      'audio_sink',
      'virtual_sink',
      'install_steam_audio_drivers',
      'stream_audio',
      'external_audio',
      'keep_default_sink',
    ])
    expect(config.external_audio).toBe('disabled')
    expect(wrapper.get('#external_audio').element.checked).toBe(false)
    await wrapper.get('#external_audio').setValue(true)
    expect(config.external_audio).toBe('enabled')
    await wrapper.get('#external_audio').setValue(false)
    expect(config.external_audio).toBe('disabled')
    wrapper.unmount()
  })

  it.each(['linux', 'freebsd', 'macos'])('keeps the Windows-only option hidden on %s', platform => {
    const { wrapper } = mountAudioVideo(platform)
    expect(wrapper.find('#external_audio').exists()).toBe(false)
    expect(wrapper.find('#stream_audio').exists()).toBe(true)
    wrapper.unmount()
  })

  it('provides the standard documentation table and a working configuration shortcut', () => {
    const section = configDocumentation.split('### external_audio\n')[1].split('\n### ')[0]
    // Doxygen emits configuration headings as h2 and gives tables this class.
    document.body.innerHTML = '<input id="host-authority" value="localhost:47990">'
      + '<h2>external_audio</h2>' + section.replace('<table>', '<table class="doxtable">')
    const rows = [...document.querySelectorAll('tr')]
    expect(rows.map(row => row.cells[0].textContent.trim())).toEqual(['Description', 'Default', 'Example'])
    expect(rows[0].textContent).toContain('This option is only supported on Windows.')
    expect(rows[1].textContent).toContain('disabled')
    expect(rows[2].textContent).toContain('external_audio = enabled')

    new Function('document', configurationShortcuts)(document)
    document.dispatchEvent(new Event('DOMContentLoaded'))
    const button = document.querySelector('.open-button')
    expect(button.textContent).toBe('Open configuration')
    const open = vi.spyOn(window, 'open').mockImplementation(() => null)
    button.click()
    expect(open).toHaveBeenCalledWith('https://localhost:47990/config/#external_audio', '_blank', 'noopener')
  })
})
