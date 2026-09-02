// Vitest configuration for the Aius JS regressions (test/sd-editor/*.test.js).
//
// The firmware serves a small vanilla-JS UI off the Teensy's SD card
// (firmware/sd/www/*), and these suites load those shipped files directly --
// no build step, no served URL. This tree is a git submodule of the upstream repository,
// whose root vitest.config.js is the repo-graph compiler's vitest anchor and
// deliberately covers only `src/**`; the aius suites therefore run with this
// directory as the vitest root:
//
//   pnpm exec vitest run --root git_modules/aius          (from the repository root)
//   firmware/scripts/test.sh --environment native --suite sd-editor
//
// Dependencies (vitest, happy-dom) resolve up the tree to the upstream repo's
// node_modules; the suites declare `@vitest-environment happy-dom` per file.
// The cache goes under firmware/.build/ (already ignored) -- Vite's default
// would create an untracked node_modules/.vite/ at this root.
export default {
  cacheDir: 'firmware/.build/vitest',
  test: {
    include: ['test/**/*.test.js'],
    environment: 'node',
    globals: false,
  },
};
