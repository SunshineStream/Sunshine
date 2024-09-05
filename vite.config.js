import { fileURLToPath, URL } from 'node:url'
import fs from 'fs';
import { resolve } from 'path'
import { defineConfig } from 'vite'
import { ViteEjsPlugin } from "vite-plugin-ejs";
import vue from '@vitejs/plugin-vue'
import process from 'process'

/**
 * Before actually building the pages with Vite, we do an intermediate build step using ejs
 * Importing this separately and joining them using ejs 
 * allows us to split some repeating HTML that cannot be added 
 * by Vue itself (e.g. style/script loading, common meta head tags, Widgetbot)
 * The vite-plugin-ejs handles this automatically
 */
let assetsSrcPath = 'src_assets/common/assets/web';
let assetsDstPath = 'build/assets/web';

if (process.env.SUNSHINE_BUILD_HOMEBREW) {
    console.log("Building for homebrew, using default paths")
}
else {
    if (process.env.SUNSHINE_SOURCE_ASSETS_DIR) {
        console.log("Using srcdir from Cmake: " + resolve(process.env.SUNSHINE_SOURCE_ASSETS_DIR,"common/assets/web"));
        assetsSrcPath = resolve(process.env.SUNSHINE_SOURCE_ASSETS_DIR,"common/assets/web")
    }
    if (process.env.SUNSHINE_ASSETS_DIR) {
        console.log("Using destdir from Cmake: " + resolve(process.env.SUNSHINE_ASSETS_DIR,"assets/web"));
        assetsDstPath = resolve(process.env.SUNSHINE_ASSETS_DIR,"assets/web")
    }
}

let header = fs.readFileSync(resolve(assetsSrcPath, "template_header.html"))

// https://vitejs.dev/config/
export default defineConfig({
    resolve: {
        alias: {
            vue: 'vue/dist/vue.esm-bundler.js'
        }
    },
    plugins: [vue(), ViteEjsPlugin({ header })],
    root: resolve(assetsSrcPath),
    build: {
        outDir: resolve(assetsDstPath),
        rollupOptions: {
            input: {
                apps: resolve(assetsSrcPath, 'apps.html'),
                config: resolve(assetsSrcPath, 'config.html'),
                login: resolve(assetsSrcPath, 'login.html'),
                index: resolve(assetsSrcPath, 'index.html'),
                password: resolve(assetsSrcPath, 'password.html'),
                pin: resolve(assetsSrcPath, 'pin.html'),
                troubleshooting: resolve(assetsSrcPath, 'troubleshooting.html'),
                welcome: resolve(assetsSrcPath, 'welcome.html'),
            },
        },
    },
})
