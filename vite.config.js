import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import basicSsl from '@vitejs/plugin-basic-ssl'

export default defineConfig({
  plugins: [react(), basicSsl()],
  server: {
    host: true,   // expose on local network so phone can connect
    https: true,  // required for Web Bluetooth on mobile
  },
})
