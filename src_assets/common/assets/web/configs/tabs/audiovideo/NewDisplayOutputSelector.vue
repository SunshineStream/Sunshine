<script setup>
import { reactive, ref } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../PlatformLayout.vue'
import LegacyDisplayOutputSelector from './LegacyDisplayOutputSelector.vue'

const props = defineProps({
  platform: String,
  config: Object,
  displays: Array
})

const config = reactive(props.config)

const selectedDisplay = ref(getDisplaySelected(props.config.output_name))

function getDisplaySelected(id) {
  if (!id || !props.displays) { return null }
  for (let i = 0; i < props.displays.length; i++) {
    if (props.displays[i].id === id) {
      return props.displays[i]
    }
  }
  return null
}

function updatedSelected(id) {
  selectedDisplay.value = getDisplaySelected(id)
}

function isPrimary(display) {
  const origin = display.current_settings.origin
  return origin.x === 0 && origin.y === 0;
}

function resolution(display) {
  const resolution = display.current_settings.mode.resolution
  return `${resolution.width}x${resolution.height}`;
}
</script>

<template>
  <LegacyDisplayOutputSelector
      v-if="!displays || platform === 'linux'"
      :platform="platform"
      :config="config"
  />

  <div class="mb-3" v-if="displays">
    <label for="output_name" class="form-label">{{ $tp('config.output_name', $t('config.output_name_linux')) }}</label>
    <select id="output_name" class="form-select" v-model="config.output_name" v-on:change="updatedSelected(config.output_name)">
      <option value="">Default</option>
      <option v-for="display in displays" :value="display.id">
        {{ display.id }}: {{ display.friendly_name }} {{ resolution(display) }}<template v-if='isPrimary(display)'>, primary</template>
      </option>
    </select>
    <div class="form-text">
      <PlatformLayout :platform="platform">
        <template #windows>
          <p style="white-space: pre-line">{{ $tp('config.output_name_desc', $t('config.output_name_desc_linux')) }}</p>
          <b>&nbsp;&nbsp;&nbsp;&nbsp;DEVICE ID: {de9bb7e2-186e-505b-9e93-f48793333810}</b><br>
          <b>&nbsp;&nbsp;&nbsp;&nbsp;DISPLAY NAME: \\.\DISPLAY1</b><br>
          <b>&nbsp;&nbsp;&nbsp;&nbsp;FRIENDLY NAME: ROG PG279Q</b><br>
          <b>&nbsp;&nbsp;&nbsp;&nbsp;DEVICE STATE: PRIMARY</b><br>
          <b>&nbsp;&nbsp;&nbsp;&nbsp;HDR STATE: UNKNOWN</b>
        </template>

        <template #linux>
        </template>

        <template #macos>
          <div v-if="selectedDisplay">
            <b>&nbsp;&nbsp;&nbsp;&nbsp;DEVICE ID: {{ selectedDisplay.id }}</b><br>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;FRIENDLY NAME: {{ selectedDisplay.friendly_name }}</b><br>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;PRIMARY DEVICE: {{ isPrimary(selectedDisplay) }}</b><br>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;RAW DEVICE DATA:</b><br>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;{{ JSON.stringify(selectedDisplay) }}</b><br>
          </div>
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
