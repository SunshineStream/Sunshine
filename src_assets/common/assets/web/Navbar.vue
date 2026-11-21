<template>
  <div>
    <nav class="navbar navbar-expand-lg navbar-sunshine">
      <div class="container-fluid">
        <RouterLink class="navbar-brand" to="/" title="Sunshine">
          <img src="/images/logo-sunshine-45.png" height="45" alt="Sunshine">
        </RouterLink>
        <button class="navbar-toggler" type="button" data-bs-toggle="collapse" data-bs-target="#navbarSupportedContent"
                aria-controls="navbarSupportedContent" aria-expanded="false" aria-label="Toggle navigation">
          <span class="navbar-toggler-icon"></span>
        </button>
        <div class="collapse navbar-collapse" id="navbarSupportedContent">
          <ul class="navbar-nav me-auto mb-2 mb-lg-0">
            <li class="nav-item">
              <RouterLink class="nav-link" to="/">
                <Home :size="18" class="icon"></Home>
                {{ $t('navbar.home') }}
              </RouterLink>
            </li>
            <li class="nav-item">
              <RouterLink class="nav-link" to="/pin">
                <Lock :size="18" class="icon"></Lock>
                {{ $t('navbar.pin') }}
              </RouterLink>
            </li>
            <li class="nav-item">
              <RouterLink class="nav-link" to="/apps">
                <Layers :size="18" class="icon"></Layers>
                {{ $t('navbar.applications') }}
              </RouterLink>
            </li>
            <li class="nav-item">
              <RouterLink class="nav-link" to="/featured">
                <Star :size="18" class="icon"></Star>
                {{ $t('navbar.featured') }}
              </RouterLink>
            </li>
            <li class="nav-item">
              <RouterLink class="nav-link" to="/config">
                <Settings :size="18" class="icon"></Settings>
                {{ $t('navbar.configuration') }}
              </RouterLink>
            </li>
            <li class="nav-item">
              <RouterLink class="nav-link" to="/troubleshooting">
                <Info :size="18" class="icon"></Info>
                {{ $t('navbar.troubleshoot') }}
              </RouterLink>
            </li>
          </ul>
          <ul class="navbar-nav ms-auto mb-2 mb-lg-0">
            <li class="nav-item">
              <ThemeToggle/>
            </li>
            <li class="nav-item dropdown">
              <button class="nav-link dropdown-toggle" type="button" id="navbarUserMenu"
                      data-bs-toggle="dropdown" aria-expanded="false" aria-label="User menu" title="User menu">
                <CircleUserRound :size="18" class="icon"></CircleUserRound>
              </button>
              <ul class="dropdown-menu dropdown-menu-end" aria-labelledby="navbarUserMenu">
                <li>
                  <RouterLink class="dropdown-item d-flex align-items-center" to="/password">
                    <Shield :size="18" class="icon"></Shield>
                    {{ $t('navbar.password') }}
                  </RouterLink>
                </li>
                <li><hr class="dropdown-divider"></li>
                <li>
                  <button type="button" class="dropdown-item d-flex align-items-center" @click="logout">
                    <LogOut :size="18" class="icon"></LogOut>
                    {{ $t('navbar.logout') }}
                  </button>
                </li>
              </ul>
            </li>
          </ul>
        </div>
      </div>
    </nav>
    <Notification></Notification>
  </div>
</template>

<script>
import { CircleUserRound, Home, Info, Layers, Lock, LogOut, Settings, Shield, Star } from '@lucide/vue'
import ThemeToggle from './ThemeToggle.vue'
import Notification from './Notification.vue'

export default {
  components: {
    ThemeToggle,
    Notification,
    Home,
    Lock,
    Layers,
    Star,
    Settings,
    Shield,
    Info,
    CircleUserRound,
    LogOut
  },
  methods: {
    logout() {
      const cacheBuster = Date.now().toString()
      const logoutPageUrl = new URL('logout', document.baseURI)
      const request = new XMLHttpRequest()
      const finish = () => {
        globalThis.location.replace(logoutPageUrl.toString())
      }

      request.open('GET', './', true, 'sunshine-logout', cacheBuster)
      request.setRequestHeader('Cache-Control', 'no-store')
      request.onload = finish
      request.onerror = finish
      request.ontimeout = finish
      request.timeout = 5000
      request.send()
    }
  }
}
</script>

<style>
/** @brief Keep the Sunshine toggler icon above Bootstrap's light and dark variants. */
.navbar-sunshine .navbar-toggler .navbar-toggler-icon {
  --bs-navbar-toggler-icon-bg: url("data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 30 30'%3e%3cpath stroke='rgba%28255, 255, 255, 0.9%29' stroke-linecap='round' stroke-miterlimit='10' stroke-width='2' d='M4 7h22M4 15h22M4 23h22'/%3e%3c/svg%3e");
}
</style>
