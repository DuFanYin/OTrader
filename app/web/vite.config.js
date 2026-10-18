import { defineConfig } from "vite";
import preact from "@preact/preset-vite";
import tailwindcss from "@tailwindcss/vite";
import { readFileSync } from "node:fs";

// The page, built into dist/, which the server serves at / (app/server/main.py). Every URL is relative
// (base "./"), so it runs under any path prefix. `npm run dev` proxies to the server at OPTRA_SERVER
// (default http://127.0.0.1:8080). ./scripts/dev.sh starts both and sets OPTRA_KEY_FILE: then every
// request carries the person's key (X-Optra-Key, app/server/optra/infra/access.py), as Optra's front door
// adds it once signed in, so the local user works the page as the person.
const server = process.env.OPTRA_SERVER || "http://127.0.0.1:8080";
const keyFile = process.env.OPTRA_KEY_FILE;
const withKey = (proxy) =>
  proxy.on("proxyReq", (request) => {
    try {
      request.setHeader("x-optra-key", readFileSync(keyFile, "utf8").trim()); // made by the server on its first start
    } catch {
      // no key yet: the request goes as the agent's
    }
  });

export default defineConfig({
  base: "./",
  plugins: [preact(), tailwindcss()],
  build: { outDir: "dist", emptyOutDir: true, assetsDir: "assets", chunkSizeWarningLimit: 300 },
  server: {
    proxy: {
      "/api": { target: server, configure: keyFile ? withKey : undefined },
      "/ws": { target: server.replace(/^http/, "ws"), ws: true, configure: keyFile ? withKey : undefined },
    },
  },
});
