import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))
vi.mock('../../src_assets/common/assets/web/fetch_utils', () => ({
  apiFetch: vi.fn(() => Promise.resolve({ status: 200 })),
}))

import { apiFetch } from '../../src_assets/common/assets/web/fetch_utils'
import Apps from '../../src_assets/common/assets/web/Apps.vue'

beforeEach(() => vi.clearAllMocks())
afterEach(() => vi.unstubAllGlobals())

/**
 * Build a minimal context object that satisfies
 * the `this` contract of fileBrowserConfirm().
 *
 * @param {string} path Selected file path.
 * @param {string[]|null} acceptedExtensions Allowed extensions or null.
 * @param {string} type Browser type (e.g. 'file', 'directory').
 * @returns {object} A stub context for `.call()`.
 */
function browserContext(path, acceptedExtensions, type = 'file') {
  return {
    fileBrowserSelectedPath: path,
    fileBrowserTypedPath: '',
    fileBrowserAcceptedExtensions: acceptedExtensions,
    fileBrowserType: type,
    fileBrowserError: '',
    fileBrowserCallback: vi.fn(),
    fileBrowserClose: vi.fn(),
    $t: (key, _params) => key,
  }
}

/**
 * Build a minimal context object that satisfies
 * the `this` contract of save().
 *
 * @param {string} imagePath The image-path value in the edit form.
 * @returns {object} A stub context for `.call()`.
 */
function saveContext(imagePath) {
  const modalBody = { scrollTop: 100 }
  return {
    editForm: { 'image-path': imagePath },
    editFormError: '',
    $refs: { editModal: { querySelector: () => modalBody } },
    $t: (key, _params) => key,
    _modalBody: modalBody,
  }
}

describe('fileBrowserConfirm – extension validation', () => {
  it('rejects a non-PNG file when acceptedExtensions is [".png"]', () => {
    const ctx = browserContext('/covers/cover.jpg', ['.png'])
    const onSelect = ctx.fileBrowserCallback

    Apps.methods.fileBrowserConfirm.call(ctx)

    expect(ctx.fileBrowserError).toBe('file_browser.error_invalid_extension')
    expect(onSelect).not.toHaveBeenCalled()
    expect(ctx.fileBrowserClose).not.toHaveBeenCalled()
  })

  it('accepts a PNG with mixed-case extension', () => {
    const ctx = browserContext('/covers/cover.PNG', ['.png'])
    const onSelect = ctx.fileBrowserCallback

    Apps.methods.fileBrowserConfirm.call(ctx)

    expect(ctx.fileBrowserError).toBe('')
    expect(onSelect).toHaveBeenCalledWith('/covers/cover.PNG')
    expect(ctx.fileBrowserClose).toHaveBeenCalledOnce()
  })

  it('allows any extension when acceptedExtensions is null', () => {
    const ctx = browserContext('/output/log.txt', null)
    const onSelect = ctx.fileBrowserCallback

    Apps.methods.fileBrowserConfirm.call(ctx)

    expect(ctx.fileBrowserError).toBe('')
    expect(onSelect).toHaveBeenCalledWith('/output/log.txt')
    expect(ctx.fileBrowserClose).toHaveBeenCalledOnce()
  })

  it('skips extension check for directory type even with acceptedExtensions', () => {
    const ctx = browserContext('/some/directory', ['.png'], 'directory')
    const onSelect = ctx.fileBrowserCallback

    Apps.methods.fileBrowserConfirm.call(ctx)

    expect(ctx.fileBrowserError).toBe('')
    expect(onSelect).toHaveBeenCalledWith('/some/directory')
    expect(ctx.fileBrowserClose).toHaveBeenCalledOnce()
  })

  it('does nothing when no path is selected', () => {
    const ctx = browserContext('', ['.png'])
    const onSelect = ctx.fileBrowserCallback

    Apps.methods.fileBrowserConfirm.call(ctx)

    expect(ctx.fileBrowserError).toBe('')
    expect(onSelect).not.toHaveBeenCalled()
    expect(ctx.fileBrowserClose).not.toHaveBeenCalled()
  })

  it('uses fileBrowserTypedPath as fallback when selectedPath is empty', () => {
    const ctx = browserContext('', ['.png'])
    ctx.fileBrowserTypedPath = '/covers/art.png'
    const onSelect = ctx.fileBrowserCallback

    Apps.methods.fileBrowserConfirm.call(ctx)

    expect(onSelect).toHaveBeenCalledWith('/covers/art.png')
    expect(ctx.fileBrowserClose).toHaveBeenCalledOnce()
  })
})

