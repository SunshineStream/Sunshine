import fs from 'node:fs';
import { resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { defineConfig } from 'vite'
import { codecovVitePlugin } from "@codecov/vite-plugin";
import vue from '@vitejs/plugin-vue'
import process from 'node:process'

const projectRoot = fileURLToPath(new URL('.', import.meta.url));
let assetsSrcPath = 'src_assets/common/assets/web';
let assetsDstPath = 'build/assets/web';

if (process.env.SUNSHINE_BUILD_HOMEBREW) {
    console.log("Building for homebrew, using default paths")
}
else {
    // If the paths supplied in the environment variables contain any symbolic links
    // at any point in the series of directories, the entire build will fail with
    // a cryptic error message like this:
    //     RollupError: The "fileName" or "name" properties of emitted chunks and assets
    //     must be strings that are neither absolute nor relative paths.
    // To avoid this, we resolve the potential symlinks using `fs.realpathSync` before
    // doing anything else with the paths.
    if (process.env.SUNSHINE_SOURCE_ASSETS_DIR) {
        let path = resolve(fs.realpathSync(process.env.SUNSHINE_SOURCE_ASSETS_DIR), "common/assets/web");
        console.log("Using srcdir from Cmake: " + path);
        assetsSrcPath = path;
    }
    if (process.env.SUNSHINE_ASSETS_DIR) {
        let path = resolve(fs.realpathSync(process.env.SUNSHINE_ASSETS_DIR), "assets/web");
        console.log("Using destdir from Cmake: " + path);
        assetsDstPath = path;
    }
}

const emitTrayIconsPlugin = {
    name: 'emit-tray-icons',
    buildStart() {
        this.emitFile({
            type: 'asset',
            fileName: 'images/logo-sunshine.svg',
            source: fs.readFileSync(resolve(projectRoot, 'sunshine.svg')),
        });

        const virtualHidIcon = resolve(projectRoot, 'third-party/libvirtualhid/libvirtualhid.svg');
        if ((process.platform === 'win32' || process.platform === 'darwin') && fs.existsSync(virtualHidIcon)) {
            this.emitFile({
                type: 'asset',
                fileName: 'images/logo-libvirtualhid.svg',
                source: fs.readFileSync(virtualHidIcon),
            });
        }
    },
};

// https://vitejs.dev/config/
export default defineConfig({
    resolve: {
        alias: {
            vue: 'vue/dist/vue.esm-bundler.js'
        }
    },
    base: '/',
    plugins: [
        vue(),
        emitTrayIconsPlugin,
        // The Codecov vite plugin should be after all other plugins
        codecovVitePlugin({
            enableBundleAnalysis: true,
            bundleName: "sunshine",
            uploadToken: process.env.CODECOV_TOKEN,
            gitService: "github",
            dryRun: process.env.GITHUB_REPOSITORY !== 'SunshineStream/Sunshine',
            telemetry: process.env.GITHUB_REPOSITORY === 'SunshineStream/Sunshine',
        }),
    ],
    root: resolve(assetsSrcPath),
    build: {
        outDir: resolve(assetsDstPath),
        emptyOutDir: true,
    },
})
