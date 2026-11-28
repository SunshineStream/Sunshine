import { describe, expect, it } from 'vitest'

import { isConfigBoolEnabled } from '../../src_assets/common/assets/web/config_bool'

describe('isConfigBoolEnabled', () => {
  it.each([true, 1, 'true', 'yes', 'enable', 'enabled', 'on', '1', 'TRUE', 'On'])('treats %s as enabled', value => {
    expect(isConfigBoolEnabled(value)).toBe(true)
  })

  it.each([false, 0, 'disabled', 'false', 'no', 'off', '0', '', undefined, null])('treats %s as disabled', value => {
    expect(isConfigBoolEnabled(value)).toBe(false)
  })
})
