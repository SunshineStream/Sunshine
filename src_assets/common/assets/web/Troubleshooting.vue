<template>
  <Navbar></Navbar>
  <div id="content" class="container">
    <h1 class="my-4">{{ $t('troubleshooting.troubleshooting') }}</h1>
    <div class="card my-4" v-if="permissions.length || permissionError">
      <div class="card-body">
        <div class="d-flex align-items-center justify-content-between gap-3 flex-wrap mb-2">
          <h2 id="permissions" class="mb-0">{{ $t('troubleshooting.permissions_title') }}</h2>
          <button class="btn btn-outline-secondary" type="button" @click="refreshPermissions">
            <refresh-cw :size="17" class="icon"></refresh-cw>
            {{ $t('troubleshooting.permissions_refresh') }}
          </button>
        </div>
        <p>{{ $t('troubleshooting.permissions_desc') }}</p>
        <div v-if="permissionError" class="alert alert-danger" role="alert">{{ permissionError }}</div>
        <div v-if="permissions.length" class="table-responsive permission-table-shell">
          <table class="table align-middle mb-0">
            <thead>
              <tr>
                <th scope="col">{{ $t('troubleshooting.permissions_name') }}</th>
                <th scope="col">{{ $t('troubleshooting.permissions_status') }}</th>
                <th scope="col">{{ $t('troubleshooting.permissions_requirement') }}</th>
                <th scope="col" class="permission-action-column">
                  <span class="visually-hidden">{{ $t('troubleshooting.permissions_action') }}</span>
                </th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="permission in orderedPermissions" :key="permission.id">
                <th scope="row">
                  <strong>{{ $t('troubleshooting.permission_' + permission.id +
                    (permission.id === 'input' && (platform === 'linux' || platform === 'freebsd') ? '_unix' : '')) }}</strong>
                  <p class="permission-detail mb-0">{{ $t('troubleshooting.permission_' + permission.id + '_desc' +
                    (permission.id === 'input' && (platform === 'linux' || platform === 'freebsd') ? '_unix' : '')) }}</p>
                  <output v-if="permissionHelp === permission.id" class="permission-help d-block mb-0">
                    {{ $t('troubleshooting.permission_' + permission.id + '_help' +
                      (permission.id === 'input' && platform !== 'macos' ? '_' + platform : '')) }}
                  </output>
                </th>
                <td>
                  <span class="status-icon" :class="permissionStatusClass(permission.status)"
                        :title="$t('troubleshooting.permissions_status_' + permission.status)">
                    <check-circle v-if="permission.status === 'granted'" :size="20" aria-hidden="true"></check-circle>
                    <x-circle v-else-if="permission.status === 'denied'" :size="20" aria-hidden="true"></x-circle>
                    <clock-3 v-else-if="permission.status === 'on_use'" :size="20" aria-hidden="true"></clock-3>
                    <alert-circle v-else-if="permission.status === 'not_determined'" :size="20" aria-hidden="true"></alert-circle>
                    <alert-triangle v-else :size="20" aria-hidden="true"></alert-triangle>
                    <span class="visually-hidden">{{ $t('troubleshooting.permissions_status_' + permission.status) }}</span>
                  </span>
                </td>
                <td>
                  <span class="badge" :class="permission.required ? 'text-bg-primary' : 'text-bg-secondary'">
                    {{ $t(permission.required ? 'troubleshooting.permissions_required' : 'troubleshooting.permissions_optional') }}
                  </span>
                </td>
                <td class="permission-action-column">
                  <button v-if="permission.status !== 'granted' && permission.requestable" class="btn btn-outline-primary"
                          type="button" :disabled="permissionBusy === permission.id"
                          @click="requestPermission(permission.id)">
                    {{ $t(permission.id === 'local_network'
                      ? 'troubleshooting.permissions_privacy_settings' : 'troubleshooting.permissions_request') }}
                  </button>
                  <button v-else-if="permission.status !== 'granted'" class="btn btn-outline-primary"
                          type="button" @click="permissionHelp = permissionHelp === permission.id ? '' : permission.id">
                    {{ $t('troubleshooting.permissions_instructions') }}
                  </button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </div>
    <!-- Virtual gamepad broker and license -->
    <div class="card my-4 virtual-gamepad-card" v-if="platform === 'windows' || platform === 'macos'">
      <div class="card-body">
        <header class="virtual-gamepad-hero">
          <div class="virtual-gamepad-heading-icon" aria-hidden="true">
            <gamepad-2 :size="28"></gamepad-2>
          </div>
          <div>
            <h2 id="virtualhid" class="mb-1">{{ $t('troubleshooting.virtual_gamepad') }}</h2>
            <p class="mb-0">{{ $t(virtualInputDescriptionKey) }}</p>
            <RouterLink v-if="platform === 'windows' && gamepadDriver === 'vigembus'"
               class="btn btn-primary mt-3"
               to="/config#gamepad_driver">
              <gamepad-2 :size="18" class="icon"></gamepad-2>
              {{ $t('troubleshooting.change_gamepad_driver') }}
            </RouterLink>
          </div>
        </header>

        <div class="virtual-gamepad-feature-grid"
             v-if="showVirtualhidBenefits"
             :aria-label="$t('troubleshooting.virtualhid_benefits_title')">
          <article class="virtual-gamepad-feature">
            <div class="virtual-gamepad-feature-icon" aria-hidden="true">
              <gamepad-2 :size="21"></gamepad-2>
            </div>
            <div>
              <h3>{{ $t('troubleshooting.virtualhid_benefit_gamepads_title') }}</h3>
              <p>{{ $t('troubleshooting.virtualhid_benefit_gamepads') }}</p>
            </div>
          </article>
          <article class="virtual-gamepad-feature">
            <div class="virtual-gamepad-feature-icon" aria-hidden="true">
              <mouse-pointer-2 :size="21"></mouse-pointer-2>
            </div>
            <div>
              <h3>{{ $t('troubleshooting.virtualhid_benefit_mouse_title') }}</h3>
              <p>{{ $t('troubleshooting.virtualhid_benefit_mouse') }}</p>
            </div>
          </article>
          <article class="virtual-gamepad-feature">
            <div class="virtual-gamepad-feature-icon" aria-hidden="true">
              <sparkles :size="21"></sparkles>
            </div>
            <div>
              <h3>{{ $t('troubleshooting.virtualhid_benefit_features_title') }}</h3>
              <p>{{ $t('troubleshooting.virtualhid_benefit_features') }}</p>
            </div>
          </article>
          <article class="virtual-gamepad-feature">
            <div class="virtual-gamepad-feature-icon" aria-hidden="true">
              <shield-check :size="21"></shield-check>
            </div>
            <div>
              <h3>{{ $t('troubleshooting.virtualhid_benefit_maintained_title') }}</h3>
              <p>{{ $t('troubleshooting.virtualhid_benefit_maintained') }}</p>
            </div>
          </article>
        </div>

        <section class="virtual-gamepad-section" v-if="platform === 'windows' || platform === 'macos'">
          <div class="virtual-gamepad-section-heading">
            <div>
              <h3 class="h4 mb-1">{{ $t('troubleshooting.virtual_gamepad_drivers') }}</h3>
              <p class="mb-0">{{ $t('troubleshooting.virtual_gamepad_drivers_desc') }}</p>
            </div>
            <button class="btn btn-outline-secondary driver-refresh-button"
                    type="button"
                    @click="refreshDriverInformation">
              <refresh-cw :size="17" :class="{ 'driver-release-spinning': driverReleaseLoading }"></refresh-cw>
              {{ $t('troubleshooting.driver_refresh') }}
            </button>
          </div>

          <div class="table-responsive driver-table-shell">
            <table class="table align-middle mb-0">
              <thead>
                <tr>
                  <th scope="col">{{ $t('troubleshooting.driver_name') }}</th>
                  <th scope="col">{{ $t('troubleshooting.driver_installed_version') }}</th>
                  <th scope="col">{{ $t('troubleshooting.driver_latest_version') }}</th>
                  <th scope="col">{{ $t('troubleshooting.driver_supported_versions') }}</th>
                  <th scope="col">{{ $t('troubleshooting.driver_status') }}</th>
                  <th scope="col" class="driver-download-column">
                    <span class="visually-hidden">{{ $t('troubleshooting.driver_download') }}</span>
                  </th>
                </tr>
              </thead>
              <tbody>
                <tr v-if="showVirtualhid">
                  <th scope="row">{{ $t('troubleshooting.virtualhid_driver') }}</th>
                  <td>{{ driverVersion(virtualhid) }}</td>
                  <td>
                    <div class="driver-release-state"
                         :class="driverReleaseStateClass(virtualhid, virtualhidRelease)"
                         :title="driverReleaseStatusText(virtualhid, virtualhidRelease)"
                         aria-live="polite">
                      <refresh-cw v-if="virtualhidRelease.loading"
                                  :size="17"
                                  class="driver-release-spinning"></refresh-cw>
                      <check-circle v-else-if="driverReleaseState(virtualhid, virtualhidRelease) === 'current'"
                                    :size="17"></check-circle>
                      <alert-triangle v-else-if="driverReleaseState(virtualhid, virtualhidRelease) === 'outdated'"
                                      :size="17"></alert-triangle>
                      <alert-circle v-else :size="17"></alert-circle>
                      <span>{{ driverLatestVersion(virtualhidRelease) }}</span>
                    </div>
                    <small class="driver-release-detail">{{ driverReleaseStatusText(virtualhid, virtualhidRelease) }}</small>
                  </td>
                  <td>{{ virtualhid.supported_versions }}</td>
                  <td>
                    <span class="status-icon driver-status-icon" :class="driverStatusClass(virtualhid)"
                          :title="driverStatusText(virtualhid)">
                      <alert-circle v-if="!virtualhid.installed" :size="20" aria-hidden="true"></alert-circle>
                      <check-circle v-else-if="virtualhid.version_compatible" :size="20" aria-hidden="true"></check-circle>
                      <x-circle v-else :size="20" aria-hidden="true"></x-circle>
                      <span class="visually-hidden">{{ driverStatusText(virtualhid) }}</span>
                    </span>
                  </td>
                  <td class="driver-download-column">
                    <a class="btn btn-outline-primary driver-download-button"
                       :href="virtualhidRelease.url"
                       target="_blank"
                       rel="noopener noreferrer">
                      <download :size="17"></download>
                      {{ $t('troubleshooting.driver_download') }}
                    </a>
                  </td>
                </tr>
                <tr id="vigembus" v-if="showVigembus">
                  <th scope="row">{{ $t('troubleshooting.vigembus_driver') }}</th>
                  <td>{{ driverVersion(vigembus) }}</td>
                  <td>
                    <div class="driver-release-state driver-release-neutral">
                      <alert-circle :size="17"></alert-circle>
                      <span>{{ $t('troubleshooting.driver_release_eol') }}</span>
                    </div>
                  </td>
                  <td>{{ vigembus.supported_versions }}</td>
                  <td>
                    <span class="status-icon driver-status-icon" :class="driverStatusClass(vigembus)"
                          :title="driverStatusText(vigembus)">
                      <alert-circle v-if="!vigembus.installed" :size="20" aria-hidden="true"></alert-circle>
                      <check-circle v-else-if="vigembus.version_compatible" :size="20" aria-hidden="true"></check-circle>
                      <x-circle v-else :size="20" aria-hidden="true"></x-circle>
                      <span class="visually-hidden">{{ driverStatusText(vigembus) }}</span>
                    </span>
                  </td>
                  <td class="driver-download-column">
                    <a class="btn btn-outline-primary driver-download-button"
                       href="https://github.com/nefarius/ViGEmBus/releases/latest"
                       target="_blank"
                       rel="noopener noreferrer">
                      <download :size="17"></download>
                      {{ $t('troubleshooting.driver_download') }}
                    </a>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </section>

        <section class="virtualhid-license-section" aria-labelledby="virtualhid-license" v-if="showVirtualhid">
          <div class="virtualhid-license-heading">
            <div>
              <h3 id="virtualhid-license" class="h4 mb-1">{{ $t('troubleshooting.virtualhid_broker_license') }}</h3>
              <p class="mb-0">{{ $t(platform === 'macos' ? 'troubleshooting.virtualhid_macos_license_desc' : 'troubleshooting.virtualhid_license_desc') }}</p>
            </div>
            <span :class="licenseStatusClass()">{{ licenseStatusText() }}</span>
          </div>

          <div class="alert alert-danger alert-inline" role="alert" v-if="licenseError">
            <alert-circle :size="18" class="icon flex-shrink-0"></alert-circle>
            <span>{{ licenseError }}</span>
          </div>
          <div class="alert alert-warning alert-inline" role="alert" v-else-if="!virtualhidLicense.service_available">
            <alert-triangle :size="18" class="icon flex-shrink-0"></alert-triangle>
            <span>{{ virtualhidLicense.message || $t('troubleshooting.virtualhid_broker_unavailable') }}</span>
          </div>

          <div class="virtualhid-license-stats">
            <dl class="virtualhid-license-stat" v-if="virtualhidLicense.plan_name">
              <dt>{{ $t('troubleshooting.virtualhid_license_plan') }}</dt>
              <dd>{{ virtualhidLicense.plan_name }}</dd>
            </dl>
            <dl class="virtualhid-license-stat">
              <dt>{{ $t('troubleshooting.virtualhid_license_activation_limit') }}</dt>
              <dd>{{ licenseActivationLimitText() }}</dd>
            </dl>
            <dl class="virtualhid-license-stat">
              <dt>{{ $t('troubleshooting.virtualhid_license_active_devices') }}</dt>
              <dd>{{ virtualhidLicense.active_devices }}</dd>
            </dl>
            <dl class="virtualhid-license-stat" v-if="virtualhidLicense.customer_email">
              <dt>{{ $t('troubleshooting.virtualhid_license_customer') }}</dt>
              <dd>{{ virtualhidLicense.customer_email }}</dd>
            </dl>
          </div>

          <p class="virtualhid-license-message"
             v-if="virtualhidLicense.message && virtualhidLicense.service_available">
            {{ virtualhidLicense.message }}
          </p>

          <div class="virtualhid-license-toolbar">
            <button class="btn btn-secondary"
                    type="button"
                    :disabled="licenseBusy || !virtualhidLicense.service_available"
                    @click="updateLicense('validate')">
              <refresh-cw :size="18" :class="{ 'driver-release-spinning': licenseBusy }"></refresh-cw>
              {{ $t('troubleshooting.virtualhid_license_refresh') }}
            </button>
            <a class="btn btn-outline-primary"
               v-if="virtualhidLicense.manage_account_url"
               :href="virtualhidLicense.manage_account_url"
               target="_blank"
               rel="noopener noreferrer">
              <external-link :size="18"></external-link>
              {{ $t('troubleshooting.virtualhid_license_manage') }}
            </a>
            <a class="btn btn-success"
               v-if="virtualhidLicense.purchase_url && !virtualhidLicense.licensed"
               :href="virtualhidLicense.purchase_url"
               target="_blank"
               rel="noopener noreferrer">
              {{ $t('troubleshooting.virtualhid_license_buy') }}
            </a>
            <button class="btn btn-danger"
                    type="button"
                    v-if="virtualhidLicense.licensed"
                    :disabled="licenseBusy || !virtualhidLicense.service_available"
                    @click="updateLicense('deactivate')">
              <trash-2 :size="18" class="icon"></trash-2>
              {{ $t('troubleshooting.virtualhid_license_deactivate') }}
            </button>
          </div>

          <div class="virtualhid-license-activation" v-if="!virtualhidLicense.licensed">
            <label for="virtualhid-license-key" class="form-label">
              {{ $t('troubleshooting.virtualhid_license_key') }}
            </label>
            <div class="input-group virtualhid-license-input">
              <input id="virtualhid-license-key"
                     class="form-control"
                     type="password"
                     v-model="licenseKey"
                     :placeholder="$t('troubleshooting.virtualhid_license_key_placeholder')"
                     :disabled="licenseBusy || !virtualhidLicense.service_available"
                     autocomplete="off"
                     autocapitalize="off"
                     spellcheck="false"
                     @keyup.enter="updateLicense('activate')" />
              <button class="btn btn-primary"
                      type="button"
                      :disabled="licenseBusy || !virtualhidLicense.service_available || !licenseKey.trim()"
                      @click="updateLicense('activate')">
                <key-round :size="18"></key-round>
                {{ $t('troubleshooting.virtualhid_license_activate') }}
              </button>
            </div>
            <div class="form-text">{{ $t('troubleshooting.virtualhid_license_key_desc') }}</div>
          </div>

          <div class="virtualhid-license-active" v-else>
            <check-circle :size="22" aria-hidden="true"></check-circle>
            <div>
              <strong>{{ $t('troubleshooting.virtualhid_license_machine_activated') }}</strong>
              <p class="mb-0">{{ $t('troubleshooting.virtualhid_broker_activated_desc') }}</p>
            </div>
          </div>
        </section>
      </div>
    </div>
    <!-- Force Close App -->
    <div class="card my-4">
      <div class="card-body">
        <h2 id="close_apps">{{ $t('troubleshooting.force_close') }}</h2>
        <p>{{ $t('troubleshooting.force_close_desc') }}</p>
        <div class="alert alert-success" v-if="closeAppStatus === true">
          <check-circle :size="18" class="icon"></check-circle>
          {{ $t('troubleshooting.force_close_success') }}
        </div>
        <div class="alert alert-danger" v-if="closeAppStatus === false">
          <alert-circle :size="18" class="icon"></alert-circle>
          {{ $t('troubleshooting.force_close_error') }}
        </div>
        <div>
          <button class="btn btn-warning" :disabled="closeAppPressed" @click="closeApp">
            <x-circle :size="18" class="icon"></x-circle>
            {{ $t('troubleshooting.force_close') }}
          </button>
        </div>
      </div>
    </div>
    <!-- Restart Sunshine -->
    <div class="card my-4">
      <div class="card-body">
        <h2 id="restart">{{ $t('troubleshooting.restart_sunshine') }}</h2>
        <p>{{ $t('troubleshooting.restart_sunshine_desc') }}</p>
        <div class="alert alert-success" v-if="restartPressed === true">
          <check-circle :size="18" class="icon"></check-circle>
          {{ $t('troubleshooting.restart_sunshine_success') }}
        </div>
        <div>
          <button class="btn btn-warning" :disabled="restartPressed" @click="restart">
            <refresh-cw :size="18" class="icon"></refresh-cw>
            {{ $t('troubleshooting.restart_sunshine') }}
          </button>
        </div>
      </div>
    </div>
    <!-- Reset XDG Portal restore token -->
    <div class="card my-4" v-if="platform === 'linux' || platform === 'freebsd'">
      <div class="card-body">
        <h2 id="portal_reset">{{ $t('troubleshooting.portal_reset') }}</h2>
        <p>{{ $t('troubleshooting.portal_reset_desc') }}</p>
        <div class="alert alert-success" v-if="portalResetStatus === true">
          <check-circle :size="18" class="icon"></check-circle>
          {{ $t('troubleshooting.portal_reset_success') }}
        </div>
        <div class="alert alert-danger" v-if="portalResetStatus === false">
          <alert-circle :size="18" class="icon"></alert-circle>
          {{ $t('troubleshooting.portal_reset_error') }}
        </div>
        <div>
          <button class="btn btn-warning" :disabled="portalResetPressed" @click="resetPortalToken">
            <rotate-ccw :size="18" class="icon"></rotate-ccw>
            {{ $t('troubleshooting.portal_reset') }}
          </button>
        </div>
      </div>
    </div>
    <!-- Reset persistent display device settings -->
    <div class="card my-4" v-if="platform === 'windows'">
      <div class="card-body">
        <h2 id="dd_reset">{{ $t('troubleshooting.dd_reset') }}</h2>
        <p style="white-space: pre-line">{{ $t('troubleshooting.dd_reset_desc') }}</p>
        <div class="alert alert-success" v-if="ddResetStatus === true">
          <check-circle :size="18" class="icon"></check-circle>
          {{ $t('troubleshooting.dd_reset_success') }}
        </div>
        <div class="alert alert-danger" v-if="ddResetStatus === false">
          <alert-circle :size="18" class="icon"></alert-circle>
          {{ $t('troubleshooting.dd_reset_error') }}
        </div>
        <div>
          <button class="btn btn-warning" :disabled="ddResetPressed" @click="ddResetPersistence">
            <rotate-ccw :size="18" class="icon"></rotate-ccw>
            {{ $t('troubleshooting.dd_reset') }}
          </button>
        </div>
      </div>
    </div>
    <!-- Unpair Clients -->
    <div class="card my-4">
      <div class="card-body">
        <div class="d-flex justify-content-between align-items-center mb-3">
          <h2 id="unpair" class="mb-0">{{ $t('troubleshooting.unpair_title') }}</h2>
          <button class="btn btn-danger" :disabled="unpairAllPressed" @click="unpairAll">
            <trash-2 :size="18" class="icon"></trash-2>
            {{ $t('troubleshooting.unpair_all') }}
          </button>
        </div>
        <p>{{ $t('troubleshooting.unpair_desc') }}</p>
        <div class="alert alert-success d-flex align-items-center" v-if="showApplyMessage">
          <check-circle :size="18" class="icon"></check-circle>
          <div><b>{{ $t('_common.success') }}</b> {{ $t('troubleshooting.unpair_single_success') }}</div>
          <button class="btn btn-success ms-auto" @click="clickedApplyBanner">{{ $t('_common.dismiss') }}</button>
        </div>
        <div class="alert alert-success" v-if="unpairAllStatus === true">
          <check-circle :size="18" class="icon"></check-circle>
          {{ $t('troubleshooting.unpair_all_success') }}
        </div>
        <div class="alert alert-danger" v-if="unpairAllStatus === false">
          <alert-circle :size="18" class="icon"></alert-circle>
          {{ $t('troubleshooting.unpair_all_error') }}
        </div>
      </div>
      <ul class="list-group list-group-flush" v-if="clients && clients.length > 0">
        <li v-for="client in clients" :key="client.uuid" class="list-group-item d-flex align-items-center">
          <div class="flex-grow-1">
            {{ client.name !== "" ? client.name : $t('troubleshooting.unpair_single_unknown') }}
          </div>
          <div class="form-check form-switch ms-2 mb-0">
            <input class="form-check-input" type="checkbox" role="switch"
                   :id="'toggle-' + client.uuid"
                   :checked="client.enabled"
                   :aria-checked="client.enabled.toString()"
                   @change="toggleClient(client.uuid, !client.enabled)">
          </div>
          <button class="btn btn-danger btn-sm ms-2" @click="unpairSingle(client.uuid)">
            <trash-2 :size="18" class="icon"></trash-2>
          </button>
        </li>
      </ul>
      <ul v-else class="list-group list-group-flush">
        <li class="list-group-item p-3 text-center">
          <em>{{ $t('troubleshooting.unpair_single_no_devices') }}</em>
        </li>
      </ul>
    </div>
    <!-- Logs -->
    <div class="card my-4">
      <div class="card-body">
        <h2 id="logs">{{ $t('troubleshooting.logs') }}</h2>
        <div class="d-flex justify-content-between align-items-baseline py-2">
          <p>{{ $t('troubleshooting.logs_desc') }}</p>
          <div class="input-group" style="max-width: 300px">
            <label for="log-filter" class="visually-hidden">{{ $t('troubleshooting.logs_find') }}</label>
            <span class="input-group-text">
              <search :size="18" class="icon"></search>
            </span>
            <input id="log-filter" type="text" class="form-control" v-model="logFilter" :placeholder="$t('troubleshooting.logs_find')" />
          </div>
        </div>
        <div>
          <div class="troubleshooting-logs" ref="logsContainer">
            <div class="log-nav-overlay">
              <div class="log-nav-controls">
                <button class="log-nav-btn" @click="scrollLogsTo('top')" title="Jump to Top">
                  <chevrons-up :size="18" class="icon"></chevrons-up>
                </button>
                <button class="log-nav-btn" @click="navigateToLog('prev')" :disabled="!hasPrevLog" title="Previous Warning/Error">
                  <chevron-up :size="18" class="icon"></chevron-up>
                </button>
                <button class="log-nav-btn" @click="navigateToLog('next')" :disabled="!hasNextLog" title="Next Warning/Error">
                  <chevron-down :size="18" class="icon"></chevron-down>
                </button>
                <button class="log-nav-btn" @click="scrollLogsTo('bottom')" title="Jump to Bottom">
                  <chevrons-down :size="18" class="icon"></chevrons-down>
                </button>
                <button class="log-nav-btn" @click="copyLogs" title="Copy Logs">
                  <check :size="18" class="icon text-success" v-if="logsCopied"></check>
                  <copy :size="18" class="icon" v-else></copy>
                </button>
              </div>
            </div>
            <pre class="mb-0" v-html="highlightedLogs"></pre>
          </div>
        </div>
      </div>
    </div>
  </div>

