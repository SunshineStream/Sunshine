import { createRouter, createWebHistory } from 'vue-router'

/**
 * Web UI routes rendered from the single Vite entry page.
 *
 * Lazy page imports keep the source split into focused components while the
 * server serves the same index document for every browser route.
 */
const routes = [
  { path: '/', component: () => import('./Home.vue') },
  { path: '/apps', component: () => import('./Apps.vue') },
  { path: '/clients', redirect: '/' },
  { path: '/config', component: () => import('./Config.vue') },
  { path: '/featured', component: () => import('./Featured.vue') },
  { path: '/logout', component: () => import('./Logout.vue') },
  { path: '/password', component: () => import('./Password.vue') },
  { path: '/pin', component: () => import('./Pin.vue') },
  { path: '/troubleshooting', component: () => import('./Troubleshooting.vue') },
  { path: '/welcome', component: () => import('./Welcome.vue') },
  { path: '/:pathMatch(.*)*', redirect: '/' },
]

const router = createRouter({
  history: createWebHistory(),
  routes,
  scrollBehavior(to) {
    return to.hash ? { el: to.hash } : { top: 0 }
  },
})

export default router
