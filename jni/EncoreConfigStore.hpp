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

#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <rapidjson/document.h>

#include <Encore.hpp>
#include <EncoreLog.hpp>

class EncoreConfigStore {
public:
    struct Preferences {
        bool enforce_lite_mode = false;
        bool use_device_mitigation = false;
        bool disable_tweaks = false;
        int log_level = 4;
    };

    struct FasConfig {
        int target_fps = 0;        // 0 = auto detect
        int min_freq_percent = 40; // lowest CPU frequency FAS may set, % of max freq
        double margin_fps = 1.0;   // aim this many fps (per 60) below target
        int thermal_limit = 90;    // CPU temperature (C) above which target is lowered, 0 = off

        bool operator==(const FasConfig &) const = default;
    };

    // Voltage margin in percent per domain, 0 = stock, negative = undervolt
    struct UndervoltConfig {
        int cpu_little = 0;
        int cpu_mid = 0;
        int cpu_big = 0;
        int gpu = 0;

        bool operator==(const UndervoltConfig &) const = default;
    };

    struct CPUGovernor {
        std::string balance;
        std::string powersave;
    };

    struct ConfigData {
        Preferences preferences;
        CPUGovernor cpu_governor;
        FasConfig fas;
        UndervoltConfig undervolt;
    };

    /**
     * @brief Get singleton instance
     */
    static EncoreConfigStore &get_instance() {
        static EncoreConfigStore instance;
        return instance;
    }

    /**
     * @brief Load configuration from file
     * @param config_path Path to config file (defaults to CONFIG_FILE)
     * @return true if loaded successfully, false otherwise
     */
    bool load_config(const std::string &config_path = CONFIG_FILE);

    /**
     * @brief Save current configuration to file
     * @param config_path Optional custom path, uses loaded path by default
     * @return true if saved successfully, false otherwise
     */
    bool save_config(const std::string &config_path = CONFIG_FILE);

    /**
     * @brief Get current configuration
     */
    ConfigData get_config() const;

    /**
     * @brief Get preferences
     */
    Preferences get_preferences() const;

    /**
     * @brief Get CPU governor settings
     */
    CPUGovernor get_cpu_governor() const;

    /**
     * @brief Get FAS settings
     */
    FasConfig get_fas_config() const;

    /**
     * @brief Get undervolt settings
     */
    UndervoltConfig get_undervolt_config() const;

    /**
     * @brief Update undervolt settings (call save_config() to persist)
     */
    void set_undervolt_config(const UndervoltConfig &undervolt);

    /**
     * @brief Update preferences
     */
    void set_preferences(const Preferences &prefs);

    /**
     * @brief Update CPU governor settings
     */
    void set_cpu_governor(const CPUGovernor &governor);

    /**
     * @brief Get config file path
     */
    std::string get_config_path() const;

    /**
     * @brief Reload configuration from disk
     */
    bool reload();

private:
    EncoreConfigStore() = default;
    ~EncoreConfigStore() = default;

    EncoreConfigStore(const EncoreConfigStore &) = delete;
    EncoreConfigStore &operator=(const EncoreConfigStore &) = delete;

    /**
     * @brief Read default CPU governor from file
     */
    std::string read_default_cpu_governor() const;

    /**
     * @brief Create default configuration file
     */
    bool create_default_config();

    /**
     * @brief Parse JSON document into config structure
     */
    bool parse_config(const rapidjson::Document &doc);

    mutable std::mutex mutex_;
    ConfigData config_;
    std::string config_path_ = CONFIG_FILE;
};

#define config_store EncoreConfigStore::get_instance()
