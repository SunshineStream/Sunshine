<script setup>
import { ref, watch } from 'vue'
import PlatformLayout from '../../PlatformLayout.vue'
import Checkbox from "../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config',
  'openVhfSupported'
])

const config = ref(props.config)

const vigembusGamepads = new Set(['auto', 'x360', 'ds4'])
const openVhfGamepads = new Set(['auto', 'xone', 'xseries', 'ds4', 'ds5', 'switch'])

watch(
  () => config.value.gamepad_driver,
  (gamepadDriver) => {
    const supported = gamepadDriver === 'vigembus' ? vigembusGamepads : gamepadDriver === 'open_vhf' ? openVhfGamepads : null
    if (props.platform === 'windows' && supported && !supported.has(config.value.gamepad)) {
      config.value.gamepad = 'auto'
    }
  },
  { immediate: true },
)
</script>

<template>
  <div id="input" class="config-page">
    <!-- Enable Gamepad Input -->
    <Checkbox class="mb-3"
              id="controller"
              locale-prefix="config"
              v-model="config.controller"
              default="true"
    ></Checkbox>

    <!-- Windows virtual gamepad driver policy -->
    <div class="mb-3" v-if="platform === 'windows'">
      <label for="gamepad_driver" class="form-label">{{ $t('config.gamepad_driver') }}</label>
      <select id="gamepad_driver" class="form-select" v-model="config.gamepad_driver" required>
        <option value="" disabled>{{ $t('config.gamepad_driver_select') }}</option>
        <option value="all">{{ $t('config.gamepad_driver_all') }}</option>
        <option v-if="openVhfSupported" value="open_vhf">{{ $t('config.gamepad_driver_open_vhf') }}</option>
        <option value="virtualhid">{{ $t('config.gamepad_driver_virtualhid') }}</option>
        <option value="vigembus">{{ $t('config.gamepad_driver_vigembus') }}</option>
      </select>
      <div class="form-text">{{ $t(openVhfSupported ? 'config.gamepad_driver_open_vhf_desc' : 'config.gamepad_driver_desc') }}</div>
    </div>

    <!-- Emulated Gamepad Type -->
    <div class="mb-3" v-if="config.controller === 'enabled' && platform !== 'macos'">
      <label for="gamepad" class="form-label">{{ $t('config.gamepad') }}</label>
      <select id="gamepad" class="form-select" v-model="config.gamepad">
        <option value="auto">{{ $t('_common.auto') }}</option>

        <PlatformLayout :platform="platform">
          <template #freebsd>
            <option value="generic">{{ $t("config.gamepad_generic") }}</option>
            <option value="x360">{{ $t('config.gamepad_x360') }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
            <option value="xseries">{{ $t("config.gamepad_xseries") }}</option>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
            <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
          </template>

          <template #linux>
            <option value="generic">{{ $t("config.gamepad_generic") }}</option>
            <option value="x360">{{ $t('config.gamepad_x360') }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
            <option value="xseries">{{ $t("config.gamepad_xseries") }}</option>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
            <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
          </template>

          <template #windows>
            <option v-if="config.gamepad_driver !== 'vigembus' && config.gamepad_driver !== 'open_vhf'" value="generic">{{ $t("config.gamepad_generic") }}</option>
            <option v-if="config.gamepad_driver !== 'open_vhf'" value="x360">{{ $t('config.gamepad_x360') }}</option>
            <option v-if="config.gamepad_driver !== 'vigembus'" value="xone">{{ $t("config.gamepad_xone") }}</option>
            <option v-if="config.gamepad_driver !== 'vigembus'" value="xseries">{{ $t("config.gamepad_xseries") }}</option>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
            <option v-if="config.gamepad_driver !== 'vigembus'" value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option v-if="config.gamepad_driver !== 'vigembus'" value="switch">{{ $t("config.gamepad_switch") }}</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">{{ $t('config.gamepad_desc') }}</div>
    </div>

    <!-- Additional options based on gamepad type -->
    <template v-if="config.controller === 'enabled'">
      <template v-if="config.gamepad === 'ds4' || config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform !== 'macos')">
        <div class="mb-3 accordion">
          <div class="accordion-item">
            <h2 class="accordion-header">
              <button class="accordion-button" type="button" data-bs-toggle="collapse"
                      data-bs-target="#panelsStayOpen-collapseOne">
                {{ $t(config.gamepad === 'auto' ? 'config.gamepad_auto' : 'config.gamepad_ds4_manual') }}
              </button>
            </h2>
            <div id="panelsStayOpen-collapseOne" class="accordion-collapse collapse show"
                 aria-labelledby="panelsStayOpen-headingOne">
              <div class="accordion-body">
                <!-- Automatic PlayStation-style detection options -->
                <template v-if="config.gamepad === 'auto' && (platform === 'windows' || platform === 'linux')">
                  <!-- Gamepad with motion capability as a PlayStation-style controller -->
                  <Checkbox class="mb-3"
                            id="motion_as_ds4"
                            locale-prefix="config"
                            v-model="config.motion_as_ds4"
                            default="true"
                  ></Checkbox>
                  <!-- Gamepad with touch capability as a PlayStation-style controller -->
                  <Checkbox class="mb-3"
                            id="touchpad_as_ds4"
                            locale-prefix="config"
                            v-model="config.touchpad_as_ds4"
                            default="true"
                  ></Checkbox>
                </template>
                <!-- PlayStation-style option: Back/Select as touchpad click -->
                <template v-if="config.gamepad === 'ds4' || config.gamepad === 'ds5' || config.gamepad === 'auto'">
                  <Checkbox class="mb-3"
                            id="ds4_back_as_touchpad_click"
                            locale-prefix="config"
                            v-model="config.ds4_back_as_touchpad_click"
                            default="true"
                  ></Checkbox>
                </template>
                <!-- Virtual HID option: Controller MAC randomization -->
                <template v-if="config.gamepad_driver !== 'vigembus' && config.gamepad_driver !== 'open_vhf' && (config.gamepad === 'ds4' || config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform !== 'macos'))">
                  <Checkbox class="mb-3"
                            id="virtualhid_randomize_mac"
                            locale-prefix="config"
                            v-model="config.virtualhid_randomize_mac"
                            default="true"
                  ></Checkbox>
                </template>
              </div>
            </div>
          </div>
        </div>
      </template>
    </template>

    <!-- Home/Guide Button Emulation Timeout -->
    <div class="mb-3" v-if="config.controller === 'enabled'">
      <label for="back_button_timeout" class="form-label">{{ $t('config.back_button_timeout') }}</label>
      <input type="text" class="form-control" id="back_button_timeout" placeholder="-1"
             v-model="config.back_button_timeout" />
      <div class="form-text">{{ $t('config.back_button_timeout_desc') }}</div>
    </div>

    <!-- Enable Keyboard Input -->
    <hr>
    <Checkbox class="mb-3"
              id="keyboard"
              locale-prefix="config"
              v-model="config.keyboard"
              default="true"
    ></Checkbox>

    <!-- Key Repeat Delay-->
    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_delay" class="form-label">{{ $t('config.key_repeat_delay') }}</label>
      <input type="text" class="form-control" id="key_repeat_delay" placeholder="500"
             v-model="config.key_repeat_delay" />
      <div class="form-text">{{ $t('config.key_repeat_delay_desc') }}</div>
    </div>

    <!-- Key Repeat Frequency-->
    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_frequency" class="form-label">{{ $t('config.key_repeat_frequency') }}</label>
      <input type="text" class="form-control" id="key_repeat_frequency" placeholder="24.9"
             v-model="config.key_repeat_frequency" />
      <div class="form-text">{{ $t('config.key_repeat_frequency_desc') }}</div>
    </div>

    <!-- Always send scancodes -->
    <Checkbox v-if="config.keyboard === 'enabled' && platform === 'windows'"
              class="mb-3"
              id="always_send_scancodes"
              locale-prefix="config"
              v-model="config.always_send_scancodes"
              default="true"
    ></Checkbox>

    <!-- Mapping Key AltRight to Key Windows -->
    <Checkbox v-if="config.keyboard === 'enabled'"
              class="mb-3"
              id="key_rightalt_to_key_win"
              locale-prefix="config"
              v-model="config.key_rightalt_to_key_win"
              default="false"
    ></Checkbox>

    <!-- Enable Mouse Input -->
    <hr>
    <Checkbox class="mb-3"
              id="mouse"
              locale-prefix="config"
              v-model="config.mouse"
              default="true"
    ></Checkbox>

    <!-- High resolution scrolling support -->
    <Checkbox v-if="config.mouse === 'enabled'"
              class="mb-3"
              id="high_resolution_scrolling"
              locale-prefix="config"
              v-model="config.high_resolution_scrolling"
              default="true"
    ></Checkbox>

    <!-- Native pen/touch support -->
    <Checkbox v-if="config.mouse === 'enabled'"
              class="mb-3"
              id="native_pen_touch"
              locale-prefix="config"
              v-model="config.native_pen_touch"
              default="true"
    ></Checkbox>
  </div>
</template>

<style scoped>

</style>
