import { readdirSync, readFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { afterEach, describe, expect, it } from 'vitest'
import { createRouter, createWebHistory } from 'vue-router'

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../..')
const webRoot = join(repoRoot, 'src_assets/common/assets/web')
const indexHtml = readFileSync(join(webRoot, 'index.html'), 'utf8')
const rootBaseElement = '<base href="/" />'

/**
 * @brief List web UI source files that are bundled by Vite.
 *
 * @param {string} dir Directory to scan.
 * @return {string[]} Absolute paths of .js and .vue files outside public assets.
 */
function sourceFiles(dir) {
  return readdirSync(dir, { withFileTypes: true }).flatMap(entry => {
    const path = join(dir, entry.name)
    if (entry.isDirectory()) {
      return entry.name === 'public' ? [] : sourceFiles(path)
    }
    return /\.(js|vue)$/.test(entry.name) ? [path] : []
  })
}

/**
 * @brief Point the test document at a page URL with the base element the server would send.
 *
 * @param {string} baseHref The href of the base element.
 * @param {string} path Path of the page within the document origin.
 */
function loadPage(baseHref, path) {
  document.head.querySelectorAll('base').forEach(element => element.remove())
  const base = document.createElement('base')
  base.setAttribute('href', baseHref)
  document.head.prepend(base)
  globalThis.history.replaceState({}, '', path)
}

describe('web UI base path', () => {
  afterEach(() => {
    document.head.querySelectorAll('base').forEach(element => element.remove())
    globalThis.history.replaceState({}, '', '/')
  })

  it('ships one root base element for the server to replace', () => {
    expect(indexHtml.split(rootBaseElement)).toHaveLength(2)
    expect(indexHtml).not.toMatch(/<script>/)
  })

  it('uses the same base element marker as the server', () => {
    const server = readFileSync(join(repoRoot, 'src/confighttp.cpp'), 'utf8')
    const marker = /root_base_element = R"\((.*?)\)"sv/.exec(server)

    expect(marker?.[1]).toBe(rootBaseElement)
  })

  it('builds asset URLs relative to the document base', () => {
    const viteConfig = readFileSync(join(repoRoot, 'vite.config.js'), 'utf8')

    expect(viteConfig).toMatch(/base: '\.\/'/)
  })

  // Vite rewrites public asset URLs such as /images/... relative to the bundle, but not API request URLs.
  it('does not request the API from the domain root', () => {
    const rootRelative = /["'`]\/api\//
    const offenders = sourceFiles(webRoot)
      .flatMap(file => readFileSync(file, 'utf8')
        .split('\n')
        .filter(line => rootRelative.test(line))
        .map(line => `${file}: ${line.trim()}`))

    expect(offenders).toEqual([])
  })

  it.each([
    ['/', '/pin', '/pin'],
    ['/', '/apps/', '/apps/'],
    ['/', '/logout/', '/logout/'],
    ['/', '/unknown/deep/link', '/unknown/deep/link'],
    ['/sunshine/', '/sunshine/', '/'],
    ['/sunshine/', '/sunshine/troubleshooting', '/troubleshooting'],
    ['/apps/', '/apps/', '/'],
    ['/apps/', '/apps/config', '/config'],
    ['/proxy/config/', '/proxy/config/', '/'],
    ['/proxy/config/', '/proxy/config/apps', '/apps'],
    ['/a/b/sunshine/', '/a/b/sunshine/pin', '/pin'],
  ])('with base %s, routes %s as %s and keeps links, assets and API under the base', (baseHref, path, route) => {
    loadPage(baseHref, path)
    const history = createWebHistory()
    const router = createRouter({ history, routes: [{ path: '/:pathMatch(.*)*', component: {} }] })
    const origin = globalThis.location.origin

    expect(history.base).toBe(baseHref.replace(/\/$/, ''))
    expect(history.location.replace(/\/$/, '') || '/').toBe(route.replace(/\/$/, '') || '/')
    expect(router.resolve('/troubleshooting').href).toBe(`${baseHref}troubleshooting`)
    for (const relative of ['./assets/index.js', './api/config', './api/covers/0', './images/sunshine.ico', 'logout', './']) {
      const url = new URL(relative, document.baseURI)
      expect(url.origin).toBe(origin)
      expect(url.pathname.startsWith(baseHref)).toBe(true)
    }
    history.destroy()
  })
})
