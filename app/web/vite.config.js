import { defineConfig } from "vite";
import preact from "@preact/preset-vite";
import tailwindcss from "@tailwindcss/vite";

// The page, built into dist/, which the server serves at / (app/server/main.py). Every URL is relative
// (base "./"), so it runs under any path prefix. `npm run dev` proxies to a server on port 8080
// (./scripts/dev.sh starts both).
export default defineConfig({
  base: "./",
  plugins: [preact(), tailwindcss()],
  build: { outDir: "dist", emptyOutDir: true, assetsDir: "assets", chunkSizeWarningLimit: 300 },
  server: {
    proxy: {
      "/api": "http://127.0.0.1:8080",
      "/ws": { target: "ws://127.0.0.1:8080", ws: true },
    },
  },
});
