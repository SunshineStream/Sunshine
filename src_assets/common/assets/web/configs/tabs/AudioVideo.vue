<script setup>
import {reactive, ref} from 'vue'
import {$tp} from '../../platform-i18n'
import PlatformLayout from '../../PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import LegacyDisplayOutputSelector from './audiovideo/LegacyDisplayOutputSelector.vue'
import NewDisplayOutputSelector from './audiovideo/NewDisplayOutputSelector.vue'
import DisplayDeviceOptions from "./audiovideo/DisplayDeviceOptions.vue";
import DisplayModesSettings from "./audiovideo/DisplayModesSettings.vue";

const props = defineProps([
  'platform',
  'config',
  'resolutions',
  'fps',
  'displays'
])

const config = reactive(props.config)
</script>

<template>
  <div id="audio-video" class="config-page">
    <!-- Audio Sink -->
    <div class="mb-3">
      <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
      <input type="text" class="form-control" id="audio_sink"
             :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
             v-model="config.audio_sink" />
      <div class="form-text">
        {{ $tp('config.audio_sink_desc') }}<br>
        <PlatformLayout :platform="platform">
          <template #windows>
            <pre>tools\audio-info.exe</pre>
          </template>
          <template #linux>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #macos>
            <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br>
            <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
          </template>
        </PlatformLayout>
      </div>
    </div>


    <PlatformLayout :platform="platform">
      <template #windows>
        <!-- Virtual Sink -->
        <div class="mb-3">
          <label for="virtual_sink" class="form-label">{{ $t('config.virtual_sink') }}</label>
          <input type="text" class="form-control" id="virtual_sink" :placeholder="$t('config.virtual_sink_placeholder')"
                 v-model="config.virtual_sink" />
          <div class="form-text">{{ $t('config.virtual_sink_desc') }}</div>
        </div>

        <!-- Install Steam Audio Drivers -->
        <div class="mb-3">
          <label for="install_steam_audio_drivers" class="form-label">{{ $t('config.install_steam_audio_drivers') }}</label>
          <select id="install_steam_audio_drivers" class="form-select" v-model="config.install_steam_audio_drivers">
            <option value="disabled">{{ $t('_common.disabled') }}</option>
            <option value="enabled">{{ $t('_common.enabled_def') }}</option>
          </select>
          <div class="form-text">{{ $t('config.install_steam_audio_drivers_desc') }}</div>
        </div>
      </template>
    </PlatformLayout>

    <AdapterNameSelector
        :platform="platform"
        :config="config"
    />

    <NewDisplayOutputSelector
      :platform="platform"
      :config="config"
      :displays="displays"
    />

    <!-- Display Modes -->
    <DisplayModesSettings
        :platform="platform"
        :config="config"
        :resolutions="resolutions"
        :fps="fps"
    />

  </div>
</template>
