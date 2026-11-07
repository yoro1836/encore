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

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

/**
 * @brief Frame time driven CPU frequency governor (platform independent).
 *
 * The idea, as popularized by fas-rs: aim slightly below the game's frame
 * rate cap. A game that is capped finishes every frame in exactly one cap
 * period, which is a little faster than the lowered goal, so frequency keeps
 * creeping down until frames start to overrun the goal, then comes back up.
 * The loop settles where the game just barely holds its cap, without having
 * to measure how much of each frame was spent working or sleeping.
 *
 * Frequency is one absolute value (kHz) shared by all clusters.
 */
class FasGovernor {
public:
    struct Params {
        std::vector<int> candidates; // ascending target fps candidates
        double margin_fps = 1.0;     // how far below target to aim, per 60 fps
        double kp = 0.0003;          // kHz per ns of normalized frame time error
        long max_freq_khz = 0;       // highest frequency of any cluster
        long min_freq_khz = 0;       // floor for the controlled frequency
    };

    enum class Phase {
        Idle,    // no usable frames, frequency released to the governor
        Warming, // collecting frames for a new target
        Active,  // controlling frequency
    };

    static constexpr uint64_t kNsPerMs = 1'000'000ULL;
    static constexpr uint64_t kNsPerSec = 1'000'000'000ULL;
    static constexpr uint64_t kTargetEvalNs = 100 * kNsPerMs;
    static constexpr uint64_t kWarmupNs = 1 * kNsPerSec;
    static constexpr uint64_t kStallNs = 100 * kNsPerMs;
    static constexpr uint64_t kIdleNs = 1 * kNsPerSec;
    static constexpr size_t kMinFrames = 60;
    // After a frame overruns, don't probe below the frequency it overran at
    // for this long; frequency tables are coarse and a late frame costs a
    // whole vsync, so probing down every frame would drop a frame each cycle.
    static constexpr uint64_t kMissHoldNs = 5 * kNsPerSec;
    static constexpr double kMissThreshold = 1.2;

    explicit FasGovernor(Params params) : p_(std::move(params)), freq_(p_.max_freq_khz) {
        std::sort(p_.candidates.begin(), p_.candidates.end());
    }

    /**
     * @brief Feed one frame (queue to queue time) of the game's main surface.
     */
    void on_frame(uint64_t frametime_ns, uint64_t now) {
        last_frame_at_ = now;
        last_frametime_ns_ = frametime_ns;

        const size_t capacity = static_cast<size_t>(target_ ? target_ : 144) * 5;
        while (frames_.size() >= capacity) {
            frame_sum_ns_ -= frames_.back();
            frames_.pop_back();
        }
        frames_.push_front(frametime_ns);
        frame_sum_ns_ += frametime_ns;

        if (now - last_target_eval_ >= kTargetEvalNs) {
            last_target_eval_ = now;
            evaluate_target(now);
        }

        if (phase_ == Phase::Warming && now >= warm_until_ && frames_.size() >= kMinFrames) {
            phase_ = Phase::Active;
        }

        if (phase_ == Phase::Active) control(frametime_ns, now);
    }

    /**
     * @brief Call periodically (~every 100 ms). Reacts to stalls and idling.
     */
    void on_tick(uint64_t now) {
        if (last_frame_at_ == 0) return;
        const uint64_t since = now - last_frame_at_;

        if (since >= kIdleNs) {
            // Not rendering (menu paused, loading, background): hand back
            if (phase_ != Phase::Idle) reset_to_idle();
            return;
        }

        // A frame is running late: treat the time so far as its frame time
        if (phase_ == Phase::Active && since >= kStallNs) {
            control(std::max(since, last_frametime_ns_), now);
        }
    }

    /**
     * @brief Lowers the goal while the SoC is above its temperature limit.
     */
    void set_thermal_over(bool over) {
        thermal_offset_ = std::clamp(thermal_offset_ + (over ? -0.1 : 0.1), -10.0, 0.0);
    }

    /**
     * @brief Busiest game thread utilization (0..1) and current max cluster
     *        frequency. While not janking, frequency is capped so that thread
     *        would run at about 50% utilization.
     */
    void set_util(std::optional<double> util_max, long cur_freq_max_khz) {
        util_max_ = util_max;
        cur_freq_max_khz_ = cur_freq_max_khz;
    }

