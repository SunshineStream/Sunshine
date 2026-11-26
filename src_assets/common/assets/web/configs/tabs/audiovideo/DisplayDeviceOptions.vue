<script setup>
import { ref } from 'vue'
import { Trash2 } from '@lucide/vue'
import PlatformLayout from '../../../PlatformLayout.vue'
import Checkbox from "../../../Checkbox.vue";

const props = defineProps({
  platform: String,
  config: Object
})
const config = ref(props.config)

const REFRESH_RATE_ONLY = "refresh_rate_only"
const RESOLUTION_ONLY = "resolution_only"
const MIXED = "mixed"

function canBeRemapped() {
  return (config.value.dd_resolution_option === "auto" || config.value.dd_refresh_rate_option === "auto")
    && config.value.dd_configuration_option !== "disabled";
}

function getRemappingType() {
  // Assuming here that at least one setting is set to "auto" if other is not
  if (config.value.dd_resolution_option !== "auto") {
    return REFRESH_RATE_ONLY;
  }
  if (config.value.dd_refresh_rate_option !== "auto") {
    return RESOLUTION_ONLY;
  }
  return MIXED;
}

function addRemappingEntry() {
  const type = getRemappingType();
  let template = {};

  if (type !== RESOLUTION_ONLY) {
    template["requested_fps"] = "";
    template["final_refresh_rate"] = "";
  }

  if (type !== REFRESH_RATE_ONLY) {
    template["requested_resolution"] = "";
    template["final_resolution"] = "";
  }

  config.value.dd_mode_remapping[type].push(template);
}
</script>

