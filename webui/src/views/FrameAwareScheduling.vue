<template>
  <div class="page h-full flex flex-col overflow-hidden bg-surface">
    <div class="max-w-3xl mx-auto h-full flex flex-col w-full">
      <div class="flex-none p-5 pb-3">
        <div class="flex items-center gap-4 mb-2">
          <button @click="goBack" class="text-on-surface transition-colors">
            <ArrowLeftIcon class="w-6 h-6 cursor-pointer rtl:rotate-180" />
          </button>
        </div>
      </div>

      <div class="scrollbar-hidden pb-safe-nav flex-1 min-h-0 overflow-y-scroll px-5">
        <div class="space-y-6">
          <h1 class="text-4xl text-on-surface mt-12 mb-6">
            {{ $t('fas.title') }}
          </h1>

          <div class="bg-primary-container rounded-3xl p-5 -mx-1.5">
            <div class="flex items-center justify-between">
              <h2 class="text-base font-medium text-on-primary-container">
                {{ $t('fas.toggle_title') }}
              </h2>
              <ToggleSwitch v-model="isFasEnabled" @update:modelValue="toggleFas" />
            </div>
          </div>

          <!-- Live status -->
          <div class="bg-surface-container rounded-3xl p-5 -mx-1.5 space-y-3">
            <div class="flex items-center justify-between">
              <h2 class="text-sm font-medium text-on-surface">{{ $t('fas.status_title') }}</h2>
              <span
                class="text-xs font-medium px-3 py-1 rounded-full"
                :class="
                  status.state === 'running'
                    ? 'bg-primary text-on-primary'
                    : 'bg-surface-dim text-on-surface-variant'
                "
              >
                {{ stateLabel }}
              </span>
            </div>

            <p class="text-xs" :class="moduleLoaded ? 'text-on-surface-variant' : 'text-error'">
              {{ moduleLoaded ? $t('fas.module_loaded') : $t('fas.module_missing') }}
            </p>

            <div v-if="status.state === 'running'" class="grid grid-cols-3 gap-2 pt-1">
              <div class="bg-surface-container-high rounded-2xl p-3">
                <p class="text-[11px] text-on-surface-variant">{{ $t('fas.current_fps') }}</p>
                <p class="text-lg font-medium text-on-surface tabular-nums">{{ status.fps }}</p>
              </div>
              <div class="bg-surface-container-high rounded-2xl p-3">
                <p class="text-[11px] text-on-surface-variant">{{ $t('fas.target_fps') }}</p>
                <p class="text-lg font-medium text-on-surface tabular-nums">
                  {{ status.target_fps }}
                </p>
              </div>
              <div class="bg-surface-container-high rounded-2xl p-3">
                <p class="text-[11px] text-on-surface-variant">{{ $t('fas.cpu_cap') }}</p>
                <p class="text-lg font-medium text-on-surface tabular-nums">
                  {{ Math.round(Number(status.cpu_cap)) }}%
                </p>
              </div>
            </div>
            <p v-if="status.state === 'running'" class="text-xs text-on-surface-variant truncate">
              {{ status.package }}
            </p>
          </div>

          <!-- Target FPS -->
          <div class="space-y-3">
            <h2 class="text-sm font-medium text-on-surface-variant px-1">
              {{ $t('fas.target_fps') }}
            </h2>
            <div class="flex flex-wrap gap-2">
              <button
                v-for="fps in fpsOptions"
                :key="fps"
                @click="setTargetFps(fps)"
                class="px-4 py-2 text-sm rounded-full transition-colors"
                :class="
                  targetFps === fps
                    ? 'bg-primary text-on-primary'
                    : 'bg-surface-container text-on-surface'
                "
              >
                {{ fps === 0 ? $t('fas.target_fps_auto') : fps }}
              </button>
            </div>
          </div>

          <!-- Minimum CPU cap -->
          <div class="space-y-3">
            <div class="flex items-center justify-between px-1">
              <h2 class="text-sm font-medium text-on-surface-variant">{{ $t('fas.min_freq') }}</h2>
              <span class="text-sm font-medium text-on-surface tabular-nums">{{ minFreq }}%</span>
            </div>
            <input
              type="range"
              min="20"
              max="100"
              step="5"
              v-model.number="minFreq"
              @change="setMinFreq(minFreq)"
              class="w-full accent-primary"
            />
            <p class="text-xs text-on-surface-variant leading-relaxed px-1">
              {{ $t('fas.min_freq_brief') }}
            </p>
          </div>

          <InformationOutlineIcon class="text-on-surface-variant my-6" :size="22" />
          <p class="text-sm text-on-surface-variant leading-relaxed">
            {{ $t('fas.brief') }}
          </p>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onBeforeUnmount } from 'vue'