</template>

<script>
    import Navbar from './Navbar.vue'
    import { apiFetch } from './fetch_utils'
    import {
      AlertCircle,
      AlertTriangle,
      Check,
      CheckCircle,
      ChevronDown,
      ChevronUp,
      ChevronsDown,
      ChevronsUp,
      Clock3,
      Copy,
      Download,
      ExternalLink,
      Gamepad2,
      KeyRound,
      MousePointer2,
      RefreshCw,
      RotateCcw,
      Search,
      ShieldCheck,
      Sparkles,
      Trash2,
      XCircle,
    } from '@lucide/vue'

    const permissionOrder = ['screen_recording', 'input', 'notifications', 'microphone', 'system_audio', 'local_network'];

    export default {
      components: {
        Navbar,
        AlertCircle,
        AlertTriangle,
        Check,
        CheckCircle,
        ChevronDown,
        ChevronUp,
        ChevronsDown,
        ChevronsUp,
        Clock3,
        Copy,
        Download,
        ExternalLink,
        Gamepad2,
        KeyRound,
        MousePointer2,
        RefreshCw,
        RotateCcw,
        Search,
        ShieldCheck,
        Sparkles,
        Trash2,
        XCircle,
      },
      data() {
        return {
          clients: [],
          closeAppPressed: false,
          closeAppStatus: null,
          ddResetPressed: false,
          ddResetStatus: null,
          logsCopied: false,
          logs: 'Loading...',
          logFilter: null,
          logInterval: null,
          licenseBusy: false,
          licenseError: '',
          licenseKey: '',
          portalResetPressed: false,
          portalResetStatus: null,
          permissions: [],
          permissionBusy: '',
          permissionError: '',
          permissionHelp: '',
          restartPressed: false,
          showApplyMessage: false,
          platform: "",
          gamepadDriver: '',
          unpairAllPressed: false,
          unpairAllStatus: null,
          virtualhid: {
            installed: false,
            version: '',
            version_compatible: false,
            minimum_version: '',
            supported_versions: '',
          },
          virtualhidLicense: {
            operation_ok: false,
            service_available: false,
            state: 'unavailable',
            licensed: false,
            active_devices: 0,
            activation_limit: 0,
            activation_usage: 0,
            plan_name: '',
            customer_email: '',
            message: '',
            purchase_url: '',
            manage_account_url: '',
          },
          virtualhidRelease: {
            loading: false,
            version: '',
            url: 'https://github.com/LizardByte/libvirtualhid/releases/latest',
            error: false,
          },
          vigembus: {
            installed: false,
            version: '',
            version_compatible: false,
            minimum_version: '',
            supported_versions: '',
          },
          currentLogIndex: -1,
          logLines: [],
        };
      },
      computed: {
        /** Keep known permissions in troubleshooting order on every platform. */
        orderedPermissions() {
          return this.permissions.slice().sort((left, right) => {
            const leftRank = permissionOrder.indexOf(left.id);
            const rightRank = permissionOrder.indexOf(right.id);
            return (leftRank < 0 ? permissionOrder.length : leftRank) -
              (rightRank < 0 ? permissionOrder.length : rightRank);
          });
        },

        showVirtualhid() {
          return this.gamepadDriver !== 'none' && (this.platform === 'macos' || this.gamepadDriver !== 'vigembus');
        },

        showVigembus() {
          return this.platform === 'windows' && this.gamepadDriver !== 'none' && this.gamepadDriver !== 'virtualhid';
        },

        showVirtualhidBenefits() {
          return this.platform === 'windows' && this.gamepadDriver !== 'none' && !(this.virtualhid.installed && this.virtualhidLicense.licensed);
        },

        virtualInputDescriptionKey() {
          if (this.gamepadDriver === 'none') {
            return 'troubleshooting.virtual_gamepad_none_desc';
          }
          if (this.platform === 'macos') {
            return 'troubleshooting.virtual_gamepad_macos_desc';
          }
          if (!this.gamepadDriver) {
            return 'troubleshooting.virtual_gamepad_unset_desc';
          }
          if (this.gamepadDriver === 'vigembus') {
            return 'troubleshooting.virtual_gamepad_vigembus_desc';
          }
          if (this.virtualhid.installed && this.virtualhidLicense.licensed) {
            return this.gamepadDriver === 'all' ?
              'troubleshooting.virtual_gamepad_licensed_all_desc' :
              'troubleshooting.virtual_gamepad_licensed_desc';
          }
          return 'troubleshooting.virtual_gamepad_desc';
        },

        actualLogs() {
          if (!this.logFilter) return this.logs;
          const filterLower = this.logFilter.toLowerCase();
          return this.logs
            .split("\n")
            .filter((x) => x.toLowerCase().includes(filterLower))
            .join("\n");
        },

        /**
         * Parse the (possibly multi-line) log output into timestamp-prefixed entries.
         * Each entry starts with: [YYYY-MM-DD HH:MM:SS.mmm]:
         */
        parsedLogEntries() {
          const text = this.actualLogs || '';

          // Match on timestamp tokens, but keep everything between them as the entry body.
          // Using a global exec loop lets us split without losing delimiters and works
          // even when entries span multiple lines.
          const tsRegex = /\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\]:/g;

          const entries = [];
          const matches = Array.from(text.matchAll(tsRegex));

          // If no timestamps are found, treat everything as a single entry.
          if (matches.length === 0) {
            const raw = text.trimEnd();
            if (!raw) return [];
            return [
              {
                index: 0,
                raw,
                level: 'Info',
                cssClass: 'log-line-info',
              },
            ];
          }

          for (let i = 0; i < matches.length; i++) {
            const start = matches[i].index;
            const end = i + 1 < matches.length ? matches[i + 1].index : text.length;
            const raw = text.slice(start, end).trimEnd();
            if (!raw) continue;

            // Determine level based on the *first* level token in the entry.
            // Sunshine logs are typically: "[ts]: Level: message".
            // Some messages may contain additional embedded timestamps, but we treat
            // those as part of the entry content.
            let level = 'Info';
            let cssClass = 'log-line-info';

            if (/\]:\s*Fatal:/i.test(raw)) {
              level = 'Fatal';
              cssClass = 'log-line-fatal';
            } else if (/\]:\s*(Error|Critical):/i.test(raw)) {
              level = 'Error';
              cssClass = 'log-line-error';
            } else if (/\]:\s*Warning:/i.test(raw)) {
              level = 'Warning';
              cssClass = 'log-line-warning';
            } else if (/\]:\s*Debug:/i.test(raw)) {
              level = 'Debug';
              cssClass = 'log-line-debug';
            }

            entries.push({
              index: entries.length,
              raw,
              level,
              cssClass,
            });
          }

          return entries;
        },

        highlightedLogs() {
          const escapeHtml = (s) =>
            s
              .replaceAll('&', '&amp;')
              .replaceAll('<', '&lt;')
              .replaceAll('>', '&gt;')
              .replaceAll('"', '&quot;')
              .replaceAll("'", '&#39;');

          return this.parsedLogEntries
            .map((entry) => {
              const safe = escapeHtml(entry.raw);
              const isSelected = entry.index === this.currentLogIndex;
              const selectedClass = isSelected ? ' log-entry-selected' : '';
              return `<span data-entry-index="${entry.index}" data-log-level="${entry.cssClass}" class="${entry.cssClass}${selectedClass}">${safe}</span>`;
            })
            // Separate entries visually with a newline.
            .join("\n");
        },

        errorWarningEntries() {
          // Only navigate between warnings/errors/fatal/critical
          return this.parsedLogEntries
            .filter((e) => e.level === 'Warning' || e.level === 'Error' || e.level === 'Fatal')
            .map((e) => e.index);
        },

        hasNextLog() {
          const indices = this.errorWarningEntries;
          if (indices.length === 0) return false;
          if (this.currentLogIndex === -1) return true;
          return indices.some((i) => i > this.currentLogIndex);
        },

        hasPrevLog() {
          const indices = this.errorWarningEntries;
          if (indices.length === 0) return false;
          if (this.currentLogIndex === -1) return false;
          return indices.some((i) => i < this.currentLogIndex);
        },

        driverReleaseLoading() {
          return this.showVirtualhid && this.virtualhidRelease.loading;
        }
      },
      created() {
        this._logsCopyTimeout = null;
        fetch("./api/config")
          .then((r) => r.json())
          .then((r) => {
            this.platform = r.platform;
            this.gamepadDriver = r.gamepad_driver || '';
            this.refreshPermissions();
            // The Windows broker also backs relative mouse input when gamepads are disabled.
            if (this.platform === 'windows' || this.platform === 'macos') {
              this.refreshDriverInformation();
            }
            if ((this.platform === 'windows' || this.platform === 'macos') && this.showVirtualhid) {
              this.refreshLicenseStatus();
            }
          });

        this.logInterval = setInterval(() => {
          this.refreshLogs();
          if (this.platform) this.refreshPermissions();
        }, 5000);
        this.refreshLogs();
        this.refreshClients();
      },
      beforeUnmount() {
        clearInterval(this.logInterval);
        if (this._logsCopyTimeout) clearTimeout(this._logsCopyTimeout);
      },
      methods: {
        /** Refresh the current platform's permission status. */
        async refreshPermissions() {
          try {
            const response = await fetch('./api/permissions');
            if (!response.ok) throw new Error(this.$t('troubleshooting.permissions_error'));
            this.permissions = (await response.json()).permissions || [];
            this.permissionError = '';
          } catch (error) {
            this.permissionError = error.message;
          }
        },
        /** Initiate the selected native permission action. */
        async requestPermission(id) {
          this.permissionBusy = id;
          try {
            const response = await apiFetch('./api/permissions/request', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ id }),
            });
            if (!response.ok) throw new Error(this.$t('troubleshooting.permissions_error'));
            if (id === 'local_network' || (this.platform === 'macos' && (id === 'screen_recording' || id === 'input'))) {
              this.permissionHelp = id;
            }
            await this.refreshPermissions();
          } catch (error) {
            this.permissionError = error.message;
          } finally {
            this.permissionBusy = '';
          }
        },
        refreshLogs() {
          fetch("./api/logs",)
            .then((r) => r.text())
            .then((r) => {
              this.logs = r;
            });
        },
        closeApp() {
          this.closeAppPressed = true;
          apiFetch("./api/apps/close", {
            method: "POST",
            headers: {
                "Content-Type": "application/json"
            } })
            .then((r) => r.json())
            .then((r) => {
              this.closeAppPressed = false;
              this.closeAppStatus = r.status;
              setTimeout(() => {
                this.closeAppStatus = null;
              }, 5000);
            });
        },
        unpairAll() {
          this.unpairAllPressed = true;
          apiFetch("./api/clients/unpair-all", {
            method: "POST",
            headers: {
                "Content-Type": "application/json"
            }
           })
            .then((r) => r.json())
            .then((r) => {
              this.unpairAllPressed = false;
              this.unpairAllStatus = r.status;
              setTimeout(() => {
                this.unpairAllStatus = null;
              }, 5000);
              this.refreshClients();
            });
        },
        unpairSingle(uuid) {
          apiFetch("./api/clients/unpair", {
            method: "POST",
            headers: {
              'Content-Type': 'application/json'
            },
            body: JSON.stringify({ uuid })
          }).then(() => {
            this.showApplyMessage = true;
            this.refreshClients();
          });
        },
        toggleClient(uuid, enabled) {
          fetch("./api/clients/update", {
            method: "POST",
            headers: {
              'Content-Type': 'application/json'
            },
            body: JSON.stringify({ uuid, enabled })
          }).then(() => {
            this.refreshClients();
          });
        },
        refreshClients() {
          fetch("./api/clients/list")
            .then((response) => response.json())
            .then((response) => {
              if (response.status === true && response.named_certs && response.named_certs.length) {
                this.clients = response.named_certs.sort((a, b) => {
                  return (a.name.toLowerCase() > b.name.toLowerCase() || a.name === "" ? 1 : -1)
                });
              } else {
                this.clients = [];
              }
            });
        },
        clickedApplyBanner() {
          this.showApplyMessage = false;
        },
        copyLogs() {
          // Copy the filtered view if a filter is active.
          navigator.clipboard.writeText(this.actualLogs).then(() => {
            // Clear any existing reset timer (handles rapid successive clicks).
            if (this._logsCopyTimeout) clearTimeout(this._logsCopyTimeout);

            // Show checkmark feedback.
            this.logsCopied = true;

            // Revert icon after 2 seconds.
            this._logsCopyTimeout = setTimeout(() => {
              this.logsCopied = false;
            }, 2000);
          }).catch((err) => {
            console.error('Failed to copy logs:', err);
          });
        },
        restart() {
          this.restartPressed = true;
          setTimeout(() => {
            this.restartPressed = false;
          }, 5000);
          apiFetch("./api/restart", {
            method: "POST",
            headers: {
                "Content-Type": "application/json"
            }
          });
        },
        resetPortalToken() {
          this.portalResetPressed = true;
          apiFetch("./api/reset-portal-token", {
            method: "POST",
            headers: {
              "Content-Type": "application/json"
            }
          })
            .then((r) => r.json())
            .then((r) => {
              this.portalResetStatus = r.status;
              if (r.status) {
                apiFetch("./api/restart", {
                  method: "POST",
                  headers: {
                    "Content-Type": "application/json"
                  }
                });
              }
              setTimeout(() => {
                this.portalResetPressed = false;
                this.portalResetStatus = null;
              }, 5000);
            })
            .catch(() => {
              this.portalResetPressed = false;
              this.portalResetStatus = false;
            });
        },
        ddResetPersistence() {
          this.ddResetPressed = true;
          apiFetch("./api/reset-display-device-persistence", {
            method: "POST",
            headers: {
              "Content-Type": "application/json"
            }
          })
            .then((r) => r.json())
            .then((r) => {
              this.ddResetPressed = false;
              this.ddResetStatus = r.status;
              setTimeout(() => {
                this.ddResetStatus = null;
              }, 5000);
            });
        },
        /**
         * @brief Refresh the installed backend details and latest stable Virtual HID Broker release.
         */
        refreshDriverInformation() {
          this.refreshVirtualInputStatus();
          this.refreshDriverReleases();
        },
        /**
         * @brief Refresh the latest stable release metadata for Virtual HID Broker.
         */
        refreshDriverReleases() {
          if (this.showVirtualhid) {
            this.updateLatestRelease(
              'virtualhidRelease',
              'LizardByte/libvirtualhid',
              'https://github.com/LizardByte/libvirtualhid/releases/latest',
            );
          }
        },
        /**
         * @brief Fetch one repository's latest non-prerelease release from GitHub.
         *
         * @param {string} property Component data property that stores the release metadata.
         * @param {string} repository GitHub repository in owner/name form.
         * @param {string} fallbackUrl Releases page used if the API request fails.
         */
        updateLatestRelease(property, repository, fallbackUrl) {
          this[property] = {
            ...this[property],
            loading: true,
            error: false,
          };

          fetch(`https://api.github.com/repos/${repository}/releases/latest`, {
            headers: {
              Accept: 'application/vnd.github+json',
            },
          })
            .then((response) => {
              if (!response.ok) {
                throw new Error(`GitHub returned ${response.status}`);
              }
              return response.json();
            })
            .then((release) => {
              if (!release.tag_name || release.draft || release.prerelease) {
                throw new Error('GitHub did not return a stable release');
              }
              this[property] = {
                loading: false,
                version: release.tag_name,
                url: release.html_url || fallbackUrl,
                error: false,
              };
            })
            .catch((error) => {
              console.error(`Failed to fetch the latest ${repository} release:`, error);
              this[property] = {
                loading: false,
                version: '',
                url: fallbackUrl,
                error: true,
              };
            });
        },
        refreshVirtualInputStatus() {
          fetch("./api/virtual-input/status")
            .then((r) => r.json())
            .then((r) => {
              const virtualhid = r.virtualhid || {};
              const vigembus = r.vigembus || {};
              this.virtualhid = {
                installed: virtualhid.installed || false,
                version: virtualhid.version || '',
                version_compatible: virtualhid.version_compatible || false,
                minimum_version: virtualhid.minimum_version || '',
                supported_versions: virtualhid.supported_versions || '',
              };
              this.vigembus = {
                installed: vigembus.installed || false,
                version: vigembus.version || '',
                version_compatible: vigembus.version_compatible || false,
                minimum_version: vigembus.minimum_version || '',
                supported_versions: vigembus.supported_versions || '',
              };
            })
            .catch((err) => {
              console.error("Failed to fetch virtual input driver status:", err);
            });
        },
        /**
         * @brief Parse a numeric dotted driver or release version.
         *
         * @param {string} version Version string, optionally prefixed with "v".
         * @return {number[]|null} Numeric version parts, or null for an invalid version.
         */
        parseDriverVersion(version) {
          const match = /^v?(\d+(?:\.\d+)*)/i.exec(String(version || '').trim());
          return match ? match[1].split('.').map(Number) : null;
        },
        /**
         * @brief Compare installed and latest driver versions.
         *
         * @param {string} installedVersion Installed driver version.
         * @param {string} latestVersion Latest stable release version.
         * @return {number|null} Negative if outdated, zero if equal, positive if newer, or null if invalid.
         */
        compareDriverVersions(installedVersion, latestVersion) {
          const installedParts = this.parseDriverVersion(installedVersion);
          const latestParts = this.parseDriverVersion(latestVersion);
          if (!installedParts || !latestParts) {
            return null;
          }

          const partCount = Math.max(installedParts.length, latestParts.length);
          for (let index = 0; index < partCount; ++index) {
            const installedPart = installedParts[index] || 0;
            const latestPart = latestParts[index] || 0;
            if (installedPart !== latestPart) {
              return installedPart > latestPart ? 1 : -1;
            }
          }
          return 0;
        },
        /**
         * @brief Get the availability state for one installed driver and GitHub release.
         *
         * @param {object} driver Installed driver status.
         * @param {object} release Latest GitHub release status.
         * @return {string} Release state used to select the icon and message.
         */
        driverReleaseState(driver, release) {
          if (release.loading) {
            return 'loading';
          }
          if (release.error || !release.version) {
            return 'unavailable';
          }
          if (!driver.installed) {
            return 'not-installed';
          }

          const comparison = this.compareDriverVersions(driver.version, release.version);
          if (comparison === null) {
            return 'unknown';
          }
          return comparison >= 0 ? 'current' : 'outdated';
        },
        /**
         * @brief Get the localized release comparison message for a driver.
         *
         * @param {object} driver Installed driver status.
         * @param {object} release Latest GitHub release status.
         * @return {string} Localized release comparison message.
         */
        driverReleaseStatusText(driver, release) {
          const state = this.driverReleaseState(driver, release).replace('-', '_');
          return this.$t(`troubleshooting.driver_release_${state}`);
        },
        /**
         * @brief Get a theme-aware text class for a driver release state.
         *
         * @param {object} driver Installed driver status.
         * @param {object} release Latest GitHub release status.
         * @return {string} CSS class for the release icon and version.
         */
        driverReleaseStateClass(driver, release) {
          const state = this.driverReleaseState(driver, release);
          if (state === 'current') {
            return 'driver-release-current';
          }
          if (state === 'outdated') {
            return 'driver-release-outdated';
          }
          return 'driver-release-neutral';
        },
        /**
         * @brief Format a latest release version for the drivers table.
         *
         * @param {object} release Latest GitHub release status.
         * @return {string} Release version or a localized placeholder.
         */
        driverLatestVersion(release) {
          if (release.loading) {
            return this.$t('troubleshooting.driver_release_checking');
          }
          return release.version || this.$t('troubleshooting.driver_release_unavailable');
        },
        applyLicenseStatus(status) {
          this.virtualhidLicense = {
            ...this.virtualhidLicense,
            ...status,
          };
          this.licenseError = status.error || '';
        },
        refreshLicenseStatus() {
          this.licenseBusy = true;
          this.licenseError = '';
          fetch("./api/virtual-input/license")
            .then(async (response) => {
              const status = await response.json();
              if (!response.ok) {
                throw new Error(status.error || this.$t('troubleshooting.virtualhid_license_request_failed'));
              }
              this.applyLicenseStatus(status);
            })
            .catch((error) => {
              this.licenseError = error.message;
            })
            .finally(() => {
              this.licenseBusy = false;
            });
        },
        updateLicense(action) {
          if (this.licenseBusy || (action === 'activate' && !this.licenseKey.trim())) {
            return;
          }

          this.licenseBusy = true;
          this.licenseError = '';
          const body = { action };
          if (action === 'activate') {
            body.license_key = this.licenseKey.trim();
          }

          apiFetch("./api/virtual-input/license", {
            method: "POST",
            headers: {
              "Content-Type": "application/json",
            },
            body: JSON.stringify(body),
          })
            .then(async (response) => {
              const status = await response.json();
              if (!response.ok) {
                throw new Error(status.error || this.$t('troubleshooting.virtualhid_license_request_failed'));
              }
              this.applyLicenseStatus(status);
              if (action === 'activate' && status.operation_ok) {
                this.licenseKey = '';
              }
            })
            .catch((error) => {
              this.licenseError = error.message;
            })
            .finally(() => {
              body.license_key = '';
              this.licenseBusy = false;
            });
        },
        licenseStatusClass() {
          if (this.virtualhidLicense.state === 'licensed') {
            return 'badge text-bg-success';
          }
          if (['expired', 'disabled', 'invalid'].includes(this.virtualhidLicense.state)) {
            return 'badge text-bg-danger';
          }
          return 'badge text-bg-secondary';
        },
        licenseStatusText() {
          const key = `troubleshooting.virtualhid_license_state_${this.virtualhidLicense.state}`;
          return this.$t(key);
        },
        licenseActivationLimitText() {
          if (!this.virtualhidLicense.activation_limit) {
            return this.$t('troubleshooting.virtualhid_license_not_reported');
          }
          return String(this.virtualhidLicense.activation_limit);
        },
        driverVersion(driver) {
          if (!driver.installed) {
            return this.$t('troubleshooting.driver_not_installed');
          }
          return driver.version || this.$t('troubleshooting.driver_version_unknown');
        },
        /**
         * @brief Choose a visual state for a permission status icon.
         *
         * @param {string} status Permission status returned by the platform.
         * @return {string} Theme-aware status icon class.
         */
        permissionStatusClass(status) {
          if (status === 'granted') return 'status-icon-success';
          if (status === 'denied') return 'status-icon-danger';
          if (status === 'on_use') return 'status-icon-primary';
          if (status === 'not_determined') return 'status-icon-neutral';
          return 'status-icon-warning';
        },
        /**
         * @brief Choose a visual state for a virtual input backend status icon.
         *
         * @param {object} driver Installed backend status.
         * @return {string} Theme-aware status icon class.
         */
        driverStatusClass(driver) {
          if (!driver.installed) {
            return 'status-icon-neutral';
          }
          return driver.version_compatible ? 'status-icon-success' : 'status-icon-danger';
        },
        driverStatusText(driver) {
          if (!driver.installed) {
            return this.$t('troubleshooting.driver_status_not_installed');
          }
          if (driver.version_compatible) {
            return this.$t('troubleshooting.driver_status_compatible');
          }
          return this.$t('troubleshooting.driver_status_unsupported');
        },
        navigateToLog(direction) {
          const indices = this.errorWarningEntries;
          if (indices.length === 0) return;

          let targetIndex;

          if (direction === 'next') {
            if (this.currentLogIndex === -1) {
              targetIndex = indices[0];
            } else {
              const nextIndices = indices.filter((i) => i > this.currentLogIndex);
              if (nextIndices.length === 0) return;
              targetIndex = nextIndices[0];
            }
          } else if (direction === 'prev') {
            if (this.currentLogIndex === -1) return;
            const prevIndices = indices.filter((i) => i < this.currentLogIndex);
            if (prevIndices.length === 0) return;
            targetIndex = prevIndices[prevIndices.length - 1];
          } else {
            return;
          }

          this.currentLogIndex = targetIndex;

          this.$nextTick(() => {
            const container = this.$refs.logsContainer;
            if (!container) return;

            const el = container.querySelector(`[data-entry-index="${targetIndex}"]`);
            if (!el) return;

            // Ensure it's visible even for tall multi-line entries.
            const containerRect = container.getBoundingClientRect();
            const elRect = el.getBoundingClientRect();
            const relativeTop = elRect.top - containerRect.top;

            container.scrollTop = container.scrollTop + relativeTop - containerRect.height * 0.15;
          });
        },
        scrollLogsTo(where) {
          const container = this.$refs.logsContainer;
          if (!container) return;

          // Reset the selected error/warning index when jumping to top or bottom
          this.currentLogIndex = -1;

          if (where === 'top') {
            container.scrollTo({ top: 0, behavior: 'smooth' });
            return;
          }

          if (where === 'bottom') {
            container.scrollTo({ top: container.scrollHeight, behavior: 'smooth' });
          }
        },
      },
    }
  </script>
