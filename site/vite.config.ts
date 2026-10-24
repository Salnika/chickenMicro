import { defineConfig } from 'vite';

// Project page: https://salnika.github.io/chickenMicro/
// Every asset URL has to carry that prefix, so `base` is not optional here.
// Override it with SITE_BASE=/ when serving the build from a domain root.
const base = process.env.SITE_BASE ?? '/chickenMicro/';

export default defineConfig({
  base,
  build: {
    target: 'es2022',
    // The firmware images live in public/firmware and are fetched at runtime
    // rather than bundled, so a rebuilt binary does not invalidate the JS.
    assetsInlineLimit: 4096,
  },
});
