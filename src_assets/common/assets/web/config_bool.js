/**
 * Check whether a configuration value enables a boolean option.
 *
 * Mirrors config::to_bool in src/config.cpp; keep this set in sync with the backend.
 *
 * @param {*} value Configuration value.
 * @returns {boolean} Whether the value enables the option.
 */
export function isConfigBoolEnabled(value) {
  if (value === true || value === 1) {
    return true
  }
  const normalized = String(value ?? '').toLowerCase();
  return ['true', 'yes', 'enable', 'enabled', 'on'].includes(normalized) || normalized.includes('1');
}
