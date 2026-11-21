<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../PlatformLayout.vue'
import Checkbox from '../../Checkbox.vue'

const props = defineProps({
  platform: String,
  config: Object,
  global_prep_cmd: Array,
})

const config = ref(props.config)
</script>

<template>
  <div class="config-page">
    <!-- FEC Percentage -->
    <div class="mb-3">
      <label for="fec_percentage" class="form-label">{{ $t('config.fec_percentage') }}</label>
      <input type="number" class="form-control" id="fec_percentage" placeholder="20" min="0" max="100" v-model="config.fec_percentage" />
      <div class="form-text">{{ $t('config.fec_percentage_desc') }}</div>
    </div>

    <!-- Quantization Parameter -->
    <div class="mb-3">
      <label for="qp" class="form-label">{{ $t('config.qp') }}</label>
      <input type="number" class="form-control" id="qp" placeholder="28" v-model="config.qp" />
      <div class="form-text">{{ $t('config.qp_desc') }}</div>
    </div>

    <!-- Min Threads -->
    <div class="mb-3">
      <label for="min_threads" class="form-label">{{ $t('config.min_threads') }}</label>
      <input type="number" class="form-control" id="min_threads" placeholder="2" min="1" v-model="config.min_threads" />
      <div class="form-text">{{ $t('config.min_threads_desc') }}</div>
    </div>

    <!-- HEVC Support -->
    <div class="mb-3">
      <label for="hevc_mode" class="form-label">{{ $t('config.hevc_mode') }}</label>
      <select id="hevc_mode" class="form-select" v-model="config.hevc_mode">
        <option value="0">{{ $t('config.hevc_mode_0') }}</option>
        <option value="1">{{ $t('config.hevc_mode_1') }}</option>
        <option value="2">{{ $t('config.hevc_mode_2') }}</option>
        <option value="3">{{ $t('config.hevc_mode_3') }}</option>
      </select>
      <div class="form-text">{{ $t('config.hevc_mode_desc') }}</div>
    </div>

    <!-- AV1 Support -->
    <div class="mb-3">
      <label for="av1_mode" class="form-label">{{ $t('config.av1_mode') }}</label>
      <select id="av1_mode" class="form-select" v-model="config.av1_mode">
        <option value="0">{{ $t('config.av1_mode_0') }}</option>
        <option value="1">{{ $t('config.av1_mode_1') }}</option>
        <option value="2">{{ $t('config.av1_mode_2') }}</option>
        <option value="3">{{ $t('config.av1_mode_3') }}</option>
      </select>
      <div class="form-text">{{ $t('config.av1_mode_desc') }}</div>
    </div>

    <!-- PyroWave Support -->
    <Checkbox class="mb-3"
              id="pyrowave"
              locale-prefix="config"
              v-model="config.pyrowave"
              default="true"
    ></Checkbox>

    <!-- Capture -->
    <div class="mb-3" v-if="platform !== 'macos'">
      <label for="capture" class="form-label">{{ $t('config.capture') }}</label>
      <select id="capture" class="form-select" v-model="config.capture">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout :platform="platform">
          <template #freebsd>
            <option value="wlr">wlroots</option>
            <option value="x11">X11</option>
            <option value="portal">XDG Portal</option>
          </template>
          <template #linux>
            <option value="nvfbc">NvFBC</option>
            <option value="wlr">wlroots</option>
            <option value="kms">KMS</option>
            <option value="x11">X11</option>
            <option value="kwin">KWin Screencast</option>
            <option value="portal">XDG Portal</option>
          </template>
          <template #windows>
            <option value="ddx">Desktop Duplication API</option>
            <option value="wgc">Windows.Graphics.Capture {{ $t('_common.beta') }}</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">{{ $t('config.capture_desc') }}</div>
    </div>

    <!-- FP16 SDR Gamma Encoded -->
    <Checkbox v-if="platform === 'windows'"
              class="mb-3"
              id="fp16_sdr_gamma_encoded"
              locale-prefix="config"
              v-model="config.fp16_sdr_gamma_encoded"
              default="false"
    ></Checkbox>

    <!-- Encoder -->
    <div class="mb-3">
      <label for="encoder" class="form-label">{{ $t('config.encoder') }}</label>
      <select id="encoder" class="form-select" v-model="config.encoder">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout :platform="platform">
          <template #windows>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="quicksync">Intel QuickSync</option>
            <option value="amdvce">AMD AMF/VCE</option>
          </template>
          <template #freebsd>
            <option value="vulkan">Vulkan</option>
            <option value="vaapi">VA-API</option>
          </template>
          <template #linux>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="vaapi">VA-API</option>
            <option value="vulkan">Vulkan</option>
          </template>
          <template #macos>
            <option value="videotoolbox">VideoToolbox</option>
          </template>
        </PlatformLayout>
        <option value="software">{{ $t('config.encoder_software') }}</option>
      </select>
      <div class="form-text">{{ $t('config.encoder_desc') }}</div>
    </div>

  </div>
</template>
