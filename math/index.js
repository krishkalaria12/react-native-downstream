/** Enable the installed native math engine for a Downstream view or provider. */
function math(options = {}) {
  if (
    typeof options !== 'object' ||
    options === null ||
    Array.isArray(options)
  ) {
    throw new TypeError('Downstream math options must be an object.');
  }
  return {
    name: 'math',
    native: { math: true },
    parse: { math: { ...options } },
  };
}

module.exports = { math };
