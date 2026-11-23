/*
 * Copyright (C) 2024-2026 Rem01Gaming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <string>

#define NOTIFY_TITLE "Encore Tweaks"
#define LOG_TAG "EncoreTweaks"

#define CONFIG_DIR "/data/adb/.config/encore"
#define MODPATH "/data/adb/modules/encore"

#define LOCK_FILE CONFIG_DIR "/.lock"
#define JAVA_LOCK_FILE CONFIG_DIR "/java.lock"
#define LOG_FILE CONFIG_DIR "/encore.log"
#define PROFILE_MODE CONFIG_DIR "/current_profile"
#define GAME_INFO CONFIG_DIR "/gameinfo"
#define CONFIG_FILE CONFIG_DIR "/config.json"
#define DEVICE_MITIGATION_FILE CONFIG_DIR "/device_mitigation.json"
#define DEFAULT_CPU_GOV CONFIG_DIR "/default_cpu_gov"
#define ENCORE_GAMELIST CONFIG_DIR "/gamelist.json"
#define SYSTEM_STATUS_FILE CONFIG_DIR "/system_status"
#define FAS_STATUS_FILE CONFIG_DIR "/fas_status"
#define FAS_DEVICE "/dev/encore_fas"
#define UNDERVOLT_PENDING_FILE CONFIG_DIR "/.undervolt_pending"
#define UNDERVOLT_UNSTABLE_FILE CONFIG_DIR "/undervolt_unstable"
#define UNDERVOLT_MARGIN_DIR "/sys/kernel/percent_margin"
#define UNDERVOLT_MIN_PERCENT (-15)

#define MODULE_PROP MODPATH "/module.prop"
#define MODULE_UPDATE MODPATH "/update"

enum EncoreProfileMode : char {
    PERFCOMMON,
    PERFORMANCE_PROFILE,
    BALANCE_PROFILE,
    POWERSAVE_PROFILE
};

struct EncoreGameList {
    std::string package_name;
    bool lite_mode;
    bool enable_dnd;
    bool enable_fas = false;
    // Samsung Exynos game governor (gperf) knob profile for this game:
    // "" = off, else one of GPERF_PROFILES. Takes precedence over FAS.
    std::string gperf_profile;
};

// gperf knob profiles shipped in CONFIG_DIR/gperf/<name>.knob ("stock" has no file)
inline constexpr const char *GPERF_PROFILES[] = {"stock", "balanced", "performance", "cool"};
#define GPERF_BINARY "/vendor/bin/gperf"
#define GPERF_DEVICE "/dev/exynos-profiler"
