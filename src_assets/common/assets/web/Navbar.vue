<template>
  <nav class="navbar navbar-light navbar-expand-lg navbar-background header">
    <div class="container-fluid">
      <a class="navbar-brand" href="/" title="Sunshine">
        <img src="/images/logo-sunshine-45.png" height="45" alt="Sunshine">
      </a>
      <button class="navbar-toggler" type="button" data-bs-toggle="collapse" data-bs-target="#navbarSupportedContent"
              aria-controls="navbarSupportedContent" aria-expanded="false" aria-label="Toggle navigation">
        <span class="navbar-toggler-icon"></span>
      </button>
      <div class="collapse navbar-collapse" id="navbarSupportedContent">
        <ul class="navbar-nav me-auto mb-2 mb-lg-0">
          <li class="nav-item">
            <a class="nav-link" href="/"><i class="fas fa-fw fa-home"></i> {{ $t('navbar.home') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="/pin"><i class="fas fa-fw fa-unlock"></i> {{ $t('navbar.pin') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="/apps"><i class="fas fa-fw fa-stream"></i> {{ $t('navbar.applications') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="/config"><i class="fas fa-fw fa-cog"></i> {{ $t('navbar.configuration') }}</a>
          </li>
          <li class="nav-item" v-if="isLoggedIn">
            <a class="nav-link" href="/password"><i class="fas fa-fw fa-user-shield"></i> {{ $t('navbar.password') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="/troubleshooting"><i class="fas fa-fw fa-info"></i> {{ $t('navbar.troubleshoot') }}</a>
          </li>
          <li class="nav-item">
            <ThemeToggle/>
          </li>
        </ul>
        <ul  class="navbar-nav mb-2 mb-lg-0" v-if="isLoggedIn">
          <li class="nav-item">
            <a class="nav-link" href="#" @click="logout"><i class="fas fa-fw fa-right-from-bracket"></i> Logout</a>
          </li>
        </ul>
      </div>
    </div>
  </nav>
    <!-- Modal that is shown when the user gets a 401 error -->
    <div class="modal fade" id="loginModal" tabindex="-1" v-if="isLoggedIn">
        <div class="modal-dialog">
            <div class="modal-content">
                <div class="modal-header">
                    <h1 class="modal-title fs-5" id="exampleModalLabel">Session Expired</h1>
                </div>
                <div class="modal-body">
                    <LoginForm @loggedin="onLogin" />
                </div>
            </div>
        </div>
    </div>
</template>

<script>
import ThemeToggle from './ThemeToggle.vue'
import { Modal } from 'bootstrap/dist/js/bootstrap'
import LoginForm from './LoginForm.vue'

export default {
  components: {
    ThemeToggle,
    LoginForm
  },
  props: {
    isLoggedIn: {
      type: Boolean,
      default: true
    }
  },
  data() {
    modal: null
  },
  created() {
    console.log("Header mounted!")
  },
  mounted() {
    if (!this.isLoggedIn) {
      return
    }
    this.setupDiscordWidget()

    this.setupUserSession()
  },
  methods: {
    setupUserSession() {
      // TODO: Migrate this to a Pinia state?
      window.addEventListener("sunshine:session_expire", () => {
        this.modal.toggle();
      })
      this.modal = new Modal(document.getElementById('loginModal'), {});
    },
    setupDiscordWidget() {
      let el = document.querySelector("a[href='" + document.location.pathname + "']");
      if (el) el.classList.add("active")
      let discordWidget = document.createElement('script')
      discordWidget.setAttribute('src', 'https://app.lizardbyte.dev/js/discord.js')
      document.head.appendChild(discordWidget)
    },
    onLogin() {
      this.modal.toggle();
    },
    logout() {
      fetch("/api/logout", { method: "POST" }).then(r => {
        document.location.href = '/';
      })
    }
  }
}
</script>

<style>
.navbar-background {
  background-color: #ffc400
}

.header .nav-link {
  color: rgba(0, 0, 0, .65) !important;
}

.header .nav-link.active {
  color: rgb(0, 0, 0) !important;
  font-weight: 500;
}

.header .nav-link:hover {
  color: rgb(0, 0, 0) !important;
  font-weight: 500;
}

.header .navbar-toggler {
  color: rgba(var(--bs-dark-rgb), .65) !important;
  border: var(--bs-border-width) solid rgba(var(--bs-dark-rgb), 0.15) !important;
}

.header .navbar-toggler-icon {
  --bs-navbar-toggler-icon-bg: url("data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 30 30'%3e%3cpath stroke='rgba%2833, 37, 41, 0.75%29' stroke-linecap='round' stroke-miterlimit='10' stroke-width='2' d='M4 7h22M4 15h22M4 23h22'/%3e%3c/svg%3e") !important;
}

.form-control::placeholder {
  opacity: 0.5;
}
</style>
