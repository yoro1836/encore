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

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "EncoreConfigStore.hpp"

bool EncoreConfigStore::load_config(const std::string &config_path) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_path_ = config_path;
    }

    FILE *fp = fopen(config_path.c_str(), "rb");
    if (!fp) {
        LOGW_TAG("EncoreConfigStore", "Config file not found, creating default: {}", config_path);
        return create_default_config();
    }

    char readBuffer[65536];
    rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));

    rapidjson::Document doc;
    doc.ParseStream(is);
    fclose(fp);

    if (doc.HasParseError()) {
        LOGE_TAG(
            "EncoreConfigStore",
            "Parse error: {} (Offset: {})",
            rapidjson::GetParseError_En(doc.GetParseError()),
            doc.GetErrorOffset()
        );
        return create_default_config();
    }

    if (!doc.IsObject()) {
        LOGE_TAG("EncoreConfigStore", "Root is not an object");
        return false;
    }

    return parse_config(doc);
}

bool EncoreConfigStore::save_config(const std::string &config_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    rapidjson::Document doc;
    doc.SetObject();
    rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();

    // Serialize preferences
    rapidjson::Value prefs_obj(rapidjson::kObjectType);
    prefs_obj.AddMember("enforce_lite_mode", config_.preferences.enforce_lite_mode, allocator);
    prefs_obj.AddMember("use_device_mitigation", config_.preferences.use_device_mitigation, allocator);
    prefs_obj.AddMember("disable_tweaks", config_.preferences.disable_tweaks, allocator);
    prefs_obj.AddMember("log_level", config_.preferences.log_level, allocator);
    doc.AddMember("preferences", prefs_obj, allocator);

    // Serialize CPU governor
    rapidjson::Value cpu_gov_obj(rapidjson::kObjectType);
    cpu_gov_obj.AddMember("balance", rapidjson::Value(config_.cpu_governor.balance.c_str(), allocator).Move(), allocator);
    cpu_gov_obj.AddMember("powersave", rapidjson::Value(config_.cpu_governor.powersave.c_str(), allocator).Move(), allocator);
    doc.AddMember("cpu_governor", cpu_gov_obj, allocator);

    // Serialize FAS settings
    rapidjson::Value fas_obj(rapidjson::kObjectType);
    fas_obj.AddMember("target_fps", config_.fas.target_fps, allocator);
    fas_obj.AddMember("min_freq_percent", config_.fas.min_freq_percent, allocator);
    fas_obj.AddMember("margin_fps", config_.fas.margin_fps, allocator);
    fas_obj.AddMember("thermal_limit", config_.fas.thermal_limit, allocator);
    doc.AddMember("fas", fas_obj, allocator);

    // Serialize undervolt settings
    rapidjson::Value uv_obj(rapidjson::kObjectType);
    uv_obj.AddMember("cpu_little", config_.undervolt.cpu_little, allocator);
    uv_obj.AddMember("cpu_mid", config_.undervolt.cpu_mid, allocator);
    uv_obj.AddMember("cpu_big", config_.undervolt.cpu_big, allocator);
    uv_obj.AddMember("gpu", config_.undervolt.gpu, allocator);
    doc.AddMember("undervolt", uv_obj, allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    writer.SetIndent(' ', 2);
    doc.Accept(writer);

    FILE *fp = fopen(config_path.c_str(), "wb");
    if (!fp) {
        LOGE_TAG("EncoreConfigStore", "Failed to open config file for writing: {}", config_path);
        return false;
    }

    fwrite(buffer.GetString(), 1, buffer.GetSize(), fp);
    fclose(fp);

    LOGI_TAG("EncoreConfigStore", "Configuration saved to {}", config_path);
    return true;
}

EncoreConfigStore::ConfigData EncoreConfigStore::get_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

EncoreConfigStore::Preferences EncoreConfigStore::get_preferences() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.preferences;
}

EncoreConfigStore::CPUGovernor EncoreConfigStore::get_cpu_governor() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.cpu_governor;
}

EncoreConfigStore::FasConfig EncoreConfigStore::get_fas_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.fas;
}

EncoreConfigStore::UndervoltConfig EncoreConfigStore::get_undervolt_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.undervolt;
}

void EncoreConfigStore::set_undervolt_config(const UndervoltConfig &undervolt) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.undervolt = undervolt;
}

void EncoreConfigStore::set_preferences(const Preferences &prefs) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.preferences = prefs;
}

void EncoreConfigStore::set_cpu_governor(const CPUGovernor &governor) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.cpu_governor = governor;
}

std::string EncoreConfigStore::get_config_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_path_;
}

bool EncoreConfigStore::reload() {
    return load_config(config_path_);
}

