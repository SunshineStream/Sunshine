import { readdirSync, readFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'

import { routes } from '../../src_assets/common/assets/web/router.js'

const webRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../../src_assets/common/assets/web')
const indexHtml = readFileSync(join(webRoot, 'index.html'), 'utf8')

/**
 * @brief Extract the classic inline script that resolves the base path from index.html.
 *
 * @return {string} Inline script source.
 */
function baseScriptSource() {
  const match = /<script>([\s\S]*?)<\/script>/.exec(indexHtml)
  expect(match).not.toBeNull()
  return match[1]
}

/**
 * @brief Run the index.html base path script for a URL path in an isolated document.
 *
 * @param {string} pathname Path the browser requested.
 * @return {string|null} The href attribute of the inserted base element.
 */
function resolveBase(pathname) {
  const doc = document.implementation.createHTMLDocument('Sunshine')
  new Function('location', 'document', baseScriptSource())({ pathname }, doc)
  return doc.querySelector('base')?.getAttribute('href') ?? null
}

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

describe('web UI base path', () => {
  it.each([
    ['/', '/'],
    ['/config', '/'],
    ['/logout/', '/'],
    ['/unknown', '/'],
    ['/apps/unknown', '/'],
    ['/sunshine/', '/sunshine/'],
    ['/sunshine/config', '/sunshine/'],
    ['/sunshine/troubleshooting/', '/sunshine/'],
    ['/sunshine/unknown', '/sunshine/'],
    ['/proxy/sunshine/pin', '/proxy/sunshine/'],
    ['/a/b/sunshine/', '/a/b/sunshine/'],
    ['/a/b/sunshine/troubleshooting/', '/a/b/sunshine/'],
    ['/a//b/sunshine/', '/a//b/sunshine/'],
    ['//', '/'],
    ['//example.com/', '/example.com/'],
    ['///example.com/', '/example.com/'],
    ['//example.com/config', '/example.com/'],
    ['//example.com/logout/', '/example.com/'],
    ['', '/'],
    ['config', '/'],
  ])('resolves %s to base %s', (pathname, expected) => {
    expect(resolveBase(pathname)).toBe(expected)
  })

  // The browser parses the requested URL before the script reads location.pathname, so feed each request path
  // through the WHATWG URL parser and check where the resulting base actually points.
  it.each([
    '/',
    '/sunshine/',
    '/a/b/sunshine/',
    '//example.com/',
    '///example.com/',
    '////example.com/sunshine/config',
    '//example.com/logout/',
    '/\\example.com/',
    '/\\\\example.com/',
    '/\t/example.com/',
    '/%2F%2Fexample.com/',
    '//example.com//pin',
    '/sunshine//config',
    '//',
  ])('keeps the base for request path %j on the document origin', requestPath => {
    const origin = 'https://sunshine.test:47990'
    const documentUrl = new URL(origin + requestPath)
    const base = resolveBase(documentUrl.pathname)

    expect(base).toMatch(/^\/(?!\/)/)
    const resolved = new URL(base, documentUrl)
    expect(resolved.origin).toBe(origin)
    expect(new URL('./assets/index.js', resolved).origin).toBe(origin)
    expect(new URL('./api/config', resolved).origin).toBe(origin)
  })

  it('knows every top-level route defined in the router', () => {
    const routeNames = routes
      .map(route => route.path.split('/')[1])
      .filter(name => name && !name.startsWith(':'))
    const script = baseScriptSource()
    const scriptRoutes = JSON.parse(/var routes = (\[[^\]]*\]);/.exec(script)[1].replaceAll("'", '"'))

    expect(scriptRoutes.toSorted()).toEqual(routeNames.toSorted())
  })

  it('does not hard-code a root base element', () => {
    expect(indexHtml).not.toMatch(/<base\s/)
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
})
