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
            {{ $t('undervolt.title') }}
          </h1>

          <div v-if="!supported" class="bg-surface-container rounded-3xl p-5 -mx-1.5">
            <p class="text-sm text-error leading-relaxed">{{ $t('undervolt.unsupported') }}</p>
          </div>

          <template v-else>
            <div class="bg-primary-container rounded-3xl p-5 -mx-1.5 space-y-3">
              <p class="text-sm text-on-primary-container leading-relaxed">
                {{ $t('undervolt.warning') }}
              </p>
              <p class="text-xs text-on-primary-container/80 leading-relaxed">
                {{ $t('undervolt.safety') }}
              </p>
            </div>

            <div
              v-if="lastUnstable.length"
              class="bg-surface-container rounded-3xl p-5 -mx-1.5 space-y-2"
            >
              <p class="text-sm font-medium text-error">{{ $t('undervolt.last_unstable') }}</p>
              <div class="flex flex-wrap gap-2">
                <span
                  v-for="item in lastUnstable"
                  :key="item.domain"
                  class="text-xs px-3 py-1 rounded-full bg-surface-container-high text-on-surface tabular-nums"
                >
                  {{ $t(`undervolt.${item.domain}`) }} {{ item.value }}%
                </span>
              </div>
              <p class="text-xs text-on-surface-variant leading-relaxed">
                {{ $t('undervolt.last_unstable_hint') }}
              </p>
            </div>

            <div
              v-if="isUndervolted"
              class="flex items-center gap-2 text-xs px-1"
              :class="pending ? 'text-error' : 'text-on-surface-variant'"
            >
              <span
                class="w-2 h-2 rounded-full shrink-0"
                :class="pending ? 'bg-error' : 'bg-primary'"
              ></span>
              {{ pending ? $t('undervolt.pending') : $t('undervolt.stable') }}
            </div>

            <div v-for="domain in domains" :key="domain" class="space-y-2">
              <div class="flex items-center justify-between px-1">
                <h2 class="text-sm font-medium text-on-surface-variant">
                  {{ $t(`undervolt.${domain}`) }}
                </h2>
                <span
                  class="text-sm font-medium tabular-nums"
                  :class="values[domain] < 0 ? 'text-primary' : 'text-on-surface'"
                >
                  {{ values[domain] }}%
                </span>
              </div>
              <input
                type="range"
                min="-15"
                max="0"
                step="1"
                v-model.number="values[domain]"
                @change="setMargin(domain, values[domain])"
                class="w-full accent-primary"
              />
            </div>

            <button
              @click="resetAll"
              :disabled="!isUndervolted"
              class="px-4 py-2 text-sm font-medium rounded-full transition-colors bg-surface-container text-on-surface disabled:opacity-50"
            >
              {{ $t('undervolt.reset') }}
            </button>

            <InformationOutlineIcon class="text-on-surface-variant my-6" :size="22" />
            <p class="text-sm text-on-surface-variant leading-relaxed">
              {{ $t('undervolt.brief') }}
            </p>
          </template>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, reactive, computed, onMounted, onBeforeUnmount } from 'vue'
import { useRouter, onBeforeRouteLeave } from 'vue-router'
import { useEncoreConfigStore } from '@/stores/EncoreConfig'
import { createDebouncedSave } from '@/helpers/Debounce'
import { exec } from 'kernelsu'

import ArrowLeftIcon from '@/components/icons/ArrowLeft.vue'
import InformationOutlineIcon from '@/components/icons/InformationOutline.vue'

const MARGIN_DIR = '/sys/kernel/percent_margin'
const PENDING_FILE = '/data/adb/.config/encore/.undervolt_pending'
const UNSTABLE_FILE = '/data/adb/.config/encore/undervolt_unstable'
const MARKER_KEYS = { little: 'cpu_little', mid: 'cpu_mid', big: 'cpu_big', gpu: 'gpu' }

const router = useRouter()
const encoreConfigStore = useEncoreConfigStore()

const domains = ['cpu_little', 'cpu_mid', 'cpu_big', 'gpu']
const values = reactive({ cpu_little: 0, cpu_mid: 0, cpu_big: 0, gpu: 0 })
const supported = ref(true)
const pending = ref(false)
const lastUnstable = ref([])
let pollTimer = null

const debouncedSave = createDebouncedSave(() => encoreConfigStore.saveConfig(), 500)

const isUndervolted = computed(() => domains.some((d) => values[d] !== 0))

async function testPath(flag, path) {
  try {
    const { errno } = await exec(`[ ${flag} "${path}" ]`)
    return errno === 0
  } catch {
    return false
  }
}

// "little=-3 mid=-3 big=0 gpu=-3" -> [{ domain: 'cpu_little', value: -3 }, ...]
function parseMargins(text) {
  return text
    .trim()
    .split(/\s+/)
    .map((token) => token.split('='))
    .filter(([key, value]) => MARKER_KEYS[key] && Number(value) !== 0)
    .map(([key, value]) => ({ domain: MARKER_KEYS[key], value: Number(value) }))
}

async function refreshStatus() {
  supported.value = await testPath('-d', MARGIN_DIR)
  pending.value = await testPath('-f', PENDING_FILE)

  try {
    const { errno, stdout } = await exec(`cat "${UNSTABLE_FILE}"`)
    lastUnstable.value = errno === 0 ? parseMargins(stdout) : []
  } catch {
    lastUnstable.value = []
  }
}

onMounted(async () => {
  try {
    if (!encoreConfigStore.isLoaded) {
      await encoreConfigStore.loadConfig()
    }
    Object.assign(values, encoreConfigStore.undervolt)
  } catch (error) {
    console.error('Failed to load undervolt settings:', error)
  }

  refreshStatus()
  pollTimer = setInterval(refreshStatus, 2000)
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

async function setMargin(domain, percent) {
  try {
    if (!encoreConfigStore.isLoaded) {
      await encoreConfigStore.loadConfig()
    }
    encoreConfigStore.setUndervolt(domain, percent)
    debouncedSave.trigger()
  } catch (error) {
    console.error('Failed to set undervolt:', error)
  }
}

async function resetAll() {
  for (const d of domains) values[d] = 0
  try {
    if (!encoreConfigStore.isLoaded) {
      await encoreConfigStore.loadConfig()
    }
    encoreConfigStore.resetUndervolt()
    debouncedSave.trigger()
  } catch (error) {
    console.error('Failed to reset undervolt:', error)
  }
}

function goBack() {
  router.back()
}
</script>
