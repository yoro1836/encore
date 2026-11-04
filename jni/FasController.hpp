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

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <sys/types.h>

/**
 * @brief Frame Aware Scheduling controller.
 *
 * Attaches to the foreground game through the encore_fas kernel module
 * (/dev/encore_fas), measures per-frame timings from libgui's
 * dequeueBuffer/queueBuffer and caps CPU max frequency per cluster to the
 * lowest level that still keeps the game at its target frame rate.
 *
 * The CPU governor stays in charge below the cap, so FAS only removes
 * headroom the game does not need instead of pinning CPUs at max.
 */
class FasController {
public:
    static FasController &get() {
        static FasController instance;
        return instance;
    }

    /**
     * @brief Whether the encore_fas kernel module is loaded.
     */
    [[nodiscard]] static bool is_available();

    /**
     * @brief Start controlling a game. Stops any previous session first.
     * @return false if FAS is unavailable.
     */
    bool start(pid_t pid, const std::string &package);

    /**
     * @brief Stop the current session and restore CPU frequency caps.
     */
    void stop();

    [[nodiscard]] bool is_running() const;

private:
    FasController() = default;
    ~FasController();

    FasController(const FasController &) = delete;
    FasController &operator=(const FasController &) = delete;

    void worker(pid_t pid, std::string package);

    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
};

#define fas_controller FasController::get()
