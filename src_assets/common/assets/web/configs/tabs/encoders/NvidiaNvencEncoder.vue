<script setup>
import { ref } from 'vue'

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div id="nvidia-nvenc-encoder" class="config-page">
    <!-- Performance preset -->
    <div class="mb-3">
      <label for="nvenc_preset" class="form-label">{{ $t('config.nvenc_preset') }}</label>
      <select id="nvenc_preset" class="form-select" v-model="config.nvenc_preset">
        <option value="1">P1 {{ $t('config.nvenc_preset_1') }}</option>
        <option value="2">P2</option>
        <option value="3">P3</option>
        <option value="4">P4</option>
        <option value="5">P5</option>
        <option value="6">P6</option>
        <option value="7">P7 {{ $t('config.nvenc_preset_7') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_preset_desc') }}</div>
    </div>

    <!-- Two-pass mode -->
    <div class="mb-3">
      <label for="nvenc_twopass" class="form-label">{{ $t('config.nvenc_twopass') }}</label>
      <select id="nvenc_twopass" class="form-select" v-model="config.nvenc_twopass">
        <option value="disabled">{{ $t('config.nvenc_twopass_disabled') }}</option>
        <option value="quarter_res">{{ $t('config.nvenc_twopass_quarter_res') }}</option>
        <option value="full_res">{{ $t('config.nvenc_twopass_full_res') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_twopass_desc') }}</div>
    </div>

    <!-- Spatial AQ -->
    <div class="mb-3">
      <label for="nvenc_spatial_aq" class="form-label">{{ $t('config.nvenc_spatial_aq') }}</label>
      <select id="nvenc_spatial_aq" class="form-select" v-model="config.nvenc_spatial_aq">
        <option value="disabled">{{ $t('config.nvenc_spatial_aq_disabled') }}</option>
        <option value="enabled">{{ $t('config.nvenc_spatial_aq_enabled') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_spatial_aq_desc') }}</div>
    </div>

    <!-- Single-frame VBV/HRD percentage increase -->
    <div class="mb-3">
      <label for="nvenc_vbv_increase" class="form-label">{{ $t('config.nvenc_vbv_increase') }}</label>
      <input type="number" min="0" max="400" class="form-control" id="nvenc_vbv_increase" placeholder="0"
             v-model="config.nvenc_vbv_increase" />
      <div class="form-text">
        {{ $t('config.nvenc_vbv_increase_desc') }}<br>
        <br>
        <a href="https://en.wikipedia.org/wiki/Video_buffering_verifier">VBV/HRD</a>
      </div>
    </div>

    <!-- Miscellaneous options -->
    <div class="accordion">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button class="accordion-button" type="button" data-bs-toggle="collapse"
                  data-bs-target="#panelsStayOpen-collapseOne">
            {{ $t('config.misc') }}
          </button>
        </h2>
        <div id="panelsStayOpen-collapseOne" class="accordion-collapse collapse show"
             aria-labelledby="panelsStayOpen-headingOne">
          <div class="accordion-body">
            <!-- NVENC Realtime HAGS priority -->
            <div class="mb-3" v-if="platform === 'windows'">
              <label for="nvenc_realtime_hags" class="form-label">{{ $t('config.nvenc_realtime_hags') }}</label>
              <select id="nvenc_realtime_hags" class="form-select" v-model="config.nvenc_realtime_hags">
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="enabled">{{ $t('_common.enabled_def') }}</option>
              </select>
              <div class="form-text">
                {{ $t('config.nvenc_realtime_hags_desc') }}<br>
                <br>
                <a href="https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/">HAGS</a>
              </div>
            </div>

            <!-- Split frame encoding -->
            <div class="mb-3" v-if="platform === 'windows'">
              <label for="nvenc_split_encode" class="form-label">{{ $t('config.nvenc_split_encode') }}</label>
              <select id="nvenc_split_encode" class="form-select" v-model="config.nvenc_split_encode">
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="driver_decides">{{ $t('config.nvenc_split_encode_driver_decides_def') }}</option>
                <option value="enabled">{{ $t('_common.enabled') }}</option>
              </select>
              <div class="form-text">{{ $t('config.nvenc_split_encode_desc') }}</div>
            </div>

            <!-- Prefer lower encoding latency over power savings -->
            <div class="mb-3" v-if="platform === 'windows'">
              <label for="nvenc_latency_over_power" class="form-label">{{ $t('config.nvenc_latency_over_power') }}</label>
              <select id="nvenc_latency_over_power" class="form-select" v-model="config.nvenc_latency_over_power">
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="enabled">{{ $t('_common.enabled_def') }}</option>
              </select>
              <div class="form-text">{{ $t('config.nvenc_latency_over_power_desc') }}</div>
            </div>

            <!-- Present OpenGL/Vulkan on top of DXGI -->
            <div class="mb-3" v-if="platform === 'windows'">
              <label for="nvenc_opengl_vulkan_on_dxgi" class="form-label">{{ $t('config.nvenc_opengl_vulkan_on_dxgi') }}</label>
              <select id="nvenc_opengl_vulkan_on_dxgi" class="form-select" v-model="config.nvenc_opengl_vulkan_on_dxgi">
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="enabled">{{ $t('_common.enabled_def') }}</option>
              </select>
              <div class="form-text">{{ $t('config.nvenc_opengl_vulkan_on_dxgi_desc') }}</div>
            </div>

            <!-- NVENC H264 CAVLC -->
            <div>
              <label for="nvenc_h264_cavlc" class="form-label">{{ $t('config.nvenc_h264_cavlc') }}</label>
              <select id="nvenc_h264_cavlc" class="form-select" v-model="config.nvenc_h264_cavlc">
                <option value="disabled">{{ $t('_common.disabled_def') }}</option>
                <option value="enabled">{{ $t('_common.enabled') }}</option>
              </select>
              <div class="form-text">{{ $t('config.nvenc_h264_cavlc_desc') }}</div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>

</style>
