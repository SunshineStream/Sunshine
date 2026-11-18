import { beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))
vi.mock('../../src_assets/common/assets/web/fetch_utils', () => ({
  apiFetch: vi.fn(() => Promise.resolve({ status: 204 })),
}))

import { apiFetch } from '../../src_assets/common/assets/web/fetch_utils'
import Apps from '../../src_assets/common/assets/web/Apps.vue'

beforeEach(() => vi.clearAllMocks())

/**
 * @brief Construct editing state without invoking Bootstrap or the network.
 * @param {object} app Application JSON to edit.
 * @return {object} Context for editApp() and save().
 */
function editContext(app) {
  return {
    apps: [app],
    platform: 'windows',
    openEditModal: vi.fn(),
    $refs: { editModal: { querySelector: () => ({ scrollTop: 0 }) } },
    $t: key => key,
  }
}

describe('optional application fields', () => {
  const defaults = {
    'prep-cmd': [],
    detached: [],
    'exclude-global-prep-cmd': false,
    elevated: false,
    'auto-detach': true,
    'wait-all': true,
    'exit-timeout': 5,
    cmd: '',
    output: '',
    'working-dir': '',
    'image-path': '',
  }

  it.each([undefined, null])('defaults optional fields containing %s before editing and saving', value => {
    const app = { name: 'No cover' }
    if (value === null) {
      for (const key of Object.keys(defaults)) {
        app[key] = null
      }
    }
    const ctx = editContext(app)

    Apps.methods.editApp.call(ctx, 0)

    expect(ctx.editForm).toEqual({ name: 'No cover', index: 0, ...defaults })
    expect(() => Apps.methods.addPrepCmd.call(ctx)).not.toThrow()
    expect(() => Apps.methods.addDetached.call(ctx)).not.toThrow()
    expect(() => Apps.methods.save.call(ctx)).not.toThrow()
    expect(apiFetch).toHaveBeenCalledOnce()
    expect(Object.values(JSON.parse(apiFetch.mock.calls[0][1].body))).not.toContain(null)
  })

  it('preserves false, zero, and configured optional values', () => {
    const app = {
      name: 'Configured',
      ...defaults,
      'prep-cmd': [{ do: 'prepare', undo: 'restore', elevated: false }],
      detached: ['background'],
      'auto-detach': false,
      'wait-all': false,
      'exit-timeout': 0,
      cmd: 'launch --argument',
      output: '/logs/app.log',
      'working-dir': '/apps',
      'image-path': '/covers/cover.png',
    }
    const ctx = editContext(app)

    Apps.methods.editApp.call(ctx, 0)
    Apps.methods.save.call(ctx)

    expect(ctx.editForm).toEqual({ ...app, index: 0 })
    expect(JSON.parse(apiFetch.mock.calls[0][1].body)).toEqual({ ...app, index: 0 })
    expect(app).not.toHaveProperty('index')
  })
})