    [[nodiscard]] Phase phase() const { return phase_; }
    [[nodiscard]] bool pinning() const { return phase_ == Phase::Active; }
    [[nodiscard]] long freq() const { return freq_; }
    [[nodiscard]] int target_fps() const { return target_; }
    [[nodiscard]] double thermal_offset() const { return thermal_offset_; }

    [[nodiscard]] double fps_long() const {
        if (frames_.empty() || frame_sum_ns_ == 0) return 0;
        return static_cast<double>(frames_.size()) * kNsPerSec / static_cast<double>(frame_sum_ns_);
    }

    [[nodiscard]] bool janked() const {
        return target_ && fps_long() < target_ - 2.0;
    }

    /**
     * @brief The frame rate the controller is actually aiming for.
     */
    [[nodiscard]] double goal_fps() const {
        if (!target_) return 0;
        const double margin = p_.margin_fps * target_ / 60.0;
        return std::max(1.0, target_ + thermal_offset_ - margin);
    }

private:
    /**
     * Smallest candidate the long average fits under (with 3 fps slack),
     * so a game dropping from 60 to 50 still targets 60.
     */
    [[nodiscard]] int match_target() const {
        if (p_.candidates.empty() || frames_.size() < kMinFrames / 2) return 0;
        const double fps = fps_long();
        if (fps < std::max(10, p_.candidates.front() - 10)) return 0;
        for (int c : p_.candidates) {
            if (fps <= c + 3.0) return c;
        }
        return p_.candidates.back();
    }

    void evaluate_target(uint64_t now) {
        const int matched = match_target();
        if (matched == target_) return;

        if (matched == 0) {
            // Too slow for any candidate (menus, loading): only drop out
            // once enough frames confirm it
            if (frames_.size() >= kMinFrames) reset_to_idle();
            return;
        }

        // A game struggling below its cap also matches a lower candidate.
        // While frequency is already near max, assume it is struggling and
        // keep the target; a voluntary lower cap leaves frequency headroom.
        if (target_ && matched < target_ && phase_ == Phase::Active &&
            freq_ >= p_.max_freq_khz * 9 / 10) {
            return;
        }

        target_ = matched;
        frames_.clear();
        frame_sum_ns_ = 0;
        phase_ = Phase::Warming;
        warm_until_ = now + kWarmupNs;
        freq_ = p_.max_freq_khz;
    }

    void control(uint64_t frametime_ns, uint64_t now) {
        const double goal = goal_fps();
        if (goal <= 0) return;

        // 1.0 when the frame took exactly one goal period
        const double normalized = static_cast<double>(frametime_ns) / kNsPerSec * goal;
        const double delta_khz = (normalized - 1.0) * kNsPerSec * p_.kp;

        if (normalized > kMissThreshold) {
            // Remember a little above where it overran
            miss_floor_ = std::max(now < miss_floor_until_ ? miss_floor_ : 0L, freq_ + p_.max_freq_khz * 3 / 100);
            miss_floor_until_ = now + kMissHoldNs;
        }

        long next = freq_ + static_cast<long>(delta_khz);

        if (!janked() && util_max_ && cur_freq_max_khz_ > 0) {
            const long util_cap = static_cast<long>(static_cast<double>(cur_freq_max_khz_) * *util_max_ / 0.5);
            next = std::min(next, util_cap);
        }

        const long floor = now < miss_floor_until_ ? std::max(p_.min_freq_khz, miss_floor_) : p_.min_freq_khz;
        freq_ = std::clamp(next, std::min(floor, p_.max_freq_khz), p_.max_freq_khz);
    }

    void reset_to_idle() {
        miss_floor_until_ = 0;
        phase_ = Phase::Idle;
        target_ = 0;
        frames_.clear();
        frame_sum_ns_ = 0;
        freq_ = p_.max_freq_khz;
    }

    Params p_;
    Phase phase_ = Phase::Idle;
    int target_ = 0;
    long freq_;

    std::deque<uint64_t> frames_; // newest first
    uint64_t frame_sum_ns_ = 0;
    uint64_t last_frame_at_ = 0;
    uint64_t last_frametime_ns_ = 0;
    uint64_t last_target_eval_ = 0;
    uint64_t warm_until_ = 0;

    long miss_floor_ = 0;
    uint64_t miss_floor_until_ = 0;

    double thermal_offset_ = 0;
    std::optional<double> util_max_;
    long cur_freq_max_khz_ = 0;
};
