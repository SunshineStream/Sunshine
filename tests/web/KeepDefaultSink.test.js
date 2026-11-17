import { afterEach, describe, expect, it, vi } from 'vitest'

import configDocumentation from '../../docs/configuration.md?raw'
import configurationShortcuts from '../../docs/configuration.js?raw'
import { mountAudioVideo } from './audioVideoMount.js'
import en from '../../src_assets/common/assets/web/public/assets/locale/en.json'

afterEach(() => {
  vi.restoreAllMocks()
  document.body.innerHTML = ''
})

describe('keep default sink configuration', () => {
  it.each(['windows', 'linux', 'macos', 'freebsd'])('renders the capture target choices on %s', async platform => {
    const { wrapper, config } = mountAudioVideo(platform)
    expect(wrapper.find('#keep_default_sink').exists()).toBe(true)
    expect(wrapper.find('#external_audio').exists()).toBe(false)
    expect(wrapper.text()).toContain('config.audio_capture_target_silent')
    expect(wrapper.text()).toContain('config.audio_capture_target_current')
    expect(wrapper.text()).toContain('config.audio_capture_target_specific')

    const expectInsight = choice => {
      const insights = wrapper.findAll('#audio_capture_how_it_works')
      expect(insights).toHaveLength(1)
      expect(insights[0].text()).toContain('config.audio_capture_how_it_works')
      const ownerSunshine = choice === 'specific' && wrapper.find('#audio_capture_owner_sunshine').exists()
        && wrapper.get('#audio_capture_owner_sunshine').element.checked
      const expectedKey = ownerSunshine ? 'specific_sunshine' : choice
      expect(insights[0].text()).toContain(`config.audio_capture_target_${expectedKey}_desc_${platform}`)
      for (const otherChoice of ['silent', 'current', 'specific', 'specific_sunshine'].filter(candidate => candidate !== expectedKey)) {
        expect(insights[0].text()).not.toContain(`config.audio_capture_target_${otherChoice}_desc_${platform}`)
      }
      for (const other of ['windows', 'linux', 'macos', 'freebsd'].filter(candidate => candidate !== platform)) {
        expect(insights[0].text()).not.toContain(`config.audio_capture_target_${expectedKey}_desc_${other}`)
      }
    }

    if (platform === 'macos') {
      expect(wrapper.get('#audio_capture_target_silent').element.disabled).toBe(true)
      expect(wrapper.get('#audio_capture_target_silent').element.checked).toBe(false)
      expect(wrapper.text()).toContain('config.audio_capture_target_silent_desc_macos')
      expect(wrapper.get('#audio_capture_target_current').element.checked).toBe(true)
      expectInsight('current')
      // A blank sink mounts as Play on this PC here, so mount syncs the saved keys to match.
      expect(config.keep_default_sink).toBe('enabled')
      expect(config.audio_sink).toBe('')
    } else {
      expect(wrapper.get('#audio_capture_target_silent').element.checked).toBe(true)
      expectInsight('silent')
      expect(config.keep_default_sink).toBe('disabled')
      expect(config.audio_sink).toBe('')

      await wrapper.get('#audio_capture_target_current').setValue(true)
      expectInsight('current')
      expect(config.keep_default_sink).toBe('enabled')
      expect(config.audio_sink).toBe('')
      expect(wrapper.get('#audio_sink').element.disabled).toBe(true)
    }

    await wrapper.get('#audio_capture_target_specific').setValue(true)
    expectInsight('specific')
    // This flow enters Custom from Play on this PC too, so self is the owner.
    expect(config.keep_default_sink).toBe('enabled')
    if (platform !== 'macos') {
      expect(wrapper.get('#audio_capture_owner_self').element.checked).toBe(true)
    }
    expect(wrapper.get('#audio_sink').element.disabled).toBe(false)
    await wrapper.get('#audio_sink').setValue('named-target')
    expect(config.audio_sink).toBe('named-target')
    expect(config.keep_default_sink).toBe('enabled')

    if (platform !== 'macos') {
      await wrapper.get('#audio_capture_target_silent').setValue(true)
      expectInsight('silent')
      expect(config.keep_default_sink).toBe('disabled')
      expect(config.audio_sink).toBe('')
      expect(wrapper.get('#audio_sink').element.disabled).toBe(true)
      expect(wrapper.findAll('#audio_sink')).toHaveLength(1)
      expect(wrapper.get('#audio_sink').element.value).toBe('named-target')
      expect(wrapper.get('#audio_capture_effective_target').text()).toContain(`config.audio_capture_effective_target_silent_${platform}`)
      expect(wrapper.get('#audio_capture_effective_target').text()).toContain('config.audio_capture_effective_retained')
      expect(wrapper.find('#audio_capture_owner_sunshine').exists()).toBe(false)

      await wrapper.get('#audio_capture_target_specific').setValue(true)
      expectInsight('specific')
      expect(wrapper.get('#audio_sink').element.value).toBe('named-target')
      expect(config.audio_sink).toBe('named-target')
    } else {
      expect(wrapper.text()).toContain('config.audio_capture_target_silent_desc_macos')
    }
    wrapper.unmount()
  })

  it('renders only the shared audio choices on Windows', () => {
    const { wrapper } = mountAudioVideo('windows')
    expect(wrapper.findAll('input').map(input => input.attributes('id'))).toEqual([
      'stream_audio',
      'audio_capture_target_silent',
      'audio_capture_target_current',
      'audio_capture_target_specific',
      'audio_sink',
      'virtual_sink',
      'install_steam_audio_drivers',
    ])
    wrapper.unmount()
  })

  it('keeps a manually routed null sink editable as the specific target', async () => {
    const { wrapper, config } = mountAudioVideo('linux')
    await wrapper.get('#audio_capture_target_specific').setValue(true)
    await wrapper.get('#audio_capture_owner_self').setValue(true)
    await wrapper.get('#audio_sink').setValue('sink-sunshine-stereo')
    expect(config.keep_default_sink).toBe('enabled')
    expect(config.audio_sink).toBe('sink-sunshine-stereo')
    expect(wrapper.get('#audio_sink').element.disabled).toBe(false)
    expect(wrapper.get('#audio_sink').element.value).toBe('sink-sunshine-stereo')
    wrapper.unmount()
  })

  it('mounts a legacy disabled named sink as Custom with Sunshine as routing owner', async () => {
    const { wrapper, config } = mountAudioVideo('linux', { audio_sink: 'legacy-sink', keep_default_sink: 'disabled' })
    expect(wrapper.get('#audio_capture_target_specific').element.checked).toBe(true)
    expect(wrapper.get('#audio_capture_owner_sunshine').element.checked).toBe(true)
    expect(config.keep_default_sink).toBe('disabled')
    expect(config.audio_sink).toBe('legacy-sink')
    await wrapper.get('#audio_sink').setValue('renamed-sink')
    expect(config.keep_default_sink).toBe('disabled')
    expect(config.audio_sink).toBe('renamed-sink')
    wrapper.unmount()
  })

  it('mounts a legacy enabled named sink as Custom with self as routing owner', () => {
    const { wrapper, config } = mountAudioVideo('windows', { audio_sink: 'legacy-sink', keep_default_sink: 'enabled' })
    expect(wrapper.get('#audio_capture_target_specific').element.checked).toBe(true)
    expect(wrapper.get('#audio_capture_owner_self').element.checked).toBe(true)
    expect(config.keep_default_sink).toBe('enabled')
    expect(config.audio_sink).toBe('legacy-sink')
    wrapper.unmount()
  })

  it('defaults Custom entered directly from Quiet to Sunshine routing', async () => {
    const { wrapper, config } = mountAudioVideo('linux')
    await wrapper.get('#audio_capture_target_specific').setValue(true)
    expect(wrapper.get('#audio_capture_owner_sunshine').element.checked).toBe(true)
    expect(config.keep_default_sink).toBe('disabled')
    await wrapper.get('#audio_sink').setValue('named-target')
    expect(config.keep_default_sink).toBe('disabled')
    expect(config.audio_sink).toBe('named-target')
    wrapper.unmount()
  })

  it('defaults Custom entered from Play on this PC to self routing', async () => {
    const { wrapper, config } = mountAudioVideo('linux')
    await wrapper.get('#audio_capture_target_current').setValue(true)
    await wrapper.get('#audio_capture_target_specific').setValue(true)
    expect(wrapper.get('#audio_capture_owner_self').element.checked).toBe(true)
    expect(config.keep_default_sink).toBe('enabled')
    wrapper.unmount()
  })

  it('flips only keep_default_sink when the routing owner changes', async () => {
    const { wrapper, config } = mountAudioVideo('linux', { audio_sink: 'named-target', keep_default_sink: 'disabled' })
    await wrapper.get('#audio_capture_owner_self').setValue(true)
    expect(config.keep_default_sink).toBe('enabled')
    expect(config.audio_sink).toBe('named-target')
    await wrapper.get('#audio_capture_owner_sunshine').setValue(true)
    expect(config.keep_default_sink).toBe('disabled')
    expect(config.audio_sink).toBe('named-target')
    wrapper.unmount()
  })

  it('disables the audio controls when Stream Audio is off', async () => {
    const { wrapper } = mountAudioVideo('linux', { stream_audio: 'disabled' })
    expect(wrapper.get('#audio_capture_target_silent').element.disabled).toBe(true)
    expect(wrapper.get('#audio_capture_target_current').element.disabled).toBe(true)
    expect(wrapper.get('#audio_capture_target_specific').element.disabled).toBe(true)
    expect(wrapper.get('#audio_sink').element.disabled).toBe(true)
    await wrapper.get('#stream_audio').setValue(true)
    expect(wrapper.get('#audio_capture_target_silent').element.disabled).toBe(false)
    expect(wrapper.get('#audio_capture_target_current').element.disabled).toBe(false)
    wrapper.unmount()
  })

  it('warns when Custom device has no sink name', async () => {
    const { wrapper } = mountAudioVideo('linux')
    await wrapper.get('#audio_capture_target_specific').setValue(true)
    expect(wrapper.get('#audio_sink').classes()).toContain('is-invalid')
    expect(wrapper.text()).toContain('config.audio_capture_sink_needed')
    await wrapper.get('#audio_sink').setValue('named-target')
    expect(wrapper.get('#audio_sink').classes()).not.toContain('is-invalid')
    wrapper.unmount()
  })

  it('shows outcome subtitles and both capture section titles', () => {
    const { wrapper } = mountAudioVideo('linux')
    expect(wrapper.find('#video_capture').exists()).toBe(true)
    const audioSection = wrapper.get('#keep_default_sink').element
    const videoSection = wrapper.get('#video_capture').element
    expect(audioSection.compareDocumentPosition(videoSection) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
    expect(wrapper.text()).toContain('config.video_capture')
    expect(wrapper.text()).toContain('config.audio_capture_target_silent_outcome')
    expect(wrapper.text()).toContain('config.audio_capture_target_current_outcome')
    expect(wrapper.text()).toContain('config.audio_capture_target_specific_outcome')
    wrapper.unmount()
  })

  it('shows the Windows virtual sink as the Quiet effective target', () => {
    const { wrapper } = mountAudioVideo('windows', { virtual_sink: 'My Virtual Sink' })
    expect(wrapper.get('#audio_capture_effective_target').text()).toContain('My Virtual Sink')
    wrapper.unmount()
  })

  it('hides the routing owner choice on macOS', async () => {
    const { wrapper } = mountAudioVideo('macos', { audio_sink: 'BlackHole 2ch', keep_default_sink: 'enabled' })
    expect(wrapper.find('#audio_capture_owner_sunshine').exists()).toBe(false)
    expect(wrapper.find('#audio_capture_owner_self').exists()).toBe(false)
    expect(wrapper.text()).toContain('config.audio_capture_target_specific_sunshine_desc_macos')
    wrapper.unmount()
  })

  it('trims whitespace from a custom sink before saving it', async () => {
    const { wrapper, config } = mountAudioVideo('linux')
    await wrapper.get('#audio_capture_target_specific').setValue(true)
    await wrapper.get('#audio_sink').setValue('  sink-sunshine-stereo  ')
    expect(config.audio_sink).toBe('sink-sunshine-stereo')
    wrapper.unmount()
  })

  it('provides the standard documentation table and a working configuration shortcut', () => {
    const section = configDocumentation.split('### keep_default_sink\n')[1].split('\n### ')[0]
    // Doxygen emits configuration headings as h2 and gives tables this class.
    document.body.innerHTML = '<input id="host-authority" value="localhost:47990">'
      + '<h2>keep_default_sink</h2>' + section.replace('<table>', '<table class="doxtable">')
    const rows = [...document.querySelectorAll('tr')]
    expect(rows.map(row => row.cells[0].textContent.trim())).toEqual(['Description', 'Default', 'Example'])
    expect(rows[1].textContent).toContain('disabled')
    expect(rows[2].textContent).toContain('keep_default_sink = enabled')

    new Function('document', configurationShortcuts)(document)
    document.dispatchEvent(new Event('DOMContentLoaded'))
    const button = document.querySelector('.open-button')
    expect(button.textContent).toBe('Open configuration')
    const open = vi.spyOn(window, 'open').mockImplementation(() => null)
    button.click()
    expect(open).toHaveBeenCalledWith('https://localhost:47990/config/#keep_default_sink', '_blank', 'noopener')
  })
})

describe('audio sink platform guidance', () => {
  it('describes silent, current, and specific capture in platform terms', () => {
    expect(en.config.video_capture).toBe('Video Capture')
    expect(en.config.audio_capture_target_silent_outcome).toBe('Silent on this PC, sound on the client')
    expect(en.config.audio_capture_target_current_outcome).toBe('Sound on this PC and on the client')
    expect(en.config.audio_capture_target_specific_outcome).toBe('Capture one exact device; you route apps to it')
    expect(en.config.audio_capture_sink_needed).toBeDefined()
    expect(en.config.audio_capture_sink_kept_hint).toBeDefined()
    expect(en.config.audio_capture_target_silent).toBe('Quiet streaming')
    expect(en.config.audio_capture_target_current).toBe('Play on this PC too')
    expect(en.config.audio_capture_target_specific).toBe('Custom device')
    expect(en.config.audio_capture_how_it_works).toBe('How it works here')
    expect(en.config.audio_sink_placeholder_linux).toBe('sink-sunshine-stereo')
    expect(en.config.audio_sink_placeholder_freebsd).toBe('sink-sunshine-stereo')
    expect(en.config.audio_sink_placeholder_macos).toBe('BlackHole 2ch')
    expect(en.config.audio_sink_placeholder_windows).toBe('Speakers (High Definition Audio Device)')
    for (const platform of ['linux', 'freebsd', 'macos', 'windows']) {
      expect(en.config[`audio_sink_desc_${platform}`]).toBeDefined()
      expect(en.config[`audio_capture_target_current_desc_${platform}`]).toBeDefined()
      expect(en.config[`audio_capture_target_specific_desc_${platform}`]).toBeDefined()
    }
    for (const platform of ['linux', 'freebsd', 'windows']) {
      expect(en.config[`audio_capture_target_silent_desc_${platform}`]).toMatch(/client hears/i)
    }
    expect(en.config.audio_capture_target_silent_desc_linux).toMatch(/null sink/)
    expect(en.config.audio_capture_target_silent_desc_windows).toMatch(/Steam Streaming Speakers/)
    expect(en.config.audio_capture_target_silent_desc_macos).toMatch(/BlackHole/)
    expect(en.config.audio_capture_target_silent_desc_macos).toMatch(/Not available/)
    expect(en.config.audio_capture_target_current_desc_macos).toMatch(/system audio tap/)
    expect(en.config.audio_capture_owner_sunshine).toBe('Sunshine automatically sends all sound to the device configured below while streaming')
    expect(en.config.audio_capture_owner_self).toBe('I route apps myself')
    expect(en.config.audio_capture_effective_target).toBe('Streaming will capture')
    expect(en.config.audio_capture_effective_retained).toBe('Custom name kept for Custom device only')
    for (const platform of ['linux', 'freebsd', 'macos', 'windows']) {
      expect(en.config[`audio_capture_target_specific_sunshine_desc_${platform}`]).toBeDefined()
      expect(en.config[`audio_capture_effective_target_silent_${platform}`]).toBeDefined()
      expect(en.config[`audio_capture_effective_target_current_${platform}`]).toBeDefined()
    }
    expect(en.config.audio_capture_effective_target_silent_linux).toMatch(/null sink/)
    expect(en.config.audio_capture_target_specific_sunshine_desc_macos).toMatch(/Not applicable/)
    expect(en.config.audio_sink_desc_macos).not.toMatch(/Soundflower/)
    expect(en.config.virtual_sink_desc).toMatch(/Steam Streaming Speakers/)
    expect(en.config.virtual_sink_desc).toMatch(/Leave blank/)
    expect(en.config.install_steam_audio_drivers_desc).toMatch(/Windows only/)
    expect(en.config.install_steam_audio_drivers_desc).toMatch(/Steam Streaming Speakers/)
  })
})
