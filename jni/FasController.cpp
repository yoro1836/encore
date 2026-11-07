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

#include "FasController.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#ifdef ENCORE_HAVE_FAS
#include <fas/fas.h>
#include <fas/fas_frame.h>

// Internal libfas ELF symbol resolver (linked statically)
extern "C" int fas_resolve_offset(const char *lib_path, const char *const *symbols, size_t count,
                                  uint64_t *out_offset);
#endif

#include <Encore.hpp>
#include <EncoreLog.hpp>

#include "EncoreConfigStore.hpp"
#include "Write2File.hpp"

#define FAS_TAG "FAS"

#ifndef ENCORE_HAVE_FAS

// FAS is not supported on this architecture
bool FasController::is_available() { return false; }
bool FasController::is_running() const { return false; }
FasController::~FasController() = default;
bool FasController::start(pid_t, const std::string &) { return false; }
void FasController::stop() {}
void FasController::worker(pid_t, std::string) {}

#else

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kNsPerMs = 1'000'000ULL;
constexpr uint64_t kNsPerSec = 1'000'000'000ULL;

// Control loop timing
constexpr uint64_t kControlIntervalNs = 100 * kNsPerMs;
constexpr uint64_t kStatusIntervalNs = 1 * kNsPerSec;
constexpr uint64_t kTargetIntervalNs = 500 * kNsPerMs;
constexpr uint64_t kBoostHoldNs = 1 * kNsPerSec;
constexpr uint64_t kIdleTimeoutNs = 2 * kNsPerSec;

// Level adjustment (level = fraction of max frequency allowed)
constexpr double kStepUpJank = 0.10;
constexpr double kStepUpBusy = 0.03;
constexpr double kStepDown = 0.02;
constexpr double kLoadHigh = 0.85;
constexpr double kLoadLow = 0.65;

// A frame counts as missed when it took longer than this many target periods
constexpr double kMissFactor = 1.5;
constexpr double kHitchFactor = 3.0;

constexpr std::array<int, 11> kStandardFps = {24, 30, 40, 45, 50, 60, 72, 90, 120, 144, 165};
constexpr size_t kHistorySize = 120;

uint64_t now_ns() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * kNsPerSec + static_cast<uint64_t>(ts.tv_nsec);
}

long read_long(const fs::path &path, long fallback = -1) {
    std::ifstream f(path);
    long v = fallback;
    if (!(f >> v)) return fallback;
    return v;
}

bool write_sysfs(const std::string &path, const std::string &value) {
    int fd = open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = write(fd, value.data(), value.size());
    close(fd);
    return n == static_cast<ssize_t>(value.size());
}

struct CpuPolicy {
    int id = 0;
    int cluster = 0;
    std::string max_node;
    std::vector<long> freqs; // ascending
    long max_freq = 0;
    long cur_cap = -1;
};

std::vector<CpuPolicy> load_policies() {
    std::vector<CpuPolicy> policies;
    const fs::path base = "/sys/devices/system/cpu/cpufreq";
    std::error_code ec;

    for (const auto &entry : fs::directory_iterator(base, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("policy", 0) != 0) continue;

        CpuPolicy p;
        try {
            p.id = std::stoi(name.substr(6));
        } catch (...) {
            continue;
        }

        p.max_node = (entry.path() / "scaling_max_freq").string();
        p.max_freq = read_long(entry.path() / "cpuinfo_max_freq");
        if (p.max_freq <= 0) continue;

        std::ifstream avail(entry.path() / "scaling_available_frequencies");
        long f = 0;
        while (avail >> f) p.freqs.push_back(f);
        if (p.freqs.empty()) {
            long min_freq = read_long(entry.path() / "cpuinfo_min_freq", p.max_freq);
            p.freqs = {min_freq, p.max_freq};
        }
        std::sort(p.freqs.begin(), p.freqs.end());
        p.freqs.erase(std::unique(p.freqs.begin(), p.freqs.end()), p.freqs.end());
        policies.push_back(std::move(p));
    }

    std::sort(policies.begin(), policies.end(), [](const CpuPolicy &a, const CpuPolicy &b) { return a.id < b.id; });
    for (size_t i = 0; i < policies.size(); i++) policies[i].cluster = static_cast<int>(i);
    return policies;
}

/**
 * Picks the lowest available frequency that is >= level * max.
 * For policies without a frequency table, interpolates linearly.
 */
long freq_for_level(const CpuPolicy &p, double level) {
    if (level >= 1.0) return p.max_freq;
    const long want = static_cast<long>(static_cast<double>(p.max_freq) * level);

    if (p.freqs.size() <= 2) {
        return std::clamp(want, p.freqs.front(), p.max_freq);
    }

    auto it = std::lower_bound(p.freqs.begin(), p.freqs.end(), want);
    return it == p.freqs.end() ? p.max_freq : *it;
}

