import { defineStore } from 'pinia'
import { ref, computed } from 'vue'

import * as KernelSU from '@/helpers/KernelSU'

import { useHomeStore } from '@/stores/Home'

export const useEncoreConfigStore = defineStore('encoreConfig', () => {
  const config = ref(null)

  const homeStore = useHomeStore()
  const preferences = computed(() => config.value?.preferences)

  const isLiteModeEnabled = computed(() => config.value?.preferences?.enforce_lite_mode ?? false)
  const logLevel = computed(() => config.value?.preferences?.log_level ?? 3)
  const isDeviceMitigationEnabled = computed(
    () => config.value?.preferences?.use_device_mitigation ?? false,
  )
  const isDisableTweaksEnabled = computed(() => config.value?.preferences?.disable_tweaks ?? false)
  const fasTargetFps = computed(() => config.value?.fas?.target_fps ?? 0)
  const fasMinFreqPercent = computed(() => config.value?.fas?.min_freq_percent ?? 40)
  const fasMarginFps = computed(() => config.value?.fas?.margin_fps ?? 1)
  const fasThermalLimit = computed(() => config.value?.fas?.thermal_limit ?? 90)
  const undervolt = computed(() => ({
    cpu_little: config.value?.undervolt?.cpu_little ?? 0,
    cpu_mid: config.value?.undervolt?.cpu_mid ?? 0,
    cpu_big: config.value?.undervolt?.cpu_big ?? 0,
    gpu: config.value?.undervolt?.gpu ?? 0,
  }))
  const isLoaded = computed(() => config.value !== null)

  const configPath = '/data/adb/.config/encore/config.json'

  async function loadConfig() {
    try {
      const content = await KernelSU.readFile(configPath)
      config.value = JSON.parse(content)
      console.log('Encore config loaded successfully')
      return config.value
    } catch (error) {
      console.error('Failed to load encore config:', error)
      config.value = null
      throw error
    }
  }

  async function saveConfig() {
    if (!config.value) {
      throw new Error('Config not loaded')
    }

    try {
      const configString = JSON.stringify(config.value, null, 2)
      await KernelSU.writeFile(configPath, configString)
      console.log('Encore config saved successfully')
      return true
    } catch (error) {
      console.error('Failed to save encore config:', error)
      throw error
    }
  }

  function ensureConfigStructure() {
    if (!config.value) {
      throw new Error('Config not loaded')
    }

    if (!config.value.preferences) {
      config.value.preferences = {}
    }
    if (config.value.preferences.use_device_mitigation === undefined) {
      config.value.preferences.use_device_mitigation = false
    }
    if (config.value.preferences.disable_tweaks === undefined) {
      config.value.preferences.disable_tweaks = false
    }
    if (config.value.preferences.enforce_lite_mode === undefined) {
      config.value.preferences.enforce_lite_mode = false
    }
    if (config.value.preferences.log_level === undefined) {
      config.value.preferences.log_level = 5
    }
    if (!config.value.fas) {
      config.value.fas = {}
    }
    if (config.value.fas.target_fps === undefined) {
      config.value.fas.target_fps = 0
    }
    if (config.value.fas.min_freq_percent === undefined) {
      config.value.fas.min_freq_percent = 40
    }
    if (config.value.fas.margin_fps === undefined) {
      config.value.fas.margin_fps = 1
    }
    if (config.value.fas.thermal_limit === undefined) {
      config.value.fas.thermal_limit = 90
    }
    if (!config.value.undervolt) {
      config.value.undervolt = { cpu_little: 0, cpu_mid: 0, cpu_big: 0, gpu: 0 }
    }
  }

  function setLiteMode(enabled) {
    ensureConfigStructure()
    config.value.preferences.enforce_lite_mode = enabled
  }

  function setLogLevel(level) {
    if (level < 0 || level > 5) {
      throw new Error('Log level must be between 0 and 5')
    }

    ensureConfigStructure()
    config.value.preferences.log_level = level
  }

  function setDeviceMitigation(enabled) {
    ensureConfigStructure()
    config.value.preferences.use_device_mitigation = enabled
  }

  function setDisableTweaks(enabled) {
    ensureConfigStructure()
    config.value.preferences.disable_tweaks = enabled
  }

  function setFasTargetFps(fps) {
    ensureConfigStructure()
    config.value.fas.target_fps = Math.max(0, Math.min(240, Math.round(fps)))
  }

  function setFasMinFreqPercent(percent) {
    ensureConfigStructure()
    config.value.fas.min_freq_percent = Math.max(10, Math.min(100, Math.round(percent)))
  }

  function setFasMarginFps(value) {
    ensureConfigStructure()
    config.value.fas.margin_fps = Math.max(0, Math.min(10, Number(value)))
  }

  function setFasThermalLimit(value) {
    ensureConfigStructure()
    const v = Math.round(Number(value))
    config.value.fas.thermal_limit = v <= 0 ? 0 : Math.max(60, Math.min(110, v))
  }

  // Voltage margin in percent, 0 = stock, negative = undervolt
  function setUndervolt(domain, percent) {
    ensureConfigStructure()
    config.value.undervolt[domain] = Math.max(-15, Math.min(0, Math.round(percent)))
  }

  function resetUndervolt() {
    ensureConfigStructure()
    config.value.undervolt = { cpu_little: 0, cpu_mid: 0, cpu_big: 0, gpu: 0 }
  }

  function updateConfig(newConfig) {
    if (!config.value) {
      throw new Error('Config not loaded')
    }

    config.value = {
      ...config.value,
      ...newConfig,
      preferences: {
        ...config.value.preferences,
        ...(newConfig.preferences || {}),
      },
      cpu_governor: {
        ...config.value.cpu_governor,
        ...(newConfig.cpu_governor || {}),
      },
      fas: {
        ...config.value.fas,
        ...(newConfig.fas || {}),
      },
      undervolt: {
        ...config.value.undervolt,
        ...(newConfig.undervolt || {}),
      },
    }
  }

  return {
    config,

    preferences,
    isLiteModeEnabled,
    logLevel,
    isDeviceMitigationEnabled,
    isDisableTweaksEnabled,
    fasTargetFps,
    fasMinFreqPercent,
    fasMarginFps,
    fasThermalLimit,
    undervolt,
    isLoaded,

    loadConfig,
    saveConfig,
    setLiteMode,
    setLogLevel,
    setDeviceMitigation,
    setDisableTweaks,
    setFasTargetFps,
    setFasMinFreqPercent,
    setFasMarginFps,
    setFasThermalLimit,
    setUndervolt,
    resetUndervolt,
    updateConfig,
  }
})
