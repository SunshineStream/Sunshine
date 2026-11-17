<template>
  <Navbar></Navbar>
  <div id="content" class="container">
    <div class="my-4">
      <h1>{{ $t('config.configuration') }}</h1>
      <p>{{ $t('config.configuration_desc') }}</p>
    </div>

    <!-- Search Bar with Autocomplete -->
    <div class="toolbar mb-3 d-flex flex-wrap align-items-center gap-3">
      <div class="input-group config-search">
        <label for="config-search" class="visually-hidden">{{ $t('config.search_options') }}</label>
        <span class="input-group-text">
          <search :size="18" class="icon"></search>
        </span>
        <input
          id="config-search"
          type="text"
          class="form-control"
          v-model="searchQuery"
          :placeholder="$t('config.search_options')"
          @input="handleSearch"
          list="config-options"
        />
      </div>
      <datalist id="config-options">
        <option v-for="option in allConfigOptions" :key="option.key" :value="option.label">
          {{ option.tab }} - {{ option.label }}
        </option>
      </datalist>
      <span v-if="searchQuery && searchResults.length === 0" class="text-muted small flex-shrink-0">
        No results found for "{{ searchQuery }}"
      </span>
      <span v-else-if="searchQuery" class="text-muted small flex-shrink-0">
        Found {{ searchResults.length }} result(s)
      </span>
    </div>

    <div class="form" v-if="config">
      <div class="config-layout">
        <!-- Sidebar navigation -->
        <nav class="config-nav">
          <ul class="nav config-nav-list">
            <li class="nav-item" v-for="tab in generalTabs" :key="tab.id">
              <button type="button" class="nav-link" :class="{'active': tab.id === currentTab}"
                @click="currentTab = tab.id">
                <component :is="getTabIcon(tab.id)" :size="18" class="icon"></component>
                {{ $t(tab.nameKey) }}
              </button>
            </li>
          </ul>
          <template v-if="encoderTabs.length">
            <div class="config-nav-heading">{{ $t('config.encoders') }}</div>
            <ul class="nav config-nav-list">
              <li class="nav-item" v-for="tab in encoderTabs" :key="tab.id">
                <button type="button" class="nav-link" :class="{'active': tab.id === currentTab}"
                  @click="currentTab = tab.id">
                  <component :is="getTabIcon(tab.id)" :size="18" class="icon"></component>
                  {{ $t(tab.nameKey) }}
                </button>
              </li>
            </ul>
          </template>

          <div class="config-actions">
            <button class="btn btn-primary" @click="save">
              <save :size="18" class="icon"></save>
              {{ $t('_common.save') }}
            </button>
            <button class="btn btn-success" @click="apply" v-if="saved && !restarted">
              <check :size="18" class="icon"></check>
              {{ $t('_common.apply') }}
            </button>
          </div>
        </nav>

        <!-- Tab content -->
        <div class="config-content">
          <div class="alert alert-success mb-4" v-if="saved && !restarted">
            <b>{{ $t('_common.success') }}</b> {{ $t('config.apply_note') }}
          </div>
          <div class="alert alert-success mb-4" v-if="restarted">
            <b>{{ $t('_common.success') }}</b> {{ $t('config.restart_note') }}
          </div>
          <div class="alert alert-danger mb-4" v-if="saveBlocked === 'audio_capture'">
            <b>{{ $t('_common.error') }}</b> {{ $t('config.audio_capture_save_blocked') }}
          </div>

      <!-- General Tab -->
      <general
        v-if="currentTab === 'general'"
        :config="config"
        :platform="platform">
      </general>

      <!-- Input Tab -->
      <inputs
        v-if="currentTab === 'input'"
        :config="config"
        :platform="platform">
      </inputs>

      <!-- Audio/Video Tab -->
      <audio-video
        v-if="currentTab === 'av'"
        ref="audioVideo"
        :config="config"
        :platform="platform"
      >
      </audio-video>

      <!-- Network Tab -->
      <network
        v-if="currentTab === 'network'"
        :config="config"
        :platform="platform">
      </network>

      <!-- Files Tab -->
      <files
        v-if="currentTab === 'files'"
        :config="config"
        :platform="platform">
      </files>

      <!-- Advanced Tab -->
      <advanced
        v-if="currentTab === 'advanced'"
        :config="config"
        :platform="platform">
      </advanced>

      <container-encoders
        :current-tab="currentTab"
        :config="config"
        :platform="platform">
      </container-encoders>
        </div>
      </div>
    </div>

  </div>
