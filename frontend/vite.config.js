import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { resolve } from 'path'

export default defineConfig({
  plugins: [vue()],
  build: {
    outDir: resolve(__dirname, '../static'),
    emptyOutDir: true,
    rollupOptions: {
      output: {
        // Use fixed filenames so CMake embedding is predictable
        entryFileNames: 'assets/app.js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: 'assets/[name][extname]',
      },
    },
  },
  server: {
    proxy: {
      '/api': 'http://localhost:8080',
      '/stream': 'http://localhost:8080',
      '/snapshot': 'http://localhost:8080',
    },
  },
})