<template>
  <PlatformLayout :platform="platform">
    <template #windows>
      <div class="mb-3 accordion">
        <div class="accordion-item">
          <h2 class="accordion-header">
            <button class="accordion-button" type="button" data-bs-toggle="collapse"
                    data-bs-target="#panelsStayOpen-collapseOne">
              {{ $t('config.dd_options_header') }}
            </button>
          </h2>
          <div id="panelsStayOpen-collapseOne" class="accordion-collapse collapse show"
               aria-labelledby="panelsStayOpen-headingOne">
            <div class="accordion-body">

              <!-- Configuration option -->
              <div class="mb-3">
                <label for="dd_configuration_option" class="form-label">
                  {{ $t('config.dd_configuration_option') }}
                </label>
                <select id="dd_configuration_option" class="form-select" v-model="config.dd_configuration_option">
                  <option value="disabled">{{ $t('_common.disabled_def') }}</option>
                  <option value="verify_only">{{ $t('config.dd_config_verify_only') }}</option>
                  <option value="ensure_active">{{ $t('config.dd_config_ensure_active') }}</option>
                  <option value="ensure_primary">{{ $t('config.dd_config_ensure_primary') }}</option>
                  <option value="ensure_only_display">{{ $t('config.dd_config_ensure_only_display') }}</option>
                </select>
              </div>

              <!-- Resolution option -->
              <div class="mb-3" v-if="config.dd_configuration_option !== 'disabled'">
                <label for="dd_resolution_option" class="form-label">
                  {{ $t('config.dd_resolution_option') }}
                </label>
                <select id="dd_resolution_option" class="form-select" v-model="config.dd_resolution_option">
                  <option value="disabled">{{ $t('config.dd_resolution_option_disabled') }}</option>
                  <option value="auto">{{ $t('config.dd_resolution_option_auto') }}</option>
                  <option value="manual">{{ $t('config.dd_resolution_option_manual') }}</option>
                </select>
                <div class="form-text"
                     v-if="config.dd_resolution_option === 'auto' || config.dd_resolution_option === 'manual'">
                  {{ $t('config.dd_resolution_option_ogs_desc') }}
                </div>

                <!-- Manual resolution -->
                <div class="mt-2 ps-4" v-if="config.dd_resolution_option === 'manual'">
                  <label for="dd_manual_resolution" class="form-label">
                    {{ $t('config.dd_manual_resolution') }}
                  </label>
                  <input type="text" class="form-control" id="dd_manual_resolution" placeholder="2560x1440"
                         v-model="config.dd_manual_resolution" />
                </div>
              </div>

              <!-- Refresh rate option -->
              <div class="mb-3" v-if="config.dd_configuration_option !== 'disabled'">
                <label for="dd_refresh_rate_option" class="form-label">
                  {{ $t('config.dd_refresh_rate_option') }}
                </label>
                <select id="dd_refresh_rate_option" class="form-select" v-model="config.dd_refresh_rate_option">
                  <option value="disabled">{{ $t('config.dd_refresh_rate_option_disabled') }}</option>
                  <option value="auto">{{ $t('config.dd_refresh_rate_option_auto') }}</option>
                  <option value="manual">{{ $t('config.dd_refresh_rate_option_manual') }}</option>
                </select>

                <!-- Manual refresh rate -->
                <div class="mt-2 ps-4" v-if="config.dd_refresh_rate_option === 'manual'">
                  <label for="dd_manual_refresh_rate" class="form-label">
                    {{ $t('config.dd_manual_refresh_rate') }}
                  </label>
                  <input type="text" class="form-control" id="dd_manual_refresh_rate" placeholder="59.9558"
                         v-model="config.dd_manual_refresh_rate" />
                </div>
              </div>

              <!-- HDR option -->
              <div class="mb-3" v-if="config.dd_configuration_option !== 'disabled'">
                <label for="dd_hdr_option" class="form-label">
                  {{ $t('config.dd_hdr_option') }}
                </label>
                <select id="dd_hdr_option" class="mb-3 form-select" v-model="config.dd_hdr_option">
                  <option value="disabled">{{ $t('config.dd_hdr_option_disabled') }}</option>
                  <option value="auto">{{ $t('config.dd_hdr_option_auto') }}</option>
                </select>
                <!-- HDR toggle -->
                <label for="dd_wa_hdr_toggle_delay" class="form-label">
                  {{ $t('config.dd_wa_hdr_toggle_delay') }}
                </label>
                <input type="number" class="form-control" id="dd_wa_hdr_toggle_delay" placeholder="0" min="0" max="3000"
                       v-model="config.dd_wa_hdr_toggle_delay" />
                <div class="form-text">
                  {{ $t('config.dd_wa_hdr_toggle_delay_desc_1') }}
                  <br>
                  {{ $t('config.dd_wa_hdr_toggle_delay_desc_2') }}
                  <br>
                  {{ $t('config.dd_wa_hdr_toggle_delay_desc_3') }}
                </div>
              </div>

              <!-- Config revert delay -->
              <div class="mb-3" v-if="config.dd_configuration_option !== 'disabled'">
                <label for="dd_config_revert_delay" class="form-label">
                  {{ $t('config.dd_config_revert_delay') }}
                </label>
                <input type="number" class="form-control" id="dd_config_revert_delay" placeholder="3000" min="0"
                       v-model="config.dd_config_revert_delay" />
                <div class="form-text">
                  {{ $t('config.dd_config_revert_delay_desc') }}
                </div>
              </div>

              <!-- Config revert on disconnect -->
              <div class="mb-3" v-if="config.dd_configuration_option !== 'disabled'">
                <Checkbox id="dd_config_revert_on_disconnect"
                  locale-prefix="config"
                  v-model="config.dd_config_revert_on_disconnect"
                  default="false"
                ></Checkbox>
              </div>

              <!-- Reapply configuration on display loss -->
              <div class="mb-3" v-if="config.dd_configuration_option !== 'disabled' && config.dd_configuration_option !== 'verify_only'">
                <Checkbox id="dd_reapply_on_display_loss"
                  locale-prefix="config"
                  v-model="config.dd_reapply_on_display_loss"
                  default="false"
                ></Checkbox>
              </div>

              <!-- Display mode remapping -->
              <div class="mb-3" v-if="canBeRemapped()">
                <label for="dd_mode_remapping" class="form-label">
                  {{ $t('config.dd_mode_remapping') }}
                </label>
                <div id="dd_mode_remapping" class="d-flex flex-column">
                  <div class="form-text">
                    {{ $t('config.dd_mode_remapping_desc_1') }}<br>
                    {{ $t('config.dd_mode_remapping_desc_2') }}<br>
                    {{ $t('config.dd_mode_remapping_desc_3') }}<br>
                    {{ $t(getRemappingType() === MIXED ? 'config.dd_mode_remapping_desc_4_final_values_mixed' : 'config.dd_mode_remapping_desc_4_final_values_non_mixed') }}<br>
                    <template v-if="getRemappingType() === MIXED">
                      {{ $t('config.dd_mode_remapping_desc_5_sops_mixed_only') }}<br>
                    </template>
                    <template v-if="getRemappingType() === RESOLUTION_ONLY">
                      {{ $t('config.dd_mode_remapping_desc_5_sops_resolution_only') }}<br>
                    </template>
                  </div>
                </div>

                <table class="table" v-if="config.dd_mode_remapping[getRemappingType()].length > 0">
                  <thead>
                    <tr>
                      <th scope="col" v-if="getRemappingType() !== REFRESH_RATE_ONLY">
                        {{ $t('config.dd_mode_remapping_requested_resolution') }}
                      </th>
                      <th scope="col" v-if="getRemappingType() !== RESOLUTION_ONLY">
                        {{ $t('config.dd_mode_remapping_requested_fps') }}
                      </th>
                      <th scope="col" v-if="getRemappingType() !== REFRESH_RATE_ONLY">
                        {{ $t('config.dd_mode_remapping_final_resolution') }}
                      </th>
                      <th scope="col" v-if="getRemappingType() !== RESOLUTION_ONLY">
                        {{ $t('config.dd_mode_remapping_final_refresh_rate') }}
                      </th>
                      <!-- Additional columns for buttons-->
                      <th scope="col"></th>
                    </tr>
                  </thead>
                  <tbody>
                    <tr v-for="(value, idx) in config.dd_mode_remapping[getRemappingType()]" :key="idx">
                      <td v-if="getRemappingType() !== REFRESH_RATE_ONLY">
                        <label :for="`dd-requested-resolution-${idx}`" class="visually-hidden">
                          {{ $t('config.dd_mode_remapping_requested_resolution') }}
                        </label>
                        <input :id="`dd-requested-resolution-${idx}`" type="text" class="form-control monospace" v-model="value.requested_resolution"
                               :placeholder="'1920x1080'" />
                      </td>
                      <td v-if="getRemappingType() !== RESOLUTION_ONLY">
                        <label :for="`dd-requested-fps-${idx}`" class="visually-hidden">
                          {{ $t('config.dd_mode_remapping_requested_fps') }}
                        </label>
                        <input :id="`dd-requested-fps-${idx}`" type="text" class="form-control monospace" v-model="value.requested_fps"
                               :placeholder="'60'" />
                      </td>
                      <td v-if="getRemappingType() !== REFRESH_RATE_ONLY">
                        <label :for="`dd-final-resolution-${idx}`" class="visually-hidden">
                          {{ $t('config.dd_mode_remapping_final_resolution') }}
                        </label>
                        <input :id="`dd-final-resolution-${idx}`" type="text" class="form-control monospace" v-model="value.final_resolution"
                               :placeholder="'2560x1440'" />
                      </td>
                      <td v-if="getRemappingType() !== RESOLUTION_ONLY">
                        <label :for="`dd-final-refresh-rate-${idx}`" class="visually-hidden">
                          {{ $t('config.dd_mode_remapping_final_refresh_rate') }}
                        </label>
                        <input :id="`dd-final-refresh-rate-${idx}`" type="text" class="form-control monospace" v-model="value.final_refresh_rate"
                               :placeholder="'119.95'" />
                      </td>
                      <td>
                        <button class="btn btn-danger" @click="config.dd_mode_remapping[getRemappingType()].splice(idx, 1)">
                          <Trash2 :size="16" />
                        </button>
                      </td>
                    </tr>
                  </tbody>
                </table>
                <button class="ms-0 mt-2 btn btn-success" style="margin: 0 auto" @click="addRemappingEntry()">
                  &plus; {{ $t('config.dd_mode_remapping_add') }}
                </button>
              </div>
            </div>
          </div>
        </div>
      </div>
    </template>
    <template #freebsd>
    </template>
    <template #linux>
    </template>
    <template #macos>
    </template>
  </PlatformLayout>
</template>
