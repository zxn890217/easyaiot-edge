import { resolve } from 'node:path'
import type { ConfigEnv, UserConfig } from 'vite'
import dayjs from 'dayjs'
import { loadEnv } from 'vite'
import pkg from './package.json'
import { generateModifyVars } from './build/generate/generateModifyVars'
import { createProxy } from './build/vite/proxy'
import { wrapperEnv } from './build/utils'
import { createVitePlugins } from './build/vite/plugin'
import { OUTPUT_DIR } from './build/constant'
import { exclude, include } from './build/vite/optimize'

function pathResolve(dir: string) {
  return resolve(process.cwd(), '.', dir)
}

const { dependencies, devDependencies, name, version } = pkg
const __APP_INFO__ = {
  pkg: { dependencies, devDependencies, name, version },
  lastBuildTime: dayjs().format('YYYY-MM-DD HH:mm:ss'),
}

export default ({ command, mode }: ConfigEnv): UserConfig => {
  const root = process.cwd()

  const env = loadEnv(mode, root)

  // The boolean type read by loadEnv is a string. This function can be converted to boolean type
  const viteEnv = wrapperEnv(env)

  const { VITE_PORT, VITE_PUBLIC_PATH, VITE_PROXY, VITE_DROP_CONSOLE } = viteEnv

  const isBuild = command === 'build'

  const proxy = createProxy(VITE_PROXY)

  return {
    base: VITE_PUBLIC_PATH,
    root,
    server: {
      https: false,
      // Listening on all local IPs
      host: true,
      port: VITE_PORT,
      // Load proxy configuration from .env
      proxy,
      // Linux 上 IDE/多进程易占满 inotify，触发 ENOSPC；轮询不占用 file watcher 配额
      watch: isBuild
        ? undefined
        : {
            usePolling: viteEnv.VITE_USE_POLLING !== false,
            interval: 1000,
            ignored: ['**/node_modules/**', '**/.git/**', '**/__pycache__/**'],
          },
    },
    resolve: {
      alias: [
        {
          find: 'vue-i18n',
          replacement: 'vue-i18n/dist/vue-i18n.cjs.js',
        },
        // @/xxxx => src/xxxx
        {
          find: /\@\//,
          replacement: `${pathResolve('src')}/`,
        },
      ],
    },
    esbuild: {
      drop: VITE_DROP_CONSOLE ? ['console', 'debugger'] : [],
    },
    build: {
      target: 'esnext',
      cssTarget: 'chrome80',
      outDir: OUTPUT_DIR,
      // 临时排障开关：诊断 defineComponent is not defined 用，定位后改回 false
      sourcemap: true,
      // minify: 'terser',
      /**
       * 当 minify=“minify:'terser'” 解开注释
       * Uncomment when minify="minify:'terser'"
       */
      // terserOptions: {
      //   compress: {
      //     keep_infinity: true,
      //     drop_console: VITE_DROP_CONSOLE,
      //   },
      // },
      // Turning off brotliSize display can slightly reduce packaging time
      reportCompressedSize: false,
      chunkSizeWarningLimit: 2000,
      commonjsOptions: { include: [/node_modules/] },
      rollupOptions: {
        output: {
          manualChunks(id) {
            if (id.includes('node_modules')) {
              // Vue 全家桶 + ant-design-vue 放在一起，避免 defineComponent 等符号拆分断裂
              if (/vue-router|vue-i18n|pinia|@vueuse|ant-design-vue/.test(id)) return 'vendor-framework'
              // echarts 单独成 chunk（体积大且与框架无关）
              if (id.includes('echarts')) return 'vendor-echarts'
              // 其它第三方库统一归类
              return 'vendor'
            }
          },
        },
      },
    },
    define: {
      __APP_INFO__: JSON.stringify(__APP_INFO__),
    },

    css: {
      preprocessorOptions: {
        less: {
          modifyVars: generateModifyVars(),
          javascriptEnabled: true,
        },
      },
    },

    // The vite plugin used by the project. The quantity is large, so it is separately extracted and managed
    plugins: createVitePlugins(viteEnv, isBuild),

    optimizeDeps: { include, exclude },
  }
}