void apply_caps(std::vector<CpuPolicy> &policies, double level) {
    static const bool has_ppm = access("/proc/ppm/policy/hard_userlimit_max_cpu_freq", F_OK) == 0;

    for (auto &p : policies) {
        const long cap = freq_for_level(p, level);
        if (cap == p.cur_cap) continue;

        if (has_ppm) {
            write_sysfs("/proc/ppm/policy/hard_userlimit_max_cpu_freq", std::to_string(p.cluster) + " " + std::to_string(cap));
        }
        if (!write_sysfs(p.max_node, std::to_string(cap))) {
            LOGT_TAG(FAS_TAG, "Failed to write {} to {}: {}", cap, p.max_node, strerror(errno));
        }
        p.cur_cap = cap;
    }
}

void restore_caps(std::vector<CpuPolicy> &policies) {
    for (auto &p : policies) p.cur_cap = -1;
    apply_caps(policies, 1.0);
}

/*
 * ANativeWindow entry points. Every EGL/Vulkan frame goes through these,
 * while the Surface methods libfas probes by default can be bypassed:
 * on Android 17 hook_queueBuffer inlines the whole queue path and
 * hook_dequeueBuffer calls the sp<GraphicBuffer> overload, so
 * queueBufferInternal/dequeueBufferInternal only run with interceptors.
 * Argument layout matches the *Internal variants (x0 = window, x1 = buffer).
 */
const char *const kHookDequeue[] = {
    "_ZN7android7Surface18hook_dequeueBufferEP13ANativeWindowPP19ANativeWindowBufferPi",
};
const char *const kHookQueue[] = {
    "_ZN7android7Surface16hook_queueBufferEP13ANativeWindowP19ANativeWindowBufferi",
};

/**
 * Resolves probe offsets on the ANativeWindow hooks. Both must resolve so
 * dequeue and queue events use the same surface key (the ANativeWindow).
 */
bool resolve_hook_offsets(const std::string &lib_path, uint64_t &dq_off, uint64_t &q_off) {
    uint64_t dq = 0, q = 0;
    if (fas_resolve_offset(lib_path.c_str(), kHookDequeue, 1, &dq) != 0) return false;
    if (fas_resolve_offset(lib_path.c_str(), kHookQueue, 1, &q) != 0) return false;
    dq_off = dq;
    q_off = q;
    return true;
}

/**
 * Finds the libgui.so the game actually maps (lib or lib64).
 */
std::string find_libgui(pid_t pid) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.size() < 10 || line.compare(line.size() - 10, 10, "/libgui.so") != 0) continue;
        auto pos = line.find('/');
        if (pos != std::string::npos) return line.substr(pos);
    }
    return FAS_DEFAULT_LIB_PATH;
}

void fas_log_cb(void *arg, int level, const char *msg) {
    (void)arg;
    switch (level) {
        case FAS_LOG_ERROR: LOGE_TAG(FAS_TAG, "libfas: {}", msg); break;
        case FAS_LOG_WARN: LOGW_TAG(FAS_TAG, "libfas: {}", msg); break;
        case FAS_LOG_INFO: LOGI_TAG(FAS_TAG, "libfas: {}", msg); break;
        default: LOGD_TAG(FAS_TAG, "libfas: {}", msg); break;
    }
}

uint64_t percentile(std::vector<uint64_t> v, double q) {
    if (v.empty()) return 0;
    size_t idx = static_cast<size_t>(q * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<long>(idx), v.end());
    return v[idx];
}

/**
 * Estimates the frame rate the game is aiming for from its fastest frames
 * and snaps it to the nearest common refresh/frame cap at or above it.
 */
int estimate_target_fps(const std::vector<uint64_t> &history) {
    if (history.size() < 30) return 0;
    const uint64_t fast = percentile(history, 0.2);
    if (fast == 0) return 0;
    const double fps = static_cast<double>(kNsPerSec) / static_cast<double>(fast);

    for (int std_fps : kStandardFps) {
        if (static_cast<double>(std_fps) >= fps * 0.93) return std_fps;
    }
    return kStandardFps.back();
}

void write_status(const std::string &state, const std::string &pkg = "", pid_t pid = 0, int target_fps = 0,
                  double fps = 0, double level = 1.0, const char *mode = "") {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << "state=" << state << "\n";
    if (state == "running") {
        oss << "package=" << pkg << "\n"
            << "pid=" << pid << "\n"
            << "target_fps=" << target_fps << "\n"
            << "fps=" << fps << "\n"
            << "cpu_cap=" << level * 100.0 << "\n"
            << "mode=" << mode << "\n";
    }
    write2file(FAS_STATUS_FILE, oss.str());
}

} // namespace

