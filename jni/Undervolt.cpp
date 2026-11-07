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

#include "Undervolt.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

#include <Encore.hpp>
#include <EncoreLog.hpp>
#include <EncoreUtility.hpp>

#include "EncoreConfigStore.hpp"

#define UV_TAG "Undervolt"

namespace Undervolt {
namespace {

using UndervoltConfig = EncoreConfigStore::UndervoltConfig;

// How long the device must stay up before new margins count as stable
constexpr int kConfirmSeconds = 120;

// percent_margin attribute names per user facing domain.
// Not every SoC has every domain (e.g. Exynos 2400 has cpucl1l/cpucl1h
// instead of cpucl1); writes to missing domains are rejected and skipped.
const std::vector<const char *> kLittle = {"cpucl0"};
const std::vector<const char *> kMid = {"cpucl1", "cpucl1l", "cpucl1h"};
const std::vector<const char *> kBig = {"cpucl2"};
const std::vector<const char *> kGpu = {"g3d"};

std::mutex g_mutex;
// Margins Encore has written so far. Starts at stock (0) and nodes are only
// written when a domain differs from it: the kernel may apply its own boot
// margins through the same ACPM request, and writing 0 would unset those.
UndervoltConfig g_applied{};
std::atomic<uint64_t> g_generation{0};

std::string margin_node(const char *domain) {
    return std::string(UNDERVOLT_MARGIN_DIR) + "/" + domain + "_margin_percent";
}

void set_domain_margin(const std::vector<const char *> &domains, int percent) {
    const std::string value = std::to_string(percent);
    for (const char *domain : domains) {
        const std::string node = margin_node(domain);
        if (access(node.c_str(), F_OK) != 0) continue;

        int fd = open(node.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
        if (fd < 0) {
            LOGW_TAG(UV_TAG, "Cannot open {}: {}", node, strerror(errno));
            continue;
        }
        if (write(fd, value.data(), value.size()) < 0) {
            // EINVAL here means this SoC has no DVFS domain behind the node
            LOGD_TAG(UV_TAG, "{} rejected {}%: {}", domain, percent, strerror(errno));
        } else {
            LOGI_TAG(UV_TAG, "{} margin set to {}%", domain, percent);
        }
        close(fd);
    }
}

bool is_stock(const UndervoltConfig &cfg) {
    return cfg == UndervoltConfig{};
}

std::string describe(const UndervoltConfig &cfg) {
    return "little=" + std::to_string(cfg.cpu_little) + " mid=" + std::to_string(cfg.cpu_mid) +
           " big=" + std::to_string(cfg.cpu_big) + " gpu=" + std::to_string(cfg.gpu);
}

/**
 * Turns "little=-3 mid=-2 big=0 gpu=-1" into "CPU little -3%, CPU mid -2%, GPU -1%".
 */
std::string humanize(const std::string &desc) {
    std::istringstream iss(desc);
    std::string token, out;
    while (iss >> token) {
        auto eq = token.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = token.substr(0, eq);
        const std::string val = token.substr(eq + 1);
        if (val == "0") continue;
        const std::string name = key == "gpu" ? "GPU" : "CPU " + key;
        if (!out.empty()) out += ", ";
        out += name + " " + val + "%";
    }
    return out;
}

/**
 * Posts a notification, retrying for a while because system services may
 * not be ready yet right after boot.
 */
void notify_with_retry(std::string message) {
    std::thread([message = std::move(message)]() {
        for (int attempt = 0; attempt < 12; attempt++) {
            if (notify(message.c_str())) return;
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
        LOGW_TAG(UV_TAG, "Giving up posting undervolt notification");
    }).detach();
}

/**
 * Writes the pending marker and makes sure it reaches storage before any
 * margin is applied, otherwise a crash could lose it.
 */
void write_pending_marker(const UndervoltConfig &cfg) {
    int fd = open(UNDERVOLT_PENDING_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOGW_TAG(UV_TAG, "Cannot create {}: {}", UNDERVOLT_PENDING_FILE, strerror(errno));
        return;
    }
    // Record the margins under test so a crash can be attributed to them
    const std::string content = describe(cfg) + "\n";
    (void)!write(fd, content.data(), content.size());
    fsync(fd);
    close(fd);

    int dir = open(CONFIG_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) {
        fsync(dir);
        close(dir);
    }
}

void clear_pending_marker() {
    unlink(UNDERVOLT_PENDING_FILE);
}

/**
 * Removes the pending marker once the device survived kConfirmSeconds
 * with the current margins, unless newer margins were applied meanwhile.
 */
void schedule_confirm(uint64_t generation) {
    std::thread([generation]() {
        for (int i = 0; i < kConfirmSeconds; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (g_generation.load() != generation) return;
        }
        clear_pending_marker();
        unlink(UNDERVOLT_UNSTABLE_FILE);
        LOGI_TAG(UV_TAG, "Voltage margins confirmed stable");
    }).detach();
}

UndervoltConfig effective_config() {
    if (config_store.get_preferences().disable_tweaks) return UndervoltConfig{};
    return config_store.get_undervolt_config();
}

void apply(const UndervoltConfig &cfg) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (cfg == g_applied) {
        if (is_stock(cfg)) clear_pending_marker();
        return;
    }

    const uint64_t generation = ++g_generation;
    if (is_stock(cfg)) {
        clear_pending_marker();
    } else {
        write_pending_marker(cfg);
    }

    LOGI_TAG(UV_TAG, "Applying margins: {}", describe(cfg));
    if (cfg.cpu_little != g_applied.cpu_little) set_domain_margin(kLittle, cfg.cpu_little);
    if (cfg.cpu_mid != g_applied.cpu_mid) set_domain_margin(kMid, cfg.cpu_mid);
    if (cfg.cpu_big != g_applied.cpu_big) set_domain_margin(kBig, cfg.cpu_big);
    if (cfg.gpu != g_applied.gpu) set_domain_margin(kGpu, cfg.gpu);

    g_applied = cfg;

    if (!is_stock(cfg)) schedule_confirm(generation);
}

} // namespace

bool is_supported() {
    return access(UNDERVOLT_MARGIN_DIR, F_OK) == 0;
}

void init() {
    if (!is_supported()) {
        clear_pending_marker();
        LOGD_TAG(UV_TAG, "{} not found, undervolt unsupported", UNDERVOLT_MARGIN_DIR);
        return;
    }

    if (access(UNDERVOLT_PENDING_FILE, F_OK) == 0) {
        // Previous boot went down before the margins were confirmed stable
        std::string unstable;
        {
            std::ifstream marker(UNDERVOLT_PENDING_FILE);
            std::getline(marker, unstable);
        }
        LOGW_TAG(UV_TAG, "Device rebooted with unconfirmed voltage margins ({}), resetting undervolt",
                 unstable.empty() ? "unknown" : unstable);

        // Keep it around for the WebUI until a new setting is confirmed stable
        if (!unstable.empty()) {
            std::ofstream(UNDERVOLT_UNSTABLE_FILE) << unstable << "\n";
        }

        const std::string values = humanize(unstable);
        notify_with_retry("Undervolt was reset to stock: the device rebooted while testing " +
                          (values.empty() ? std::string("new margins") : values) + ".");
        clear_pending_marker();
        config_store.set_undervolt_config(UndervoltConfig{});
        config_store.save_config();
    }

    apply(effective_config());
}

void on_config_changed() {
    if (!is_supported()) return;
    apply(effective_config());
}

} // namespace Undervolt
