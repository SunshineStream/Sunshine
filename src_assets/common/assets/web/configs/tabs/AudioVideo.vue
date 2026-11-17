<script setup>
import {computed, ref, watch} from 'vue'
import {$tp} from '../../platform-i18n'
import {isConfigBoolEnabled} from '../../config_bool'
import PlatformLayout from '../../PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import DisplayOutputSelector from './audiovideo/DisplayOutputSelector.vue'
import DisplayDeviceOptions from "./audiovideo/DisplayDeviceOptions.vue";
import DisplayModesSettings from "./audiovideo/DisplayModesSettings.vue";
import Checkbox from "../../Checkbox.vue";

const props = defineProps({
  platform: String,
  config: Object,
})

const config = ref(props.config)

const silentCaptureDisabled = computed(() => props.platform === 'macos')
const streamAudioEnabled = computed(() => config.value.stream_audio === undefined ? true : isConfigBoolEnabled(config.value.stream_audio))
const configuredSink = typeof config.value.audio_sink === 'string' ? config.value.audio_sink : ''
const specificSink = ref(configuredSink)
let initialTarget = 'silent'
if (configuredSink.trim() !== '') {
  initialTarget = 'specific'
} else if (props.platform === 'macos' || isConfigBoolEnabled(config.value.keep_default_sink)) {
  initialTarget = 'current'
}
const captureTarget = ref(initialTarget)
let initialOwner = 'sunshine'
if (configuredSink.trim() !== '' && isConfigBoolEnabled(config.value.keep_default_sink)) {
  initialOwner = 'self'
}
const captureOwner = ref(initialOwner)
const specificSinkMissing = computed(() => captureTarget.value === 'specific' && specificSink.value.trim() === '')

const howItWorksKey = computed(() => captureTarget.value === 'specific' && captureOwner.value === 'sunshine'
  ? `config.audio_capture_target_specific_sunshine_desc_${props.platform}`
  : `config.audio_capture_target_${captureTarget.value}_desc_${props.platform}`)
const effectiveTargetKey = computed(() => `config.audio_capture_effective_target_${captureTarget.value}_${props.platform}`)

watch(captureTarget, (target, previousTarget) => {
  if (target === 'specific') {
    captureOwner.value = previousTarget === 'current' ? 'self' : 'sunshine'
  }
})

/**
 * @brief Write the selected capture target back to the saved config keys.
 *
 * Single writer for keep_default_sink and audio_sink: every selection change
 * funnels through here instead of cascading watchers writing the same keys.
 */
function syncCaptureConfig() {
  if (captureTarget.value === 'silent') {
    config.value.keep_default_sink = 'disabled'
    config.value.audio_sink = ''
  } else if (captureTarget.value === 'current') {
    config.value.keep_default_sink = 'enabled'
    config.value.audio_sink = ''
  } else {
    config.value.keep_default_sink = captureOwner.value === 'self' ? 'enabled' : 'disabled'
    config.value.audio_sink = specificSink.value.trim()
  }
}

watch([captureTarget, captureOwner, specificSink], syncCaptureConfig)

// Normalize platform-forced selections back to the saved keys on mount: macOS
// has no Quiet path, so a blank sink shows Play on this PC here even when the
// saved keys still describe Quiet streaming.
syncCaptureConfig()
</script>