bool FasController::is_available() {
    return access(FAS_DEVICE, F_OK) == 0;
}

bool FasController::is_running() const {
    return running_.load(std::memory_order_relaxed);
}

FasController::~FasController() {
    stop();
}

bool FasController::start(pid_t pid, const std::string &package) {
    stop();

    if (!is_available()) {
        LOGW_TAG(FAS_TAG, "{} not found, is encore_fas.ko loaded?", FAS_DEVICE);
        write_status("unavailable");
        return false;
    }

    std::lock_guard<std::mutex> lk(mutex_);
    stop_requested_.store(false);
    running_.store(true);
    thread_ = std::thread(&FasController::worker, this, pid, package);
    return true;
}

void FasController::stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!thread_.joinable()) return;
    stop_requested_.store(true);
    thread_.join();
    running_.store(false);
}

void FasController::worker(pid_t pid, std::string package) {
    pthread_setname_np(pthread_self(), "encore_fas");

    const auto cfg = config_store.get_fas_config();
    const double min_level = std::clamp(cfg.min_freq_percent, 10, 100) / 100.0;

    auto policies = load_policies();
    if (policies.empty()) {
        LOGE_TAG(FAS_TAG, "No cpufreq policies found, FAS disabled");
        write_status("error");
        running_.store(false);
        return;
    }

    char diag[512] = "";
    fas_options opts{};
    opts.size = sizeof(opts);
    opts.backend = FAS_BACKEND_LKM;
    opts.lkm_path = FAS_DEVICE;
    opts.log = fas_log_cb;
    opts.diag = diag;
    opts.diag_size = sizeof(diag);

    fas_t *f = nullptr;
    int rc = fas_open(&opts, &f);
    if (rc < 0) {
        LOGE_TAG(FAS_TAG, "fas_open failed: {} {}", strerror(-rc), diag);
        write_status("error");
        running_.store(false);
        return;
    }

    // The game may still be starting up, retry attach for a few seconds.
    const std::string lib_path = find_libgui(pid);

    uint64_t dq_off = 0, q_off = 0;
    const bool use_hooks = resolve_hook_offsets(lib_path, dq_off, q_off);
    if (use_hooks) {
        LOGD_TAG(FAS_TAG, "Probing ANativeWindow hooks: dequeue=0x{:x} queue=0x{:x}", dq_off, q_off);
    } else {
        LOGW_TAG(FAS_TAG, "ANativeWindow hooks not found in {}, using libfas default symbols", lib_path);
    }

    for (int attempt = 0; attempt < 5 && !stop_requested_.load(); attempt++) {
        fas_target target{};
        target.size = sizeof(target);
        target.pid = pid;
        target.lib_path = lib_path.c_str();
        target.dequeue_offset = dq_off;
        target.queue_offset = q_off;
        target.mode = FAS_MODE_AUTO;
        rc = fas_attach(f, &target);
        if (rc == 0) break;
        LOGW_TAG(FAS_TAG, "fas_attach to {} (PID: {}) failed: {}, retrying", package, pid, strerror(-rc));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (rc != 0) {
        LOGE_TAG(FAS_TAG, "Unable to attach FAS to {}", package);
        fas_close(f);
        write_status("error");
        running_.store(false);
        return;
    }

    fas_info info{};
    fas_get_info(f, &info);
    const char *mode_name = info.mode == FAS_MODE_Q2Q ? "q2q" : "full";
    LOGI_TAG(FAS_TAG, "Attached to {} (PID: {}) via {} using {}, mode={}", package, pid, fas_backend_name(info.backend),
             lib_path, mode_name);

    fas_frame_ctx_t *an = fas_frame_create();
    if (!an) {
        fas_close(f);
        running_.store(false);
        return;
    }
    fas_frame_set_mode(an, info.mode);

    // Controller state
    double level = 1.0;
    int target_fps = cfg.target_fps;
    int pending_target = 0;
    uint64_t hold_until = 0;
    uint64_t last_frame_ns = now_ns();
    uint64_t last_ctrl = last_frame_ns;
    uint64_t last_status = 0;
    uint64_t last_target = 0;
    uint64_t primary_surface = 0;
    uint32_t frames_this_sec = 0;
    double measured_fps = 0;

    std::vector<uint64_t> history;
    history.reserve(kHistorySize);
    size_t history_pos = 0;
    std::vector<uint64_t> win_q2q;
    std::vector<uint64_t> win_work;
    std::unordered_map<uint64_t, uint32_t> surface_count;
    std::array<fas_event, 128> ev{};

    restore_caps(policies);

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        int n = fas_wait(f, 100);
        if (n < 0 && n != -EINTR) {
            LOGW_TAG(FAS_TAG, "fas_wait failed: {}", strerror(-n));
            break;
        }

        if (n > 0) {
            int r = fas_read(f, ev.data(), ev.size());
            if (r == -ENODATA) break;
            for (int i = 0; i < r; i++) {
                fas_frame fr{};
                if (fas_frame_feed(an, &ev[static_cast<size_t>(i)], &fr) != 1) continue;
                if (!(fr.flags & FAS_FRAME_HAS_Q2Q)) continue;

                surface_count[fr.surface]++;
                if (primary_surface != 0 && fr.surface != primary_surface) continue;
                if (fr.flags & FAS_FRAME_RESYNC) continue;
                if (fr.q2q_ns == 0 || fr.q2q_ns > kNsPerSec) continue;

                uint64_t work = fr.q2q_ns;
                if ((fr.flags & FAS_FRAME_HAS_DEQUEUE) && fr.dequeue_wait_ns < fr.q2q_ns) {
                    work = fr.q2q_ns - fr.dequeue_wait_ns;
                }

                win_q2q.push_back(fr.q2q_ns);
                win_work.push_back(work);
                if (history.size() < kHistorySize) {
                    history.push_back(fr.q2q_ns);
                } else {
                    history[history_pos] = fr.q2q_ns;
                    history_pos = (history_pos + 1) % kHistorySize;
                }
                frames_this_sec++;
                last_frame_ns = now_ns();
            }
        }

        const uint64_t now = now_ns();

        // Re-estimate target frame rate (only in auto mode)
        if (cfg.target_fps == 0 && now - last_target >= kTargetIntervalNs) {
            last_target = now;
            int est = estimate_target_fps(history);
            if (est != 0 && est != target_fps) {
                // Require two consecutive agreeing estimates to avoid flapping
                if (est == pending_target) {
                    LOGD_TAG(FAS_TAG, "Target FPS {} -> {}", target_fps, est);
                    target_fps = est;
                    pending_target = 0;
                } else {
                    pending_target = est;
                }
            }
        }

        // Control step
        if (now - last_ctrl >= kControlIntervalNs) {
            last_ctrl = now;

            if (win_q2q.size() < 2 || target_fps == 0) {
                // Not rendering (loading, paused) or target unknown: uncap and let governor decide
                if (now - last_frame_ns > kIdleTimeoutNs || target_fps == 0) level = 1.0;
            } else {
                const double period = static_cast<double>(kNsPerSec) / target_fps;
                size_t misses = 0;
                bool hitch = false;
                double work_sum = 0;
                for (size_t i = 0; i < win_q2q.size(); i++) {
                    const double q2q = static_cast<double>(win_q2q[i]);
                    if (q2q > period * kMissFactor) misses++;
                    if (q2q > period * kHitchFactor) hitch = true;
                    work_sum += static_cast<double>(win_work[i]);
                }
                const double load = work_sum / static_cast<double>(win_work.size()) / period;

                if (hitch || misses * 10 > win_q2q.size()) {
                    level += kStepUpJank;
                    hold_until = now + kBoostHoldNs;
                } else if (load > kLoadHigh) {
                    level += kStepUpBusy;
                } else if (load < kLoadLow && now >= hold_until) {
                    level -= kStepDown;
                }
                level = std::clamp(level, min_level, 1.0);

                LOGT_TAG(FAS_TAG, "frames={} misses={} load={:.2f} level={:.2f}", win_q2q.size(), misses, load, level);
            }

            apply_caps(policies, level);
            win_q2q.clear();
            win_work.clear();
        }

        // Once per second: pick primary surface, publish status, check process
        if (now - last_status >= kStatusIntervalNs) {
            const double elapsed = last_status ? static_cast<double>(now - last_status) / kNsPerSec : 1.0;
            last_status = now;
            measured_fps = frames_this_sec / elapsed;
            frames_this_sec = 0;

            uint64_t best = 0;
            uint32_t best_count = 0;
            for (const auto &[surface, count] : surface_count) {
                if (count > best_count) {
                    best = surface;
                    best_count = count;
                }
            }
            if (best_count > 0) primary_surface = best;
            surface_count.clear();

            write_status("running", package, pid, target_fps, measured_fps, level, mode_name);

            if (kill(pid, 0) != 0 && errno == ESRCH) break;
        }
    }

    fas_frame_destroy(an);
    fas_detach(f);
    fas_close(f);
    restore_caps(policies);
    write_status("idle");
    LOGI_TAG(FAS_TAG, "Detached from {} (PID: {})", package, pid);
    running_.store(false);
}

#endif // ENCORE_HAVE_FAS