import { useRouter, onBeforeRouteLeave } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useEncoreConfigStore } from '@/stores/EncoreConfig'
import { createDebouncedSave } from '@/helpers/Debounce'
import * as KernelSU from '@/helpers/KernelSU'
import { exec } from 'kernelsu'

import ArrowLeftIcon from '@/components/icons/ArrowLeft.vue'
import ToggleSwitch from '@/components/ui/ToggleSwitch.vue'
import InformationOutlineIcon from '@/components/icons/InformationOutline.vue'

const STATUS_FILE = '/data/adb/.config/encore/fas_status'
const FAS_DEVICE = '/dev/encore_fas'

const router = useRouter()
const { t } = useI18n()
const encoreConfigStore = useEncoreConfigStore()

const fpsOptions = [0, 30, 60, 90, 120, 144]

const isFasEnabled = ref(false)
const targetFps = ref(0)
const minFreq = ref(40)
const moduleLoaded = ref(false)
const status = ref({ state: 'idle' })
let pollTimer = null

const debouncedSave = createDebouncedSave(() => encoreConfigStore.saveConfig(), 500)

const stateLabel = computed(() => {
  const key = `fas.state.${status.value.state}`
  const label = t(key)
  return label === key ? status.value.state : label
})

function parseStatus(text) {
  const out = {}
  for (const line of text.split('\n')) {
    const idx = line.indexOf('=')
    if (idx > 0) out[line.slice(0, idx).trim()] = line.slice(idx + 1).trim()
  }
  return out.state ? out : { state: 'idle' }
}

async function refreshStatus() {
  try {
    // /dev/encore_fas is a character device, fileExists() only checks regular files
    const { errno } = await exec(`[ -c "${FAS_DEVICE}" ]`)
    moduleLoaded.value = errno === 0
  } catch {
    moduleLoaded.value = false
  }

  try {
    status.value = parseStatus(await KernelSU.readFile(STATUS_FILE))
  } catch {
    status.value = { state: 'idle' }
  }
}

onMounted(async () => {
  try {
    if (!encoreConfigStore.isLoaded) {
      await encoreConfigStore.loadConfig()
    }
    isFasEnabled.value = encoreConfigStore.isFasEnabled
    targetFps.value = encoreConfigStore.fasTargetFps
    minFreq.value = encoreConfigStore.fasMinFreqPercent
  } catch (error) {
    console.error('Failed to load FAS settings:', error)
  }

  refreshStatus()
  pollTimer = setInterval(refreshStatus, 1000)
})

onBeforeRouteLeave(async (to, from, next) => {
  try {
    await debouncedSave.flush()
    next()
  } catch (error) {
    console.error('Failed to save on route leave:', error)
    next(false)
  }
})

onBeforeUnmount(async () => {
  if (pollTimer) clearInterval(pollTimer)
  await debouncedSave.flush()
})

async function ensureLoaded() {
  if (!encoreConfigStore.isLoaded) {
    await encoreConfigStore.loadConfig()
  }
}

async function toggleFas(enabled) {
  isFasEnabled.value = enabled
  try {
    await ensureLoaded()
    encoreConfigStore.setFasEnabled(enabled)
    debouncedSave.trigger()
  } catch (error) {
    console.error('Failed to set FAS:', error)
    isFasEnabled.value = encoreConfigStore.isFasEnabled
  }
}

async function setTargetFps(fps) {
  targetFps.value = fps
  try {
    await ensureLoaded()
    encoreConfigStore.setFasTargetFps(fps)
    debouncedSave.trigger()
  } catch (error) {
    console.error('Failed to set FAS target FPS:', error)
  }
}

async function setMinFreq(percent) {
  try {
    await ensureLoaded()
    encoreConfigStore.setFasMinFreqPercent(percent)
    debouncedSave.trigger()
  } catch (error) {
    console.error('Failed to set FAS minimum CPU cap:', error)
  }
}

function goBack() {
  router.back()
}
</script>