<template>
  <div id="audio-video" class="config-page">
    <!-- Audio Capture Target -->
    <fieldset id="keep_default_sink" class="mb-3">
      <legend class="form-label">{{ $t('config.audio_capture_target') }}</legend>

      <!-- Stream Audio -->
      <Checkbox class="mb-3"
                id="stream_audio"
                locale-prefix="config"
                v-model="config.stream_audio"
                default="true"
      ></Checkbox>

      <fieldset :disabled="!streamAudioEnabled" class="border-0 p-0 m-0">

      <div class="form-check mb-2">
        <input class="form-check-input" type="radio" id="audio_capture_target_silent" value="silent" v-model="captureTarget" :disabled="!streamAudioEnabled || silentCaptureDisabled" />
        <label class="form-check-label" for="audio_capture_target_silent">
          {{ $t('config.audio_capture_target_silent') }}
          <span class="form-text d-block">{{ $t('config.audio_capture_target_silent_outcome') }}</span>
          <span v-if="platform === 'macos'" class="form-text d-block">{{ $t('config.audio_capture_target_silent_desc_macos') }}</span>
        </label>
      </div>

      <div class="form-check mb-2">
        <input class="form-check-input" type="radio" id="audio_capture_target_current" value="current" v-model="captureTarget" :disabled="!streamAudioEnabled" />
        <label class="form-check-label" for="audio_capture_target_current">
          {{ $t('config.audio_capture_target_current') }}
          <span class="form-text d-block">{{ $t('config.audio_capture_target_current_outcome') }}</span>
        </label>
      </div>

      <div class="form-check mb-2">
        <input class="form-check-input" type="radio" id="audio_capture_target_specific" value="specific" v-model="captureTarget" :disabled="!streamAudioEnabled" />
        <label class="form-check-label" for="audio_capture_target_specific">
          {{ $t('config.audio_capture_target_specific') }}
          <span class="form-text d-block">{{ $t('config.audio_capture_target_specific_outcome') }}</span>
        </label>
      </div>

      <p id="audio_capture_how_it_works" class="form-text text-muted">{{ $t('config.audio_capture_how_it_works') }}: {{ $t(howItWorksKey) }}</p>

      <div v-if="captureTarget === 'specific'" class="mt-3">
        <div class="form-check mb-2" v-if="platform !== 'macos'">
          <input class="form-check-input" type="radio" id="audio_capture_owner_sunshine" value="sunshine" v-model="captureOwner" :disabled="!streamAudioEnabled" />
          <label class="form-check-label" for="audio_capture_owner_sunshine">{{ $t('config.audio_capture_owner_sunshine') }}</label>
        </div>
        <div class="form-check mb-2" v-if="platform !== 'macos'">
          <input class="form-check-input" type="radio" id="audio_capture_owner_self" value="self" v-model="captureOwner" :disabled="!streamAudioEnabled" />
          <label class="form-check-label" for="audio_capture_owner_self">{{ $t('config.audio_capture_owner_self') }}</label>
        </div>
        <p v-if="platform === 'macos'" class="form-text">{{ $t('config.audio_capture_target_specific_sunshine_desc_macos') }}</p>
      </div>

      <p id="audio_capture_effective_target" v-if="captureTarget !== 'specific'" class="form-text">
        {{ $t('config.audio_capture_effective_target') }}: {{ captureTarget === 'silent' && platform === 'windows' && config.virtual_sink && config.virtual_sink.trim() ? config.virtual_sink : $t(effectiveTargetKey) }}<span v-if="specificSink.trim() !== ''"> — {{ $t('config.audio_capture_effective_retained') }}</span>
      </p>

      <div class="mt-3">
        <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
        <input type="text" class="form-control" id="audio_sink"
               :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
               :disabled="captureTarget !== 'specific' || !streamAudioEnabled"
               :class="{'is-invalid': specificSinkMissing}"
               :aria-invalid="specificSinkMissing"
               :value="specificSink"
               @input="specificSink = $event.target.value" />
        <div v-if="specificSinkMissing" class="invalid-feedback d-block">{{ $t('config.audio_capture_sink_needed') }}</div>
        <div v-else-if="captureTarget !== 'specific' && specificSink.trim() !== ''" class="form-text">{{ $t('config.audio_capture_sink_kept_hint') }}</div>
        <div class="form-text">
          {{ $tp('config.audio_sink_desc') }}<br>
          <PlatformLayout :platform="platform">
            <template #windows>
              <pre>tools\audio-info.exe</pre>
            </template>
            <template #freebsd>
              <pre>pacmd list-sinks | grep "name:"</pre>
              <pre>pactl list short sinks</pre>
            </template>
            <template #linux>
              <pre>pacmd list-sinks | grep "name:"</pre>
              <pre>pactl list short sinks</pre>
            </template>
            <template #macos>
              <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
            </template>
          </PlatformLayout>
        </div>
      </div>
      </fieldset>
    </fieldset>

    <PlatformLayout :platform="platform">
      <template #windows>
        <!-- Virtual Sink -->
        <div class="mb-3" v-if="captureTarget === 'silent'">
          <label for="virtual_sink" class="form-label">{{ $t('config.virtual_sink') }}</label>
          <input type="text" class="form-control" id="virtual_sink" :placeholder="$t('config.virtual_sink_placeholder')"
                 :disabled="!streamAudioEnabled"
                 v-model="config.virtual_sink" />
          <div class="form-text">{{ $t('config.virtual_sink_desc') }}</div>
        </div>

        <!-- Install Steam Audio Drivers -->
        <fieldset :disabled="!streamAudioEnabled" class="border-0 p-0 m-0">
          <Checkbox class="mb-3" v-if="captureTarget === 'silent'"
                    id="install_steam_audio_drivers"
                    locale-prefix="config"
                    v-model="config.install_steam_audio_drivers"
                    default="true"
          ></Checkbox>
        </fieldset>
      </template>
    </PlatformLayout>

    <!-- Video Capture -->
    <fieldset id="video_capture" class="mb-3">
      <legend class="form-label">{{ $t('config.video_capture') }}</legend>

      <AdapterNameSelector
          :platform="platform"
          :config="config"
      />

      <DisplayOutputSelector
        :platform="platform"
        :config="config"
      />

      <DisplayDeviceOptions
        :platform="platform"
        :config="config"
      />

      <!-- Display Modes -->
      <DisplayModesSettings
          :platform="platform"
          :config="config"
      />
    </fieldset>

  </div>
</template>