</template>

<script>
  import { computed, toRaw } from 'vue'
  import Navbar from './Navbar.vue'
  import { apiFetch } from './fetch_utils'
  import { isConfigBoolEnabled } from './config_bool'
  import configTabs from './configs/config_tabs.json'
  import General from './configs/tabs/General.vue'
  import Inputs from './configs/tabs/Inputs.vue'
  import Network from './configs/tabs/Network.vue'
  import Files from './configs/tabs/Files.vue'
  import Advanced from './configs/tabs/Advanced.vue'
  import AudioVideo from './configs/tabs/AudioVideo.vue'
  import ContainerEncoders from './configs/tabs/ContainerEncoders.vue'
  import {
    Check,
    Cpu,
    FileCog,
    Gamepad2,
    Gpu,
    Network as NetworkIcon,
    Save,
    Search,
    Settings,
    Sliders,
    Volume2,
  } from '@lucide/vue'

  const ENCODER_TAB_IDS = new Set(["nv", "amd", "qsv", "vaapi", "vt", "vulkan", "sw"]);

  /**
   * Compare configuration values without coercing their types.
   *
   * @param {*} value Configured value.
   * @param {*} defaultValue Default value for the option.
   * @returns {boolean} Whether both values have the same type and contents.
   */
  function configValuesEqual(value, defaultValue) {
    if (Object.is(value, defaultValue)) {
      return true;
    }
    if (typeof value !== typeof defaultValue || value === null || defaultValue === null || typeof value !== 'object') {
      return false;
    }
    if (Array.isArray(value) !== Array.isArray(defaultValue)) {
      return false;
    }

    const valueKeys = Object.keys(value);
    const defaultKeys = Object.keys(defaultValue);
    return valueKeys.length === defaultKeys.length && valueKeys.every(key =>
      Object.hasOwn(defaultValue, key) && configValuesEqual(value[key], defaultValue[key])
    );
  }

  /**
   * Migrate the legacy external_audio option into keep_default_sink.
   *
   * An explicit keep_default_sink value wins; the legacy key is always dropped so
   * the next save persists only the unified key. The legacy key only ever took
   * effect on Windows with an explicit audio_sink, so the migration keeps that
   * exact scope instead of changing routing elsewhere.
   *
   * @param {Object} config Configuration object to migrate in place.
   * @param {string} platform Platform reported by the server.
   */
  function migrateExternalAudioConfig(config, platform) {
    if (!Object.hasOwn(config, 'external_audio')) {
      return;
    }
    const keepDefaultSink = config.keep_default_sink;
    const audioSink = config.audio_sink;
    if (
      platform === 'windows' &&
      typeof audioSink === 'string' &&
      audioSink !== '' &&
      (keepDefaultSink === undefined || keepDefaultSink === '') &&
      isConfigBoolEnabled(config.external_audio)
    ) {
      config.keep_default_sink = 'enabled';
    }
    delete config.external_audio;
  }

  export default {
    components: {
      Navbar,
      General,
      Inputs,
      Network,
      Files,
      Advanced,
      // They will be accessible via audio-video, container-encoders only.
      AudioVideo,
      ContainerEncoders,
      // icons
      Cpu,
      Check,
      FileCog,
      Gamepad2,
      Gpu,
      NetworkIcon,
      Save,
      Search,
      Settings,
      Sliders,
      Volume2,
    },
    data() {
      return {
        platform: "",
        saved: false,
        restarted: false,
        saveBlocked: null,
        config: null,
        currentTab: "general",
        searchQuery: "",
        hashChangeHandler: null,
        // Keep a private copy because platform filtering replaces this array at runtime.
        tabs: structuredClone(configTabs),
      };
    },
    provide() {
       return {
         platform: computed(() => this.platform),
         searchQuery: computed(() => this.searchQuery),
       }
    },
    computed: {
      generalTabs() {
        return this.tabs.filter(tab => !ENCODER_TAB_IDS.has(tab.id));
      },
      encoderTabs() {
        return this.tabs.filter(tab => ENCODER_TAB_IDS.has(tab.id));
      },
      allConfigOptions() {
        const options = [];
        this.tabs.forEach(tab => {
          Object.keys(tab.options).forEach(key => {
            options.push({
              key: key,
              label: key.replaceAll('_', ' ').replaceAll(/\b\w/g, l => l.toUpperCase()),
              tab: this.$t(tab.nameKey),
              tabId: tab.id
            });
          });
        });
        return options;
      },
      searchResults() {
        if (!this.searchQuery) return [];
        const query = this.searchQuery.toLowerCase();
        return this.allConfigOptions.filter(option =>
          option.key.toLowerCase().includes(query) ||
          option.label.toLowerCase().includes(query)
        );
      }
    },
    created() {
      fetch("./api/config")
        .then((r) => r.json())
        .then((r) => {
          this.config = r;
          this.platform = this.config.platform;

          if (this.platform === "windows") {
            this.tabs = this.tabs.filter((el) => {
              return el.id !== "vt" && el.id !== "vaapi" && el.id !== "vulkan";
            });
          }
          if (this.platform === "freebsd" || this.platform === "linux") {
            this.tabs = this.tabs.filter((el) => {
              return el.id !== "amd" && el.id !== "qsv" && el.id !== "vt";
            });
          }
          if (this.platform === "macos") {
            this.tabs = this.tabs.filter((el) => {
              return el.id !== "amd" && el.id !== "nv" && el.id !== "qsv" && el.id !== "vaapi" && el.id !== "vulkan";
            });
          }

          // remove values we don't want in the config file
          delete this.config.platform;
          delete this.config.status;
          delete this.config.version;

          migrateExternalAudioConfig(this.config, this.platform);

          // Parse the special options before population if available
          const specialOptions = ["dd_mode_remapping", "global_prep_cmd"]
          for (const optionKey of specialOptions) {
            if (this.config.hasOwnProperty(optionKey)) {
              this.config[optionKey] = JSON.parse(this.config[optionKey]);
            }
          }

          // Populate default values from tabs options
          this.tabs.forEach(tab => {
            Object.keys(tab.options).forEach(optionKey => {
              if (this.config[optionKey] === undefined) {
                // Make sure to copy by value
                this.config[optionKey] = structuredClone(toRaw(tab.options[optionKey]));
              }
            });
          });
        });
    },
    methods: {
      getTabIcon(tabId) {
        const iconMap = {
          'general': 'Settings',
          'input': 'Gamepad2',
          'av': 'Volume2',
          'network': 'NetworkIcon',
          'files': 'FileCog',
          'advanced': 'Sliders',
          'nv': 'Gpu',
          'amd': 'Gpu',
          'qsv': 'Gpu',
          'vaapi': 'Gpu',
          'vt': 'Gpu',
          'vulkan': 'Gpu',
          'sw': 'Cpu',
        };
        return iconMap[tabId] || 'Settings';
      },
      forceUpdate() {
        this.$forceUpdate()
      },
      serialize() {
        return structuredClone(toRaw(this.config));
      },
      save() {
        this.saved = false;
        this.restarted = false;
        this.saveBlocked = null;

        // Let the audio/video tab reject selections that would persist an
        // unusable capture target. The tab may be unmounted (another tab is
        // open), in which case there is nothing new to validate.
        const audioTab = this.$refs.audioVideo;
        const audioValid = audioTab?.validateAudioCapture?.() ?? true;
        if (!audioValid) {
          this.currentTab = 'av';
          this.saveBlocked = 'audio_capture';
          this.$nextTick(() => {
            const sink = document.getElementById('audio_sink');
            if (sink) {
              sink.focus();
            }
          });
          return Promise.resolve(false);
        }

        // create a temp copy of this.config to use for the post request
        let config = this.serialize();

        // delete default values from this.config
        this.tabs.forEach(tab => {
          Object.keys(tab.options).forEach(optionKey => {
            if (configValuesEqual(config[optionKey], tab.options[optionKey])) {
              delete config[optionKey]
            }
          });
        });

        return apiFetch("./api/config", {
          method: "POST",
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify(config),
        }).then((r) => {
          if (r.status === 200) {
            this.saved = true
            return this.saved
          }
          else {
            return false
          }
        });
      },
      apply() {
        this.saved = this.restarted = false;
        let saved = this.save();

        saved.then((result) => {
          if (result === true) {
            this.restarted = true;
            setTimeout(() => {
              this.saved = this.restarted = false;
            }, 5000);
            apiFetch("./api/restart", {
              method: "POST",
              headers: {
                "Content-Type": "application/json"
              }
            });
          }
        });
      },
      handleSearch() {
        // Clear all highlighting
        document.querySelectorAll('.config-search-highlight').forEach(el => {
          el.classList.remove('config-search-highlight');
        });

        if (!this.searchQuery) {
          // Show all form groups when search is cleared
          document.querySelectorAll('.mb-3').forEach(el => {
            el.style.display = '';
            delete el.dataset.searchHidden;
          });
          return;
        }

        const results = this.searchResults;

        if (results.length === 0) {
          return;
        }

        // Switch to the tab of the first result
        if (results.length > 0 && results[0].tabId !== this.currentTab) {
          this.currentTab = results[0].tabId;
        }

        // Wait for tab content to render
        this.$nextTick(() => {
          // Hide all form groups first, tagging what this search hid so the
          // reveal step only undoes its own hiding.
          document.querySelectorAll('.config-page .mb-3').forEach(el => {
            el.style.display = 'none';
            el.dataset.searchHidden = '1';
          });

          // Show each match plus every hidden ancestor up to the page root,
          // so matches nested inside grouping fieldsets become visible.
          results.forEach(result => {
            const element = document.getElementById(result.key);

            if (element) {
              this.revealSearchMatch(element);
            }
          });

          // Scroll to and highlight the first result
          if (results.length > 0) {
            const firstElement = document.getElementById(results[0].key);
            if (firstElement) {
              const container = firstElement.closest('.mb-3');
              if (container) {
                container.scrollIntoView({ behavior: 'smooth', block: 'center' });
                container.classList.add('config-search-highlight');
                setTimeout(() => {
                  container.classList.remove('config-search-highlight');
                }, 3000);
              }
            }
          }
        });
      },
      revealSearchMatch(element) {
        // Climb from the match to the page root, unhiding every ancestor
        // this search hid. Stops at .config-page so other tabs stay hidden.
        const page = element.closest('.config-page');
        let node = element.closest('.mb-3');
        while (node && node !== page) {
          if (node.dataset.searchHidden === '1') {
            node.style.display = '';
            delete node.dataset.searchHidden;
          }
          node = node.parentElement ? node.parentElement.closest('.mb-3') : null;
        }
      },
    },
    mounted() {
      // Handle hashchange events
      this.hashChangeHandler = () => {
        let hash = window.location.hash;
        if (hash) {
          // remove the # from the hash
          let stripped_hash = hash.substring(1);

          this.tabs.forEach(tab => {
            Object.keys(tab.options).forEach(key => {
              if (tab.id === stripped_hash || key === stripped_hash) {
                this.currentTab = tab.id;
              }
              if (key === stripped_hash) {
                // sleep for 2 seconds to allow the page to load
                setTimeout(() => {
                  let element = document.getElementById(stripped_hash);
                  if (element) {
                    window.location.hash = hash;
                  }
                }, 2000);
              }

              if (this.currentTab === tab.id) {
                // stop looping
                return true;
              }
            });
          });
        }
      };

      // Call handleHash for the initial load
      this.hashChangeHandler();

      // Add hashchange event listener
      window.addEventListener("hashchange", this.hashChangeHandler);
    },
    beforeUnmount() {
      window.removeEventListener("hashchange", this.hashChangeHandler);
    },
  }
</script>
