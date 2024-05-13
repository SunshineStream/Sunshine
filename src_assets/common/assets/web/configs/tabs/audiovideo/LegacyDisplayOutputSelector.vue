<script setup>
import {reactive, ref} from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../PlatformLayout.vue'

const props = defineProps([
  'platform',
  'config'
])

const config = reactive(props.config)
const outputNamePlaceholder = (props.platform === 'windows') ? '\\.\DISPLAY1' : '0'
</script>

<template>
  <div class="mb-3">
    <label for="output_name" class="form-label">{{ $tp('config.output_name') }}</label>
    <input type="text" class="form-control" id="output_name" :placeholder="outputNamePlaceholder"
           v-model="config.output_name"/>
    <div class="form-text">
      {{ $tp('config.output_name_desc') }}<br>
      <PlatformLayout :platform="platform">
        <template #windows>
          <pre>tools\dxgi-info.exe</pre>
        </template>
        <template #linux>
            <pre style="white-space: pre-line;">
              Info: Detecting displays
              Info: Detected display: DVI-D-0 (id: 0) connected: false
              Info: Detected display: HDMI-0 (id: 1) connected: true
              Info: Detected display: DP-0 (id: 2) connected: true
              Info: Detected display: DP-1 (id: 3) connected: false
              Info: Detected display: DVI-D-1 (id: 4) connected: false
            </pre>
        </template>
        <template #macos>
            <pre style="white-space: pre-line;">
              Info: Detecting displays
              Info: Detected display: Monitor-0 (id: 3) connected: true
              Info: Detected display: Monitor-1 (id: 2) connected: true
            </pre>
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
