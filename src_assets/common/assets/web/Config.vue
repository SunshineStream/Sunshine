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
          // Hide all form groups first
          document.querySelectorAll('.config-page .mb-3').forEach(el => {
            el.style.display = 'none';
          });

          // Show only matching elements
          results.forEach(result => {
            const element = document.getElementById(result.key);

            if (element) {
              // Show the element's container
              const container = element.closest('.mb-3');
              if (container) {
                container.style.display = '';
              }
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
