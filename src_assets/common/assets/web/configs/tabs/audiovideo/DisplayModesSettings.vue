<script setup>
import { ref } from 'vue'
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
