import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

// The SPA in web/. Dev: the Vite server proxies /api to the admin API (npm run dev:api, port 3200).
export default defineConfig({
  root: 'web',
  plugins: [react()],
  build: { outDir: 'dist', emptyOutDir: true },
  server: {
    port: 5173,
    proxy: { '/api': process.env.ADMIN_API_URL ?? 'http://127.0.0.1:3200' },
  },
});