describe('save – cover image validation', () => {
  it('blocks a manually typed non-PNG image path', () => {
    const ctx = saveContext('/covers/cover.jpg')

    Apps.methods.save.call(ctx)

    expect(ctx.editFormError).toBe('file_browser.error_invalid_extension')
    expect(ctx._modalBody.scrollTop).toBe(0)
    expect(apiFetch).not.toHaveBeenCalled()
  })

  it('blocks a .bmp image path', () => {
    const ctx = saveContext('/covers/image.bmp')

    Apps.methods.save.call(ctx)

    expect(ctx.editFormError).toBe('file_browser.error_invalid_extension')
    expect(apiFetch).not.toHaveBeenCalled()
  })

  it('accepts a valid .PNG path (mixed case) and submits', () => {
    const ctx = saveContext('/covers/cover.PnG')

    Apps.methods.save.call(ctx)

    expect(ctx.editFormError).toBe('')
    expect(apiFetch).toHaveBeenCalledOnce()
    expect(apiFetch).toHaveBeenCalledWith(
      './api/apps',
      expect.objectContaining({ method: 'POST' })
    )
  })

  it('submits when the image path is empty and leaves the letter cover as the default', () => {
    const ctx = saveContext('')

    Apps.methods.save.call(ctx)

    expect(ctx.editFormError).toBe('')
    expect(ctx.editForm['image-path']).toBeUndefined()
    expect(apiFetch).toHaveBeenCalledOnce()
  })

  it('submits when the image path is missing', () => {
    const ctx = saveContext(null)

    expect(() => Apps.methods.save.call(ctx)).not.toThrow()

    expect(ctx.editFormError).toBe('')
    expect(ctx.editForm['image-path']).toBeUndefined()
    expect(apiFetch).toHaveBeenCalledOnce()
  })

  it('strips double quotes from the image path before validating', () => {
    const ctx = saveContext('""/covers/cover.png""')

    Apps.methods.save.call(ctx)

    expect(ctx.editForm['image-path']).toBe('/covers/cover.png')
    expect(ctx.editFormError).toBe('')
    expect(apiFetch).toHaveBeenCalledOnce()
  })
})

/**
 * @brief Create file-browser state with an existing selection for navigation tests.
 *
 * @param {string} type Browser selection type.
 * @return {object} Context for fileBrowserNavigate().
 */
function navigationContext(type = 'file') {
  return {
    fileBrowserType: type,
    fileBrowserLoading: false,
    fileBrowserError: 'Previous error',
    fileBrowserCurrentPath: '/old',
    fileBrowserParentPath: '/',
    fileBrowserEntries: [{ name: 'old.png', path: '/old/old.png', type: 'file' }],
    fileBrowserTypedPath: '/old/old.png',
    fileBrowserSelectedPath: '/old/old.png',
  }
}

describe('fileBrowserNavigate', () => {
  it.each(['file', 'directory'])('loads a listing and updates the %s selection', async type => {
    const ctx = navigationContext(type)
    const entries = [{ name: 'cover.png', path: '/covers/cover.png', type: 'file' }]
    const response = Promise.withResolvers()
    vi.stubGlobal('fetch', vi.fn(() => response.promise))

    const navigation = Apps.methods.fileBrowserNavigate.call(ctx, '/covers')
    expect(ctx.fileBrowserLoading).toBe(true)
    expect(ctx.fileBrowserError).toBe('')
    expect(fetch).toHaveBeenCalledExactlyOnceWith(`./api/browse?type=${type}&path=%2Fcovers`)

    response.resolve({ ok: true, json: async () => ({ path: '/covers', parent: '/', entries }) })
    await navigation

    expect(ctx.fileBrowserCurrentPath).toBe('/covers')
    expect(ctx.fileBrowserParentPath).toBe('/')
    expect(ctx.fileBrowserEntries).toEqual(entries)
    expect(ctx.fileBrowserTypedPath).toBe('/covers')
    expect(ctx.fileBrowserSelectedPath).toBe(type === 'directory' ? '/covers' : '')
    expect(ctx.fileBrowserError).toBe('')
    expect(ctx.fileBrowserLoading).toBe(false)
  })

  it.each(['file', 'directory'])('uses defaults for missing fields in a %s listing', async type => {
    const ctx = navigationContext(type)
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: true, json: async () => ({}) }))

    await Apps.methods.fileBrowserNavigate.call(ctx, '')

    expect(fetch).toHaveBeenCalledExactlyOnceWith(`./api/browse?type=${type}`)
    expect(ctx.fileBrowserCurrentPath).toBe('')
    expect(ctx.fileBrowserParentPath).toBe('')
    expect(ctx.fileBrowserEntries).toEqual([])
    expect(ctx.fileBrowserTypedPath).toBe('')
    expect(ctx.fileBrowserSelectedPath).toBe('')
    expect(ctx.fileBrowserLoading).toBe(false)
  })

  it.each([
    [{ error: 'Permission denied' }, 'Permission denied'],
    [{}, 'Browse failed'],
  ])('displays HTTP errors while retaining the previous listing', async (body, message) => {
    const ctx = navigationContext()
    const previousEntries = ctx.fileBrowserEntries
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: false, json: async () => body }))

    await Apps.methods.fileBrowserNavigate.call(ctx, '/restricted')

    expect(ctx.fileBrowserError).toBe(message)
    expect(ctx.fileBrowserLoading).toBe(false)
    expect(ctx.fileBrowserCurrentPath).toBe('/old')
    expect(ctx.fileBrowserEntries).toBe(previousEntries)
    expect(ctx.fileBrowserTypedPath).toBe('/old/old.png')
    expect(ctx.fileBrowserSelectedPath).toBe('/old/old.png')
  })

  it('displays network failures and clears the loading state', async () => {
    const ctx = navigationContext()
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new Error('Network unavailable')))

    await Apps.methods.fileBrowserNavigate.call(ctx, '/covers')

    expect(ctx.fileBrowserError).toBe('Network unavailable')
    expect(ctx.fileBrowserLoading).toBe(false)
    expect(ctx.fileBrowserCurrentPath).toBe('/old')
  })

  it.each([true, false])('displays invalid JSON errors when response.ok is %s', async ok => {
    const ctx = navigationContext()
    const json = vi.fn().mockRejectedValue(new Error('Invalid JSON'))
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok, json }))

    await Apps.methods.fileBrowserNavigate.call(ctx, '/covers')

    expect(ctx.fileBrowserError).toBe('Invalid JSON')
    expect(ctx.fileBrowserLoading).toBe(false)
    expect(ctx.fileBrowserCurrentPath).toBe('/old')
  })
})
