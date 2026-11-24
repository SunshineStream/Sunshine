<template>
  <Navbar></Navbar>
  <div id="content" class="container">
    <div class="my-4">
      <h1>{{ $t('apps.applications_title') }}<span v-if="apps.length"> ({{ appCountLabel }})</span></h1>
      <p>{{ $t('apps.applications_desc') }}</p>
    </div>

    <!-- Actions toolbar -->
    <div class="toolbar apps-toolbar d-flex flex-wrap justify-content-between align-items-center gap-2 mb-3">
      <!-- Left side actions -->
      <div class="d-flex align-items-center gap-2">
        <!-- Add new application -->
        <button class="btn btn-primary" @click="newApp">
          <layers-plus :size="18" class="icon"></layers-plus>
          {{ $t('apps.add_new') }}
        </button>
      </div>
      <!-- Right side actions -->
      <div class="d-flex align-items-center gap-2" v-if="apps && apps.length > 0">
        <!-- Sort by name toggle -->
        <button class="btn btn-outline-secondary text-nowrap" type="button" @click="toggleSort"
                :class="{ active: sortMode !== 'default' }"
                :aria-pressed="sortMode !== 'default'"
                :title="$t('apps.sort_by_name') + ': ' + sortModeLabel">
          <arrow-up-down v-if="sortMode === 'default'" :size="16" class="icon me-1"></arrow-up-down>
          <arrow-up v-else-if="sortMode === 'asc'" :size="16" class="icon me-1"></arrow-up>
          <arrow-down v-else :size="16" class="icon me-1"></arrow-down>
          {{ $t('apps.sort_by_name') }}
        </button>
        <!-- Search box -->
        <div class="input-group">
          <label for="app-search" class="visually-hidden">{{ $t('apps.search_placeholder') }}</label>
          <input id="app-search" type="text" class="form-control" v-model="searchQuery" :placeholder="$t('apps.search_placeholder')" />
          <button v-if="searchQuery" class="btn btn-outline-secondary" type="button" @click="resetSearchQuery" :aria-label="$t('_common.close')">
            <x :size="16" class="icon"></x>
          </button>
          <span v-else class="input-group-text">
            <search :size="16" class="icon"></search>
          </span>
        </div>
      </div>
    </div>

    <!-- Apps Grid -->
    <div class="row g-3" v-if="displayedApps.length > 0">
      <div class="col-12 col-sm-6 col-md-4 col-lg-3" v-for="{ app, index } in displayedApps" :key="index">
        <div class="card app-card h-100">
          <div class="app-poster-container">
            <img
              v-if="app['image-path']"
              :src="'/api/covers/' + index"
              class="app-poster"
              :alt="app.name"
              @error="handleImageError"
            />
            <div v-else class="app-poster-placeholder">
              <span class="app-initial">{{ app.name.charAt(0).toUpperCase() }}</span>
            </div>
            <div class="app-poster-overlay">
              <div v-if="app.cmd" class="app-overlay-row" :title="app.cmd">
                <terminal :size="14" class="icon me-1"></terminal>
                <span class="app-detail-text">{{ app.cmd }}</span>
              </div>
              <div v-if="app['working-dir']" class="app-overlay-row" :title="app['working-dir']">
                <folder :size="14" class="icon me-1"></folder>
                <span class="app-detail-text">{{ app['working-dir'] }}</span>
              </div>
              <div class="app-overlay-badges">
                <span v-if="app.elevated" class="badge app-flag-badge">{{ $t('apps.badge_admin') }}</span>
                <span v-if="app.detached && app.detached.length" class="badge app-flag-badge">{{ $t('apps.badge_detached') }}</span>
                <span v-if="app['prep-cmd'] && app['prep-cmd'].length" class="badge app-flag-badge">{{ $t('apps.badge_app_prep') }}</span>
                <span v-if="app['exclude-global-prep-cmd']" class="badge app-flag-badge">{{ $t('apps.badge_no_global_prep') }}</span>
                <span v-if="app['auto-detach']" class="badge app-flag-badge">{{ $t('apps.badge_auto_detach') }}</span>
              </div>
            </div>
          </div>
          <div class="card-body d-flex flex-column">
            <h5 class="card-title mb-3">{{ app.name }}</h5>
            <div class="mt-auto d-flex gap-2">
              <button class="btn btn-sm btn-primary flex-fill" @click="editApp(index)">
                <edit :size="16" class="icon"></edit>
                {{ $t('apps.edit') }}
              </button>
              <button class="btn btn-sm btn-danger" @click="showDeleteModal(index)">
                <trash-2 :size="16" class="icon"></trash-2>
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- No search results -->
    <div v-else-if="apps && apps.length > 0" class="card">
      <div class="card-body text-center py-5">
        <p class="text-muted">{{ $t('apps.no_search_results') }}</p>
      </div>
    </div>

    <!-- Empty State -->
    <div v-else class="card">
      <div class="card-body text-center py-5">
        <p class="text-muted">{{ $t('apps.no_applications') }}</p>
      </div>
    </div>

    <!-- Edit / Add Application modal -->
    <div class="modal fade" ref="editModal" tabindex="-1" aria-labelledby="appEditModalLabel"
         aria-hidden="true" data-bs-backdrop="static" data-bs-keyboard="false">
      <div class="modal-dialog modal-xl modal-dialog-scrollable modal-fullscreen-lg-down">
        <div class="modal-content" v-if="editForm">
          <div class="modal-header">
            <h5 class="modal-title" id="appEditModalLabel">
              {{ editModalTitle }}
            </h5>
            <button type="button" class="btn-close" @click="closeEditModal"
              :aria-label="$t('_common.close')"></button>
          </div>
          <div class="modal-body">
            <!-- Error -->
            <div v-if="editFormError" class="alert alert-danger py-2 small">{{ editFormError }}</div>
            <!-- Application Name -->
            <div class="mb-3">
              <label for="appName" class="form-label">{{ $t('apps.app_name') }}</label>
              <input type="text" class="form-control" id="appName" aria-describedby="appNameHelp" v-model="editForm.name" />
              <div id="appNameHelp" class="form-text">{{ $t('apps.app_name_desc') }}</div>
            </div>
            <!-- output -->
            <div class="mb-3">
              <label for="appOutput" class="form-label">{{ $t('apps.output_name') }}</label>
              <div class="input-group">
                <input type="text" class="form-control monospace" id="appOutput" aria-describedby="appOutputHelp"
                  v-model="editForm.output" />
                <button class="btn btn-secondary" type="button"
                  @click="browseFor('any', 'file_browser.select_file', editForm.output, v => editForm.output = v)">
                  <folder-open :size="18" class="icon"></folder-open>
                </button>
              </div>
              <div id="appOutputHelp" class="form-text">{{ $t('apps.output_desc') }}</div>
            </div>
            <!-- prep-cmd -->
            <Checkbox class="mb-3"
                      id="excludeGlobalPrep"
                      label="apps.global_prep_name"
                      desc="apps.global_prep_desc"
                      v-model="editForm['exclude-global-prep-cmd']"
                      default="true"
                      inverse-values
            ></Checkbox>
            <div class="mb-3">
              <label for="appName" class="form-label">{{ $t('apps.cmd_prep_name') }}</label>
              <div class="form-text">{{ $t('apps.cmd_prep_desc') }}</div>
              <div class="d-flex justify-content-start mb-3 mt-3" v-if="editForm['prep-cmd'].length === 0">
                <button class="btn btn-success" @click="addPrepCmd">
                  <plus :size="18" class="icon"></plus>
                  {{ $t('apps.add_cmds') }}
                </button>
              </div>
              <table class="table" v-if="editForm['prep-cmd'].length > 0">
                <thead>
                  <tr>
                    <th scope="col">
                      <play :size="18" class="icon"></play>
                      {{ $t('_common.do_cmd') }}
                    </th>
                    <th scope="col">
                      <rotate-ccw :size="18" class="icon"></rotate-ccw>
                      {{ $t('_common.undo_cmd') }}
                    </th>
                    <th scope="col" v-if="platform === 'windows'">
                      <shield :size="18" class="icon"></shield>
                      {{ $t('_common.run_as') }}
                    </th>
                    <th scope="col"></th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="(c, i) in editForm['prep-cmd']" :key="i">
                    <td>
                      <div class="input-group">
                        <label :for="`prep-cmd-do-${i}`" class="visually-hidden">{{ $t('_common.do_cmd') }}</label>
                        <input :id="`prep-cmd-do-${i}`" type="text" class="form-control monospace" v-model="c.do" />
                        <button class="btn btn-secondary btn-sm" type="button" @click="browsePrep(i, 'do')">
                          <folder-open :size="14" class="icon"></folder-open>
                        </button>
                      </div>
                    </td>
                    <td>
                      <div class="input-group">
                        <label :for="`prep-cmd-undo-${i}`" class="visually-hidden">{{ $t('_common.undo_cmd') }}</label>
                        <input :id="`prep-cmd-undo-${i}`" type="text" class="form-control monospace" v-model="c.undo" />
                        <button class="btn btn-secondary btn-sm" type="button" @click="browsePrep(i, 'undo')">
                          <folder-open :size="14" class="icon"></folder-open>
                        </button>
                      </div>
                    </td>
                    <td v-if="platform === 'windows'" class="align-middle">
                      <Checkbox :id="'prep-cmd-admin-' + i"
                                label="_common.elevated"
                                desc=""
                                v-model="c.elevated"
                      ></Checkbox>
                    </td>
                    <td class="align-middle">
                      <button class="btn btn-danger btn-sm ms-2" @click="deletePrepCmd(i)">
                        <trash-2 :size="16" class="icon"></trash-2>
                      </button>
                      <button class="btn btn-success btn-sm ms-2" @click="addPrepCmd">
                        <plus :size="16" class="icon"></plus>
                      </button>
                    </td>
                  </tr>
                </tbody>
              </table>
            </div>
            <!-- detached -->
            <div class="mb-3">
              <label for="appName" class="form-label">{{ $t('apps.detached_cmds') }}</label>
              <div v-for="(c,i) in editForm.detached" :key="i" class="d-flex justify-content-between align-items-center my-2">
                <label :for="`detached-command-${i}`" class="visually-hidden">{{ $t('apps.detached_cmds') }}</label>
                <input :id="`detached-command-${i}`" type="text" v-model="editForm.detached[i]" class="form-control monospace">
                <button class="btn btn-secondary btn-sm ms-2" @click="browseDetached(i)">
                  <folder-open :size="14" class="icon"></folder-open>
                </button>
                <button class="btn btn-danger btn-sm ms-2" @click="editForm.detached.splice(i,1)">
                  <trash-2 :size="16" class="icon"></trash-2>
                </button>
                <button class="btn btn-success btn-sm ms-2" @click="addDetached">
                  <plus :size="16" class="icon"></plus>
                </button>
              </div>
              <div class="d-flex justify-content-start mb-3 mt-3" v-if="editForm.detached.length === 0">
                <button class="btn btn-success" @click="addDetached">
                  <plus :size="18" class="icon"></plus>
                  {{ $t('apps.detached_cmds_add') }}
                </button>
              </div>
              <div class="form-text">
                {{ $t('apps.detached_cmds_desc') }}<br>
                <b>{{ $t('_common.note') }}</b> {{ $t('apps.detached_cmds_note') }}
              </div>
            </div>
            <!-- command -->
            <div class="mb-3">
              <label for="appCmd" class="form-label">{{ $t('apps.cmd') }}</label>
              <div class="input-group">
                <input type="text" class="form-control monospace" id="appCmd" aria-describedby="appCmdHelp"
                  v-model="editForm.cmd" />
                <button class="btn btn-secondary" type="button"
                  @click="browseFor('executable', 'file_browser.select_executable', editForm.cmd, v => editForm.cmd = v)">
                  <folder-open :size="18" class="icon"></folder-open>
                </button>
              </div>
              <div id="appCmdHelp" class="form-text">
                {{ $t('apps.cmd_desc') }}<br>
                <b>{{ $t('_common.note') }}</b> {{ $t('apps.cmd_note') }}
              </div>
            </div>
            <!-- working dir -->
            <div class="mb-3">
              <label for="appWorkingDir" class="form-label">{{ $t('apps.working_dir') }}</label>
              <div class="input-group">
                <input type="text" class="form-control monospace" id="appWorkingDir" aria-describedby="appWorkingDirHelp"
                  v-model="editForm['working-dir']" />
                <button class="btn btn-secondary" type="button"
                  @click="browseFor('directory', 'file_browser.select_directory', editForm['working-dir'], v => editForm['working-dir'] = v)">
                  <folder-open :size="18" class="icon"></folder-open>
                </button>
              </div>
              <div id="appWorkingDirHelp" class="form-text">{{ $t('apps.working_dir_desc') }}</div>
            </div>
            <!-- elevation -->
            <Checkbox v-if="platform === 'windows'"
                      class="mb-3"
                      id="appElevation"
                      label="_common.run_as"
                      desc="apps.run_as_desc"
                      v-model="editForm.elevated"
                      default="false"
            ></Checkbox>
            <!-- auto-detach -->
            <Checkbox class="mb-3"
                      id="autoDetach"
                      label="apps.auto_detach"
                      desc="apps.auto_detach_desc"
                      v-model="editForm['auto-detach']"
                      default="true"
            ></Checkbox>
            <!-- wait for all processes -->
            <Checkbox class="mb-3"
                      id="waitAll"
                      label="apps.wait_all"
                      desc="apps.wait_all_desc"
                      v-model="editForm['wait-all']"
                      default="true"
            ></Checkbox>
            <!-- exit timeout -->
            <div class="mb-3">
              <label for="exitTimeout" class="form-label">{{ $t('apps.exit_timeout') }}</label>
              <input type="number" class="form-control monospace" id="exitTimeout" aria-describedby="exitTimeoutHelp"
                     v-model="editForm['exit-timeout']" min="0" placeholder="5" />
              <div id="exitTimeoutHelp" class="form-text">{{ $t('apps.exit_timeout_desc') }}</div>
            </div>
            <div class="mb-3">
              <label for="appImagePath" class="form-label">{{ $t('apps.image') }}</label>
              <div class="input-group">
                <input type="text" class="form-control monospace" id="appImagePath" aria-describedby="appImagePathHelp"
                  v-model="editForm['image-path']" />
                <button class="btn btn-secondary" type="button"
                  @click="browseFor('file', 'file_browser.select_file', editForm['image-path'], v => editForm['image-path'] = v, ['.png'])">
                  <folder-open :size="18" class="icon"></folder-open>
                </button>
                <button class="btn btn-secondary" type="button" @click="showCoverFinder">
                  <search :size="18" class="icon"></search>
                  {{ $t('apps.find_cover') }}
                </button>
              </div>
              <div id="appImagePathHelp" class="form-text">{{ $t('apps.image_desc') }}</div>
            </div>
            <div class="env-hint alert alert-info">
              <div class="form-text">
                <h4>{{ $t('apps.env_vars_about') }}</h4>
                {{ $t('apps.env_vars_desc') }}
              </div>
              <table class="env-table">
                <tr>
                  <th scope="col">{{ $t('apps.env_var_name') }}</th>
                  <th scope="col">{{ $t('apps.env_var_description') }}</th>
                </tr>
                <tr>
                  <td style="font-family: monospace">SUNSHINE_APP_ID</td>
                  <td>{{ $t('apps.env_app_id') }}</td>
                </tr>
                <tr>
                  <td style="font-family: monospace">SUNSHINE_APP_NAME</td>
                  <td>{{ $t('apps.env_app_name') }}</td>
                </tr>
                <tr>
                  <td style="font-family: monospace">SUNSHINE_CLIENT_NAME</td>
                  <td>{{ $t('apps.env_client_name') }}</td>
                </tr>
                <tr>
                  <td style="font-family: monospace">SUNSHINE_CLIENT_WIDTH</td>
                  <td>{{ $t('apps.env_client_width') }}</td>
                </tr>
                <tr>
                  <td style="font-family: monospace">SUNSHINE_CLIENT_HEIGHT</td>
                  <td>{{ $t('apps.env_client_height') }}</td>
                </tr>
                <tr>
                  <td style="font-family: monospace">SUNSHINE_CLIENT_FPS</td>
                  <td>{{ $t('apps.env_client_fps') }}</td>
                </tr>
                <tr>
                  <td style="font-family: monospace">SUNSHINE_CLIENT_HDR</td>
                  <td>{{ $t('apps.env_client_hdr') }}</td>
                </tr>
                <tr>
                  <td style="font-family: monospace">SUNSHINE_CLIENT_GCMAP</td>
                  <td>{{ $t('apps.env_client_gcmap') }}</td>
                </tr>
                <tr>
                  <td style="font-family: monospace">SUNSHINE_CLIENT_HOST_AUDIO</td>
                  <td>{{ $t('apps.env_client_host_audio') }}</td>
                </tr>
                <tr>
                  <td style="font-family: monospace">SUNSHINE_CLIENT_ENABLE_SOPS</td>
                  <td>{{ $t('apps.env_client_enable_sops') }}</td>
                </tr>
                <tr>
                  <td style="font-family: monospace">SUNSHINE_CLIENT_AUDIO_CONFIGURATION</td>
                  <td>{{ $t('apps.env_client_audio_config') }}</td>
                </tr>
              </table>
              <div class="form-text" v-if="platform === 'windows'"><b>{{ $t('apps.env_qres_example') }}</b>
                <pre>cmd /C &lt;{{ $t('apps.env_qres_path') }}&gt;\QRes.exe /X:%SUNSHINE_CLIENT_WIDTH% /Y:%SUNSHINE_CLIENT_HEIGHT% /R:%SUNSHINE_CLIENT_FPS%</pre>
              </div>
              <div class="form-text" v-else-if="platform === 'freebsd' || platform === 'linux'"><b>{{ $t('apps.env_xrandr_example') }}</b>
                <pre>sh -c "xrandr --output HDMI-1 --mode \"${SUNSHINE_CLIENT_WIDTH}x${SUNSHINE_CLIENT_HEIGHT}\" --rate ${SUNSHINE_CLIENT_FPS}"</pre>
              </div>
              <div class="form-text" v-else-if="platform === 'macos'"><b>{{ $t('apps.env_displayplacer_example') }}</b>
                <pre>sh -c "displayplacer "id:&lt;screenId&gt; res:${SUNSHINE_CLIENT_WIDTH}x${SUNSHINE_CLIENT_HEIGHT} hz:${SUNSHINE_CLIENT_FPS} scaling:on origin:(0,0) degree:0""</pre>
              </div>
              <div class="form-text"><a
                  :href="`${documentationBaseUrl}/md_docs_2app__examples.html`"
                  target="_blank">{{ $t('_common.see_more') }}</a></div>
            </div>
          </div>
          <!-- Save buttons -->
          <div class="modal-footer">
            <button @click="closeEditModal" class="btn btn-secondary">
              <x :size="18" class="icon"></x>
              {{ $t('_common.cancel') }}
            </button>
            <button class="btn btn-primary" @click="save">
              <save :size="18" class="icon"></save>
              {{ $t('_common.save') }}
            </button>
          </div>
        </div>
      </div>
    </div>

    <!-- Cover Finder modal -->
    <div class="modal fade" id="coverFinderModal" tabindex="-1" aria-labelledby="coverFinderModalLabel" aria-hidden="true" ref="coverFinderModal">
      <div class="modal-dialog modal-xl modal-dialog-scrollable modal-fullscreen-md-down">
        <div class="modal-content">
          <div class="modal-header">
            <h5 class="modal-title" id="coverFinderModalLabel">
              <span v-if="coverSearching">{{ $t('apps.searching_covers') }}</span>
              <span v-else-if="coverCandidates.length > 0">{{ $t('apps.covers_found') }} ({{ coverCandidates.length }})</span>
              <span v-else>{{ $t('apps.no_covers_found') }}</span>
            </h5>
            <button type="button" class="btn-close" data-bs-dismiss="modal" :aria-label="$t('_common.close')"></button>
          </div>
          <div class="modal-body">
            <div class="mb-3">
              <div class="input-group">
                <label for="cover-search-query" class="visually-hidden">{{ $t('_common.search') }}</label>
                <input
                  id="cover-search-query"
                  type="text"
                  class="form-control"
                  v-model="coverSearchQuery"
                  :placeholder="editForm?.name"
                  @keyup.enter="performCoverSearch"
                />
                <button class="btn btn-primary" type="button" @click="performCoverSearch">
                  <search :size="18" class="icon"></search>
                  {{ $t('_common.search') }}
                </button>
              </div>
              <div class="form-text mt-2">
                <b>{{ $t('_common.note') }}</b> {{ $t('apps.cover_search_hint') }}
                <a href="https://www.igdb.com/" target="_blank" rel="noopener noreferrer">IGDB</a>
              </div>
            </div>
            <div class="cover-results" :class="{ busy: coverFinderBusy }">
              <div class="row">
                <div v-if="coverSearching" class="col-12 col-sm-6 col-lg-4 mb-3">
                  <div class="cover-container">
                    <output class="spinner-border">
                      <span class="visually-hidden">{{ $t('apps.loading') }}</span>
                    </output>
                  </div>
                </div>
                <button type="button" v-for="cover in coverCandidates" :key="cover.url" class="cover-choice col-12 col-sm-6 col-lg-3 mb-3"
                  @click="useCover(cover)">
                  <div class="cover-container result">
                    <img class="rounded" :src="cover.url" :alt="cover.name" />
                  </div>
                  <span class="d-block text-nowrap text-center text-truncate">
                    {{cover.name}}
                  </span>
                </button>
              </div>
            </div>
          </div>
          <div class="modal-footer">
            <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">
              <x :size="18" class="icon"></x>
              {{ $t('_common.cancel') }}
            </button>
          </div>
        </div>
      </div>
    </div>

    <!-- Delete confirmation modal -->
    <div class="modal fade" ref="deleteModal" tabindex="-1" aria-labelledby="appDeleteModalLabel" aria-hidden="true">
      <div class="modal-dialog modal-dialog-centered">
        <div class="modal-content" v-if="deleteTarget">
          <div class="modal-header">
            <h5 class="modal-title" id="appDeleteModalLabel">{{ $t('apps.delete_title') }}</h5>
            <button type="button" class="btn-close" @click="closeDeleteModal"
              :aria-label="$t('_common.close')"></button>
          </div>
          <div class="modal-body">
            <i18n-t keypath="apps.delete_confirm" tag="span">
              <template #name><strong>{{ deleteTarget.name }}</strong></template>
            </i18n-t>
          </div>
          <div class="modal-footer">
            <button type="button" class="btn btn-secondary" @click="closeDeleteModal">
              <x :size="18" class="icon"></x>
              {{ $t('_common.cancel') }}
            </button>
            <button type="button" class="btn btn-danger" @click="confirmDelete">
              <trash-2 :size="18" class="icon"></trash-2>
              {{ $t('apps.delete') }}
            </button>
          </div>
        </div>
      </div>
    </div>

    <!-- Shared file browser modal -->
    <div class="modal fade" ref="fileBrowserModal" tabindex="-1" aria-hidden="true">
      <div class="modal-dialog modal-lg modal-dialog-scrollable modal-fullscreen-md-down">
        <div class="modal-content">
          <div class="modal-header">
            <h5 class="modal-title">{{ fileBrowserTitle || $t('file_browser.title') }}</h5>
            <button type="button" class="btn-close" @click="fileBrowserClose" :aria-label="$t('_common.close')"></button>
          </div>
          <div class="modal-body">
            <!-- Path input -->
            <div class="input-group mb-2">
              <label for="file-browser-path" class="visually-hidden">{{ $t('file_browser.title') }}</label>
              <input id="file-browser-path" type="text" class="form-control monospace" v-model="fileBrowserTypedPath"
                @input="fileBrowserOnTypedInput" @keyup.enter="fileBrowserNavigate(fileBrowserTypedPath)" />
              <button class="btn btn-secondary" type="button" @click="fileBrowserNavigate(fileBrowserTypedPath)">
                <arrow-right :size="16" class="icon"></arrow-right>
              </button>
            </div>
            <!-- Up button -->
            <div class="mb-2">
              <button class="btn btn-sm btn-outline-secondary" type="button"
                :disabled="fileBrowserLoading || fileBrowserParentPath === fileBrowserCurrentPath"
                @click="fileBrowserNavigateUp">
                <folder-up :size="16" class="icon me-1"></folder-up>
                {{ $t('file_browser.up') }}
              </button>
            </div>
            <!-- Error -->
            <div v-if="fileBrowserError" class="alert alert-danger py-2 small">{{ fileBrowserError }}</div>
            <!-- Loading -->
            <div v-if="fileBrowserLoading" class="text-center py-3">
              <output class="spinner-border spinner-border-sm">
                <span class="visually-hidden">{{ $t('_common.loading') }}</span>
              </output>
            </div>
            <!-- Entries -->
            <div v-else class="list-group" style="max-height: 400px; overflow-y: auto;">
              <div v-if="fileBrowserEntries.length === 0" class="list-group-item text-muted text-center">
                {{ $t('file_browser.empty') }}
              </div>
              <button v-for="entry in fileBrowserEntries" :key="entry.path" type="button"
                class="list-group-item list-group-item-action d-flex align-items-center py-1"
                :class="{ active: fileBrowserSelectedPath === entry.path }"
                @click="fileBrowserSelectEntry(entry)" @dblclick="fileBrowserActivateEntry(entry)">
                <hard-drive v-if="!fileBrowserCurrentPath && entry.type === 'directory'" :size="16" class="icon me-2 flex-shrink-0"></hard-drive>
                <folder v-else-if="entry.type === 'directory'" :size="16" class="icon me-2 flex-shrink-0 text-warning"></folder>
                <file-text v-else :size="16" class="icon me-2 flex-shrink-0"></file-text>
                <span class="text-truncate">{{ entry.name }}</span>
              </button>
            </div>
          </div>
          <div class="modal-footer flex-wrap gap-2">
            <div class="flex-grow-1 text-muted small text-truncate" v-if="fileBrowserSelectedPath">
              <code>{{ fileBrowserSelectedPath }}</code>
            </div>
            <button type="button" class="btn btn-secondary" @click="fileBrowserClose">
              <x :size="16" class="icon me-1"></x>
              {{ $t('_common.cancel') }}
            </button>
            <button type="button" class="btn btn-primary" @click="fileBrowserConfirm"
              :disabled="!fileBrowserSelectedPath && !fileBrowserTypedPath">
              <check :size="16" class="icon me-1"></check>
              {{ $t('file_browser.select') }}
            </button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
  import { toRaw } from 'vue'
  import Navbar from './Navbar.vue'
  import Checkbox from './Checkbox.vue'
  import { apiFetch } from './fetch_utils'
  import SunshineVersion from './sunshine_version'
  import { Modal } from 'bootstrap/dist/js/bootstrap'
  import {
    ArrowDown,
    ArrowRight,
    ArrowUp,
    ArrowUpDown,
    Check,
    Edit,
    FileText,
    Folder,
    FolderOpen,
    FolderUp,
    HardDrive,
    LayersPlus,
    Play,
    Plus,
    RotateCcw,
    Save,
    Search,
    Shield,
    Terminal,
    Trash2,
    X,
  } from '@lucide/vue'

  /**
   * Return the GameDB bucket for an application name.
   *
   * @param {string} name Application name to categorize.
   * @returns {string} The normalized GameDB bucket name.
   */
  function getSearchBucket(name) {
    const bucket = name.substring(0, Math.min(name.length, 2)).toLowerCase().replaceAll(/[^a-z\d]/g, '');
    return bucket || '@';
  }

  /**
   * Search GameDB for cover candidates matching an application name.
   *
   * @param {string} name Application name to search for.
   * @returns {Promise<object[]>} Matching cover candidates.
   */
  function searchCovers(name) {
    if (!name) {
      return Promise.resolve([]);
    }
    let searchName = name.replaceAll(/\s+/g, '.').toLowerCase();

    // Use raw.githubusercontent.com to avoid CORS issues as we migrate the CNAME
    let dbUrl = "https://raw.githubusercontent.com/LizardByte/GameDB/gh-pages";
    let bucket = getSearchBucket(name);
    return fetch(`${dbUrl}/buckets/${bucket}.json`).then(function (r) {
      if (!r.ok) throw new Error("Failed to search covers");
      return r.json();
    }).then(maps => Promise.all(Object.keys(maps).map(id => {
      let item = maps[id];
      if (item.name.replaceAll(/\s+/g, '.').toLowerCase().startsWith(searchName)) {
        return fetch(`${dbUrl}/games/${id}.json`).then(function (r) {
          return r.json();
        }).catch(() => null);
      }
      return null;
    }).filter(Boolean)))
      .then(results => results
        .filter(item => item && item.cover && item.cover.url)
        .map(game => {
          const thumb = game.cover.url;
          const dotIndex = thumb.lastIndexOf('.');
          const slashIndex = thumb.lastIndexOf('/');
          if (dotIndex < 0 || slashIndex < 0) {
            return null;
          }
          const slug = thumb.substring(slashIndex + 1, dotIndex);
          return {
            name: game.name,
            key: `igdb_${game.id}`,
            url: `https://images.igdb.com/igdb/image/upload/t_cover_big/${slug}.jpg`,
            saveUrl: `https://images.igdb.com/igdb/image/upload/t_cover_big_2x/${slug}.png`,
          }
        }).filter(Boolean));
  }

  export default {
    components: {
      Navbar,
      Checkbox,
      ArrowDown,
      ArrowRight,
      ArrowUp,
      ArrowUpDown,
      Check,
      Edit,
      FileText,
      Folder,
      FolderOpen,
      FolderUp,
      HardDrive,
      LayersPlus,
      Play,
      Plus,
      RotateCcw,
      Save,
      Search,
      Shield,
      Terminal,
      Trash2,
      X,
    },
    data() {
      return {
        apps: [],
        editForm: null,
        editFormError: "",
        detachedCmd: "",
        coverSearching: false,
        coverFinderBusy: false,
        coverCandidates: [],
        coverSearchQuery: "",
        platform: "",
        fileBrowserType: "any",
        fileBrowserTitle: "",
        fileBrowserCallback: null,
        fileBrowserAcceptedExtensions: null,
        fileBrowserCurrentPath: "",
        fileBrowserParentPath: "",
        fileBrowserEntries: [],
        fileBrowserLoading: false,
        fileBrowserError: "",
        fileBrowserSelectedPath: "",
        fileBrowserTypedPath: "",
        searchQuery: "",
        sortMode: "default",
        deleteTarget: null,
        version: null,
        githubVersion: null,
      };
    },
    computed: {
      installedVersionNotStable() {
        if (!this.githubVersion || !this.version) {
          return false;
        }
        return this.version.isGreater(this.githubVersion);
      },
      documentationBaseUrl() {
        const docsVersion = this.installedVersionNotStable ? 'master' : 'latest'
        return `https://docs.lizardbyte.dev/projects/sunshine/${docsVersion}`
      },
      displayedApps() {
        let list = this.apps.map((app, index) => ({ app, index }));

        const query = this.searchQuery.trim().toLowerCase();
        if (query) {
          list = list.filter(({ app }) =>
            (app.name || "").toLowerCase().includes(query)
          );
        }

        if (this.sortMode !== "default") {
          // Apps can be created without a name, so we compare name || ""
          list.sort((a, b) => {
            const result = (a.app.name || "").localeCompare(
              b.app.name || "", undefined, { sensitivity: "base" }
            );
            // localeCompare returns 0 if the strings are equal, a negative value if a < b, and a positive value if a > b
            return this.sortMode === "asc"
              ? result
              : -result;
          });
        }

        return list;
      },
      sortModeLabel() {
        switch (this.sortMode) {
          case "asc":
            return this.$t("apps.sort_ascending");
          case "desc":
            return this.$t("apps.sort_descending");
          default:
            return this.$t("apps.sort_default");
        }
      },
      appCountLabel() {
        const total = this.apps.length;
        const shown = this.displayedApps.length;
        return shown === total
          ? `${total}`
          : `${shown} / ${total}`;
      },
      editModalTitle() {
        if (!this.editForm) {
          return "";
        }
        const action = this.editForm.index === -1
          ? this.$t("apps.add_new")
          : this.$t("apps.edit");

        return this.editForm.name
          ? `${action}: ${this.editForm.name}`
          : action;
      },
    },
    created() {
      this.loadApps();

      fetch("./api/config")
        .then(r => r.json())
        .then(r => {
          this.platform = r.platform;
          this.version = new SunshineVersion(null, r.version);
        });

      fetch("https://api.github.com/repos/SunshineStream/Sunshine/releases/latest")
        .then((r) => r.json())
        .then((r) => this.githubVersion = new SunshineVersion(r, null))
        .catch((e) => console.error(e));
    },
    methods: {
      newApp() {
        this.editForm = {
          name: "",
          output: "",
          cmd: "",
          index: -1,
          "exclude-global-prep-cmd": false,
          elevated: false,
          "auto-detach": true,
          "wait-all": true,
          "exit-timeout": 5,
          "prep-cmd": [],
          detached: [],
          "working-dir": "",
          "image-path": ""
        };
        this.editFormError = "";
        this.openEditModal();
      },
      /**
       * @brief Open an application for editing with nullish optional fields defaulted.
       * @param {number} id Index of the application to edit.
       */
      editApp(id) {
        this.editForm = structuredClone(toRaw(this.apps[id]));
        this.editForm.index = id;
        const defaults = {
          "prep-cmd": [],
          detached: [],
          "exclude-global-prep-cmd": false,
          elevated: false,
          "auto-detach": true,
          "wait-all": true,
          "exit-timeout": 5,
          cmd: "",
          output: "",
          "working-dir": "",
          "image-path": ""
        };
        for (const [key, value] of Object.entries(defaults)) {
          this.editForm[key] ??= value;
        }
        this.editFormError = "";
        this.openEditModal();
      },
      showDeleteModal(id) {
        this.deleteTarget = { index: id, name: this.apps[id].name };
        this.$nextTick(() => {
          Modal.getOrCreateInstance(this.$refs.deleteModal).show();
        });
      },
      closeDeleteModal() {
        const modal = Modal.getInstance(this.$refs.deleteModal);
        if (modal) modal.hide();
      },
      loadApps() {
        return fetch("./api/apps")
          .then((r) => r.json())
          .then((r) => {
            this.apps = r.apps;
          });
      },
      confirmDelete() {
        apiFetch("./api/apps/" + this.deleteTarget.index, {
          method: "DELETE",
          headers: {
            "Content-Type": "application/json"
          },
        }).then((r) => {
          if (r.status === 200) document.location.reload();
        });
      },
      addPrepCmd() {
        let template = {
          do: "",
          undo: ""
        };

        if (this.platform === 'windows') {
          template = { ...template, elevated: false };
        }

        this.editForm["prep-cmd"].push(template);
      },
      deletePrepCmd(index) {
        this.editForm["prep-cmd"].splice(index, 1);
      },
      addDetached() {
        this.editForm.detached.push("");
      },
      showCoverFinder() {
        // Reset search state
        this.coverCandidates = [];
        this.coverSearchQuery = "";

        this.showStacked(this.$refs.coverFinderModal);

        // Perform initial search with app name
        this.performCoverSearch();
      },
      performCoverSearch() {
        this.coverSearching = true;
        this.coverCandidates = [];

        // Use search query if provided, otherwise fall back to app name
        const searchTerm = this.coverSearchQuery.trim() || this.editForm["name"].toString();

        searchCovers(searchTerm)
          .then(list => this.coverCandidates = list)
          .finally(() => this.coverSearching = false);
      },
      useCover(cover) {
        this.coverFinderBusy = true;
        apiFetch("./api/covers/upload", {
          method: "POST",
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify({
            key: cover.key,
            url: cover.saveUrl,
          })
        }).then(r => {
          if (!r.ok) throw new Error("Failed to download covers");
          return r.json();
        }).then(body => {
          this.editForm["image-path"] = body.path;
          // Close the modal
          const modalEl = this.$refs.coverFinderModal;
          if (modalEl) {
            const modal = Modal.getInstance(modalEl);
            if (modal) {
              modal.hide();
            }
          }
        })
          .finally(() => this.coverFinderBusy = false);
      },
      browseFor(type, titleKey, startPath, callback, acceptedExtensions = null) {
        this.fileBrowserType = type;
        this.fileBrowserTitle = this.$t(titleKey);
        this.fileBrowserCallback = callback;
        this.fileBrowserAcceptedExtensions = acceptedExtensions;
        this.fileBrowserSelectedPath = startPath || '';
        this.fileBrowserTypedPath = startPath || '';
        this.fileBrowserError = '';
        this.fileBrowserNavigate(startPath || '');
        this.showStacked(this.$refs.fileBrowserModal);
      },
      fileBrowserClose() {
        const modal = Modal.getInstance(this.$refs.fileBrowserModal);
        if (modal) modal.hide();
      },
      fileBrowserConfirm() {
        const path = this.fileBrowserSelectedPath || this.fileBrowserTypedPath;
        if (path) {
          if (this.fileBrowserAcceptedExtensions && this.fileBrowserType !== 'directory') {
            const lowerPath = path.toLowerCase();
            const isValid = this.fileBrowserAcceptedExtensions.some(ext => lowerPath.endsWith(ext.toLowerCase()));
            if (!isValid) {
              this.fileBrowserError = this.$t('file_browser.error_invalid_extension', { extensions: this.fileBrowserAcceptedExtensions.join(', ') });
              return;
            }
          }
          if (this.fileBrowserCallback) {
            this.fileBrowserCallback(path);
            this.fileBrowserCallback = null;
          }
          this.fileBrowserClose();
        }
      },
      /**
       * @brief Load a directory listing and display any browsing failure.
       *
       * @param {string} path Directory path, or an empty string for the default location.
       * @return {Promise<void>} Completion of the directory request and loading-state update.
       */
      async fileBrowserNavigate(path) {
        this.fileBrowserLoading = true;
        this.fileBrowserError = '';
        const params = new URLSearchParams({ type: this.fileBrowserType });
        if (path) params.set('path', path);
        try {
          const response = await fetch(`./api/browse?${params.toString()}`);
          const data = await response.json();
          if (!response.ok) {
            throw new Error(data.error || 'Browse failed');
          }
          this.fileBrowserCurrentPath = data.path ?? '';
          this.fileBrowserParentPath = data.parent ?? '';
          this.fileBrowserEntries = data.entries ?? [];
          this.fileBrowserTypedPath = data.path ?? '';
          this.fileBrowserSelectedPath = this.fileBrowserType === 'directory' ? (data.path ?? '') : '';
        } catch (err) {
          this.fileBrowserError = err.message;
        } finally {
          this.fileBrowserLoading = false;
        }
      },
      fileBrowserNavigateUp() {
        this.fileBrowserNavigate(this.fileBrowserParentPath);
      },
      fileBrowserSelectEntry(entry) {
        if (entry.type === 'directory') {
          this.fileBrowserNavigate(entry.path);
        } else {
          this.fileBrowserSelectedPath = entry.path;
          this.fileBrowserTypedPath = entry.path;
        }
      },
      fileBrowserActivateEntry(entry) {
        if (entry.type === 'directory') {
          this.fileBrowserNavigate(entry.path);
        } else {
          this.fileBrowserSelectedPath = entry.path;
          this.fileBrowserTypedPath = entry.path;
          this.fileBrowserConfirm();
        }
      },
      fileBrowserOnTypedInput() {
        this.fileBrowserSelectedPath = this.fileBrowserTypedPath;
      },
      browsePrep(index, field) {
        const current = this.editForm['prep-cmd'][index][field] || '';
        this.browseFor('executable', 'file_browser.select_executable', current, (path) => {
          this.editForm['prep-cmd'][index][field] = path;
        });
      },
      browseDetached(index) {
        const current = this.editForm.detached[index] || '';
        this.browseFor('executable', 'file_browser.select_executable', current, (path) => {
          this.editForm.detached[index] = path;
        });
      },
      save() {
        this.editFormError = "";
        const currentImagePath = this.editForm["image-path"] == null ? "" : String(this.editForm["image-path"]).trim();
        this.editForm["image-path"] = currentImagePath.replaceAll('"', '');

        const imagePath = this.editForm["image-path"];
        if (!imagePath) {
          delete this.editForm["image-path"];
        } else if (!imagePath.toLowerCase().endsWith('.png')) {
          this.editFormError = this.$t('file_browser.error_invalid_extension', { extensions: '.png' });
          const modalBody = this.$refs.editModal.querySelector('.modal-body');
          if (modalBody) modalBody.scrollTop = 0;
          return;
        }

        apiFetch("./api/apps", {
          method: "POST",
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify(this.editForm),
        }).then((r) => {
          if (r.status === 200) document.location.reload();
        });
      },
      handleImageError(event) {
        // Hide the broken image and show placeholder instead
        event.target.style.display = 'none';
        const placeholder = event.target.nextElementSibling;
        if (placeholder && placeholder.classList.contains('app-poster-placeholder')) {
          placeholder.style.display = 'flex';
        }
      },
      resetSearchQuery() {
        this.searchQuery = "";
      },
      toggleSort() {
        // Sorting goes default -> ascending -> descending -> default
        if (this.sortMode === "default")
          this.sortMode = "asc";
        else if (this.sortMode === "asc")
          this.sortMode = "desc";
        else
          this.sortMode = "default";
      },
      openEditModal() {
        // Use $nextTick because if the edit version of the modal is created too early, it breaks...
        this.$nextTick(() => {
          Modal.getOrCreateInstance(this.$refs.editModal).show();
        });
      },
      closeEditModal() {
        const modal = Modal.getInstance(this.$refs.editModal);
        if (modal) modal.hide();
      },
      // We need to update the z-index since bootstrap gives all modals the same z-index
      // Fixes the stacking issue of modals (cover finder / file browser)
      showStacked(modalEl) {
        modalEl.addEventListener('show.bs.modal', () => {
          const openCount = document.querySelectorAll('.modal.show').length;
          const z = 1055 + (openCount + 1) * 20;
          modalEl.style.zIndex = z;

          requestAnimationFrame(() => {
            const backdrops = document.querySelectorAll('.modal-backdrop');
            const backdrop = backdrops[backdrops.length - 1];
            if (backdrop) backdrop.style.zIndex = z - 10;
          });
        }, { once: true });
        Modal.getOrCreateInstance(modalEl).show();
      },
    },
  }
</script>
