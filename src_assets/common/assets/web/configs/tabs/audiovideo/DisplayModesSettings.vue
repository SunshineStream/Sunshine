<script setup>
import { ref } from 'vue'
import Checkbox from '../../../Checkbox.vue'
const props = defineProps({
  platform: String,
  config: Object,
})
const config = ref(props.config)
</script>

<template>
  <!--max_bitrate-->
  <div class="mb-3">
    <label for="max_bitrate" class="form-label">{{ $t("config.max_bitrate") }}</label>
    <input type="number" class="form-control" id="max_bitrate" placeholder="0" v-model="config.max_bitrate" />
    <div class="form-text">{{ $t("config.max_bitrate_desc") }}</div>
  </div>

  <!--bitrate_mode-->
  <div class="mb-3">
    <label for="bitrate_mode" class="form-label">{{ $t("config.bitrate_mode") }}</label>
    <select id="bitrate_mode" class="form-select" v-model="config.bitrate_mode">
      <option value="fixed">{{ $t("config.bitrate_mode_fixed") }}</option>
      <option value="adaptive">{{ $t("config.bitrate_mode_adaptive") }}</option>
    </select>
    <div class="form-text">{{ $t("config.bitrate_mode_desc") }}</div>
  </div>

  <template v-if="config.bitrate_mode === 'adaptive'">
    <!--abr_rtt_threshold_ms-->
    <div class="mb-3">
      <label for="abr_rtt_threshold_ms" class="form-label">{{ $t("config.abr_rtt_threshold_ms") }}</label>
      <input type="number" class="form-control" id="abr_rtt_threshold_ms" placeholder="100" min="1" max="10000" v-model="config.abr_rtt_threshold_ms" />
      <div class="form-text">{{ $t("config.abr_rtt_threshold_ms_desc") }}</div>
    </div>

    <!--abr_margin_ms-->
    <div class="mb-3">
      <label for="abr_margin_ms" class="form-label">{{ $t("config.abr_margin_ms") }}</label>
      <input type="number" class="form-control" id="abr_margin_ms" placeholder="20" min="0" max="10000" v-model="config.abr_margin_ms" />
      <div class="form-text">{{ $t("config.abr_margin_ms_desc") }}</div>
    </div>

    <!--abr_hysteresis_ms-->
    <div class="mb-3">
      <label for="abr_hysteresis_ms" class="form-label">{{ $t("config.abr_hysteresis_ms") }}</label>
      <input type="number" class="form-control" id="abr_hysteresis_ms" placeholder="2000" min="0" max="60000" v-model="config.abr_hysteresis_ms" />
      <div class="form-text">{{ $t("config.abr_hysteresis_ms_desc") }}</div>
    </div>

    <!--abr_stable_time_ms-->
    <div class="mb-3">
      <label for="abr_stable_time_ms" class="form-label">{{ $t("config.abr_stable_time_ms") }}</label>
      <input type="number" class="form-control" id="abr_stable_time_ms" placeholder="1000" min="0" max="60000" v-model="config.abr_stable_time_ms" />
      <div class="form-text">{{ $t("config.abr_stable_time_ms_desc") }}</div>
    </div>

    <!--abr_beta-->
    <div class="mb-3">
      <label for="abr_beta" class="form-label">{{ $t("config.abr_beta") }}</label>
      <input type="number" class="form-control" id="abr_beta" placeholder="0.7" min="0.1" max="0.95" step="0.05" v-model="config.abr_beta" />
      <div class="form-text">{{ $t("config.abr_beta_desc") }}</div>
    </div>

    <!--abr_cubic_recovery_ms-->
    <div class="mb-3">
      <label for="abr_cubic_recovery_ms" class="form-label">{{ $t("config.abr_cubic_recovery_ms") }}</label>
      <input type="number" class="form-control" id="abr_cubic_recovery_ms" placeholder="15000" min="1000" max="120000" v-model="config.abr_cubic_recovery_ms" />
      <div class="form-text">{{ $t("config.abr_cubic_recovery_ms_desc") }}</div>
    </div>

    <!--abr_fast_convergence-->
    <Checkbox class="mb-3"
              id="abr_fast_convergence"
              locale-prefix="config"
              v-model="config.abr_fast_convergence"
              default="true"
    ></Checkbox>

    <!--abr_min_kbps-->
    <div class="mb-3">
      <label for="abr_min_kbps" class="form-label">{{ $t("config.abr_min_kbps") }}</label>
      <input type="number" class="form-control" id="abr_min_kbps" placeholder="1000" min="1" v-model="config.abr_min_kbps" />
      <div class="form-text">{{ $t("config.abr_min_kbps_desc") }}</div>
    </div>

    <!--abr_max_kbps-->
    <div class="mb-3">
      <label for="abr_max_kbps" class="form-label">{{ $t("config.abr_max_kbps") }}</label>
      <input type="number" class="form-control" id="abr_max_kbps" placeholder="0" min="0" v-model="config.abr_max_kbps" />
      <div class="form-text">{{ $t("config.abr_max_kbps_desc") }}</div>
    </div>

  </template>

  <!--max_stream_resolution-->
  <div class="mb-3">
    <label for="max_stream_resolution" class="form-label">{{ $t("config.max_stream_resolution") }}</label>
    <input type="text" class="form-control" id="max_stream_resolution" placeholder="2560x1440" v-model="config.max_stream_resolution" />
    <div class="form-text">{{ $t("config.max_stream_resolution_desc") }}</div>
  </div>

  <!--minimum_fps_target-->
  <div class="mb-3">
    <label for="minimum_fps_target" class="form-label">{{ $t("config.minimum_fps_target") }}</label>
    <input type="number" min="0" max="1000" class="form-control" id="minimum_fps_target" placeholder="0" v-model="config.minimum_fps_target" />
    <div class="form-text">{{ $t("config.minimum_fps_target_desc") }}</div>
  </div>
</template>

<style scoped>
.ms-item {
  background-color: var(--bs-dark-bg-subtle);
  font-size: 12px;
  font-weight: bold;
}
</style>