std::string EncoreConfigStore::read_default_cpu_governor() const {
    // Fallback governor
    std::string default_governor = "schedutil";

    std::ifstream file(DEFAULT_CPU_GOV);
    if (!file.is_open()) {
        LOGW_TAG("EncoreConfigStore", "Default CPU governor file not found, using fallback: {}", default_governor);
    }

    if (std::getline(file, default_governor)) {
        LOGD_TAG("EncoreConfigStore", "Read default CPU governor from file: {}", default_governor);
    } else {
        LOGW_TAG("EncoreConfigStore", "Default CPU governor file is empty, using fallback: {}", default_governor);
    }

    file.close();
    return default_governor;
}

bool EncoreConfigStore::create_default_config() {
    std::string default_governor = read_default_cpu_governor();

    // clang-format off
    ConfigData default_config = ConfigData{
        .preferences = {
            .enforce_lite_mode = false,
            .use_device_mitigation = false,
            .disable_tweaks = false,
            .log_level = 4
        },
        .cpu_governor = {
            .balance = default_governor,
            .powersave = default_governor
        },
        .fas = {
            .target_fps = 0,
            .min_freq_percent = 40,
            .margin_fps = 1.0,
            .thermal_limit = 90
        },
        .undervolt = {}
    };
    // clang-format on

    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = default_config;
    }

    return save_config(config_path_);
}

bool EncoreConfigStore::parse_config(const rapidjson::Document &doc) {
    ConfigData new_config;

    // Parse preferences
    if (doc.HasMember("preferences") && doc["preferences"].IsObject()) {
        const rapidjson::Value &prefs = doc["preferences"];

        if (prefs.HasMember("enforce_lite_mode") && prefs["enforce_lite_mode"].IsBool()) {
            new_config.preferences.enforce_lite_mode = prefs["enforce_lite_mode"].GetBool();
        }

        if (prefs.HasMember("use_device_mitigation") && prefs["use_device_mitigation"].IsBool()) {
            new_config.preferences.use_device_mitigation = prefs["use_device_mitigation"].GetBool();
        }

        if (prefs.HasMember("disable_tweaks") && prefs["disable_tweaks"].IsBool()) {
            new_config.preferences.disable_tweaks = prefs["disable_tweaks"].GetBool();
        }

        if (prefs.HasMember("log_level") && prefs["log_level"].IsInt()) {
            new_config.preferences.log_level = prefs["log_level"].GetInt();
        }
    }

    // Parse CPU governor
    if (doc.HasMember("cpu_governor") && doc["cpu_governor"].IsObject()) {
        const rapidjson::Value &gov = doc["cpu_governor"];

        if (gov.HasMember("balance") && gov["balance"].IsString()) {
            new_config.cpu_governor.balance = gov["balance"].GetString();
        }

        if (gov.HasMember("powersave") && gov["powersave"].IsString()) {
            new_config.cpu_governor.powersave = gov["powersave"].GetString();
        }
    }

    // Parse FAS settings
    if (doc.HasMember("fas") && doc["fas"].IsObject()) {
        const rapidjson::Value &fas = doc["fas"];

        if (fas.HasMember("target_fps") && fas["target_fps"].IsInt()) {
            new_config.fas.target_fps = std::clamp(fas["target_fps"].GetInt(), 0, 240);
        }

        if (fas.HasMember("min_freq_percent") && fas["min_freq_percent"].IsInt()) {
            new_config.fas.min_freq_percent = std::clamp(fas["min_freq_percent"].GetInt(), 10, 100);
        }

        if (fas.HasMember("margin_fps") && fas["margin_fps"].IsNumber()) {
            new_config.fas.margin_fps = std::clamp(fas["margin_fps"].GetDouble(), 0.0, 10.0);
        }

        if (fas.HasMember("thermal_limit") && fas["thermal_limit"].IsInt()) {
            const int limit = fas["thermal_limit"].GetInt();
            new_config.fas.thermal_limit = limit <= 0 ? 0 : std::clamp(limit, 60, 110);
        }
    }

    // Parse undervolt settings (only negative margins are allowed)
    if (doc.HasMember("undervolt") && doc["undervolt"].IsObject()) {
        const rapidjson::Value &uv = doc["undervolt"];
        auto read_margin = [&uv](const char *key, int &out) {
            if (uv.HasMember(key) && uv[key].IsInt()) {
                out = std::clamp(uv[key].GetInt(), UNDERVOLT_MIN_PERCENT, 0);
            }
        };
        read_margin("cpu_little", new_config.undervolt.cpu_little);
        read_margin("cpu_mid", new_config.undervolt.cpu_mid);
        read_margin("cpu_big", new_config.undervolt.cpu_big);
        read_margin("gpu", new_config.undervolt.gpu);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = new_config;
    }

    LOGI_TAG("EncoreConfigStore", "Configuration loaded from {}", config_path_);
    return true;
}
