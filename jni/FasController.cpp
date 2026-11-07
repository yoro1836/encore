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
#include "FasGovernor.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
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

constexpr uint64_t kTickNs = 100 * kNsPerMs;        // stall check, thermal step, core sampling
constexpr uint64_t kUtilIntervalNs = 300 * kNsPerMs; // busiest thread utilization
constexpr uint64_t kSlowIntervalNs = 1 * kNsPerSec;  // thread list, temperature, status

// Default target candidates when the game's cap is auto detected
const std::vector<int> kAutoCandidates = {30, 60, 90, 120, 144};

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

bool write_sysfs(const std::string &path, long value) {
    const std::string s = std::to_string(value);
    int fd = open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = write(fd, s.data(), s.size());
    close(fd);
    return n == static_cast<ssize_t>(s.size());
}

// ---------------------------------------------------------------------------
// CPU frequency
// ---------------------------------------------------------------------------

struct CpuPolicy {
    int id = 0;
    int cluster = 0;
    uint64_t cpu_mask = 0;   // CPUs in this policy
    std::vector<long> freqs; // ascending
    std::string min_node, max_node, cur_node;
    long cur_min = -1, cur_max = -1;

    [[nodiscard]] long lowest() const { return freqs.front(); }
    [[nodiscard]] long highest() const { return freqs.back(); }

    // Lowest table frequency >= khz, clamped to the policy range
    [[nodiscard]] long snap(long khz) const {
        auto it = std::lower_bound(freqs.begin(), freqs.end(), khz);
        return it == freqs.end() ? highest() : *it;
    }
};

uint64_t parse_cpu_list(const fs::path &path) {
    std::ifstream f(path);
    std::string token;
    uint64_t mask = 0;
    while (f >> token) {
        int cpu = -1;
        try {
            cpu = std::stoi(token);
        } catch (...) {
            continue;
        }
        if (cpu >= 0 && cpu < 64) mask |= 1ULL << cpu;
    }
    return mask;
}

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

        const long max_freq = read_long(entry.path() / "cpuinfo_max_freq");
        const long min_freq = read_long(entry.path() / "cpuinfo_min_freq");
        if (max_freq <= 0 || min_freq <= 0) continue;

        std::ifstream avail(entry.path() / "scaling_available_frequencies");
        long f = 0;
        while (avail >> f) {
            if (f >= min_freq && f <= max_freq) p.freqs.push_back(f);
        }
        p.freqs.push_back(min_freq);
        p.freqs.push_back(max_freq);
        std::sort(p.freqs.begin(), p.freqs.end());
        p.freqs.erase(std::unique(p.freqs.begin(), p.freqs.end()), p.freqs.end());

        p.cpu_mask = parse_cpu_list(entry.path() / "related_cpus");
        if (!p.cpu_mask) p.cpu_mask = parse_cpu_list(entry.path() / "affected_cpus");
        p.min_node = (entry.path() / "scaling_min_freq").string();
        p.max_node = (entry.path() / "scaling_max_freq").string();
        p.cur_node = (entry.path() / "scaling_cur_freq").string();
        policies.push_back(std::move(p));
    }

    std::sort(policies.begin(), policies.end(), [](const CpuPolicy &a, const CpuPolicy &b) { return a.id < b.id; });
    for (size_t i = 0; i < policies.size(); i++) policies[i].cluster = static_cast<int>(i);
    return policies;
}

/**
 * Sets scaling_min/max in an order that never makes min > max.
 */
void set_range(CpuPolicy &p, long min_khz, long max_khz) {
    static const bool has_ppm = access("/proc/ppm/policy/hard_userlimit_max_cpu_freq", F_OK) == 0;
    if (min_khz == p.cur_min && max_khz == p.cur_max) return;

    auto write_min = [&]() {
        if (has_ppm) {
            const std::string v = std::to_string(p.cluster) + " " + std::to_string(min_khz);
            write2file("/proc/ppm/policy/hard_userlimit_min_cpu_freq", v);
        }
        write_sysfs(p.min_node, min_khz);
    };
    auto write_max = [&]() {
        if (has_ppm) {
            const std::string v = std::to_string(p.cluster) + " " + std::to_string(max_khz);
            write2file("/proc/ppm/policy/hard_userlimit_max_cpu_freq", v);
        }
        write_sysfs(p.max_node, max_khz);
    };

    if (p.cur_max < 0 || max_khz >= p.cur_max) {
        write_max();
        write_min();
    } else {
        write_min();
        write_max();
    }
    p.cur_min = min_khz;
    p.cur_max = max_khz;
}

/**
 * Pins clusters the game's busy threads run on to freq (min = max) so the
 * frame time feedback acts directly on them; other clusters are only capped.
 */
void apply_freq(std::vector<CpuPolicy> &policies, long khz, uint64_t game_cpus) {
    for (auto &p : policies) {
        const long f = p.snap(std::clamp(khz, p.lowest(), p.highest()));
        const bool critical = !game_cpus || (p.cpu_mask & game_cpus);
        set_range(p, critical ? f : p.lowest(), f);
    }
}

void release_freq(std::vector<CpuPolicy> &policies) {
    for (auto &p : policies) set_range(p, p.lowest(), p.highest());
}

void force_release_freq(std::vector<CpuPolicy> &policies) {
    for (auto &p : policies) {
        p.cur_min = p.cur_max = -1;
    }
    release_freq(policies);
}

long current_freq_max(const std::vector<CpuPolicy> &policies) {
    long m = 0;
    for (const auto &p : policies) m = std::max(m, read_long(p.cur_node, 0));
    return m;
}

// ---------------------------------------------------------------------------
// Game thread monitor
// ---------------------------------------------------------------------------

struct ThreadStat {
    uint64_t ticks = 0; // utime + stime
    int cpu = -1;       // CPU the thread last ran on
};

std::optional<ThreadStat> read_thread_stat(pid_t pid, pid_t tid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stat");
    std::string line;
    if (!std::getline(f, line)) return std::nullopt;

    // comm may contain spaces, fields start after the last ')'
    const auto close = line.rfind(')');
    if (close == std::string::npos) return std::nullopt;
    std::istringstream iss(line.substr(close + 2));
    std::vector<std::string> fields;
    std::string tok;
    while (iss >> tok) fields.push_back(tok);
    // fields[0] is field 3 (state); utime=14, stime=15, processor=39
    if (fields.size() < 37) return std::nullopt;

    try {
        ThreadStat st;
        st.ticks = std::stoull(fields[11]) + std::stoull(fields[12]);
        st.cpu = std::stoi(fields[36]);
        return st;
    } catch (...) {
        return std::nullopt;
    }
}

/**
 * Tracks the game's busiest threads: their peak utilization and which
 * CPUs they run on.
 */
class ThreadMonitor {
public:
    explicit ThreadMonitor(pid_t pid) : pid_(pid), hz_(static_cast<double>(sysconf(_SC_CLK_TCK))) {}

    // Re-rank all threads by CPU time used since the previous scan
    void rescan(uint64_t now) {
        std::vector<std::pair<double, pid_t>> usage;
        std::unordered_map<pid_t, uint64_t> next_ticks;
        const double elapsed = last_scan_ ? static_cast<double>(now - last_scan_) / kNsPerSec : 0;

        std::error_code ec;
        for (const auto &entry : fs::directory_iterator("/proc/" + std::to_string(pid_) + "/task", ec)) {
            pid_t tid = 0;
            try {
                tid = std::stoi(entry.path().filename().string());
            } catch (...) {
                continue;
            }
            auto st = read_thread_stat(pid_, tid);
            if (!st) continue;
            next_ticks[tid] = st->ticks;
            auto prev = all_ticks_.find(tid);
            if (prev != all_ticks_.end() && elapsed > 0) {
                usage.emplace_back(static_cast<double>(st->ticks - prev->second) / (elapsed * hz_), tid);
            }
        }
        all_ticks_ = std::move(next_ticks);
        last_scan_ = now;

        std::sort(usage.begin(), usage.end(), std::greater<>());
        if (usage.size() > 8) usage.resize(8);

        top_.clear();
        for (const auto &[u, tid] : usage) {
            if (u < 0.05) break; // ignore idle threads
            top_[tid] = {all_ticks_[tid], now};
        }
    }

    // Peak utilization of the top threads since the last call
    std::optional<double> util_max(uint64_t now) {
        if (top_.empty()) return std::nullopt;
        double peak = 0;
        for (auto &[tid, tr] : top_) {
            auto st = read_thread_stat(pid_, tid);
            if (!st) continue;
            const double elapsed = static_cast<double>(now - tr.at) / kNsPerSec;
            if (elapsed > 0) peak = std::max(peak, static_cast<double>(st->ticks - tr.ticks) / (elapsed * hz_));
            tr = {st->ticks, now};
        }
        return peak;
    }

    // Accumulate the CPUs top threads were seen on
    void sample_cpus() {
        for (const auto &[tid, tr] : top_) {
            auto st = read_thread_stat(pid_, tid);
            if (st && st->cpu >= 0 && st->cpu < 64) seen_cpus_ |= 1ULL << st->cpu;
        }
    }

    // CPUs seen since the previous call (0 = unknown, treat all as busy)
    uint64_t take_cpus() {
        const uint64_t cpus = seen_cpus_;
        seen_cpus_ = 0;
        return cpus;
    }

private:
    struct Track {
        uint64_t ticks;
        uint64_t at;
    };

    pid_t pid_;
    double hz_;
    uint64_t last_scan_ = 0;
    std::unordered_map<pid_t, uint64_t> all_ticks_;
    std::unordered_map<pid_t, Track> top_;
    uint64_t seen_cpus_ = 0;
};

// ---------------------------------------------------------------------------
// Thermal
// ---------------------------------------------------------------------------

std::vector<std::string> find_cpu_thermal_zones() {
    std::vector<std::string> zones;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/sys/class/thermal", ec)) {
        if (entry.path().filename().string().rfind("thermal_zone", 0) != 0) continue;
        std::ifstream f(entry.path() / "type");
        std::string type;
        if (!(f >> type)) continue;
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });
        if (type.find("cpu") != std::string::npos || type.find("big") != std::string::npos ||
            type.find("mid") != std::string::npos || type.find("little") != std::string::npos) {
            zones.push_back((entry.path() / "temp").string());
        }
    }
    return zones;
}

long max_temp_mc(const std::vector<std::string> &zones) {
    long m = 0;
    for (const auto &z : zones) m = std::max(m, read_long(z, 0));
    return m;
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

const char *phase_name(FasGovernor::Phase phase) {
    switch (phase) {
        case FasGovernor::Phase::Active: return "active";
        case FasGovernor::Phase::Warming: return "warming";
        default: return "idle";
    }
}

struct FasStatus {
    std::string package;
    pid_t pid = 0;
    const char *phase = "idle";
    int target_fps = 0;
    double goal_fps = 0;
    double fps = 0;
    double cpu_cap = 100;
    long freq_mhz = 0;
    long temp_c = 0;
};

void write_status(const std::string &state, const FasStatus *st = nullptr) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << "state=" << state << "\n";
    if (st) {
        oss << "package=" << st->package << "\n"
            << "pid=" << st->pid << "\n"
            << "phase=" << st->phase << "\n"
            << "target_fps=" << st->target_fps << "\n"
            << "goal_fps=" << st->goal_fps << "\n"
            << "fps=" << st->fps << "\n"
            << "cpu_cap=" << st->cpu_cap << "\n"
            << "freq_mhz=" << st->freq_mhz << "\n"
            << "temp_c=" << st->temp_c << "\n";
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

    const std::string lib_path = find_libgui(pid);

    uint64_t dq_off = 0, q_off = 0;
    const bool use_hooks = resolve_hook_offsets(lib_path, dq_off, q_off);
    if (use_hooks) {
        LOGD_TAG(FAS_TAG, "Probing ANativeWindow hooks: queue=0x{:x}", q_off);
    } else {
        LOGW_TAG(FAS_TAG, "ANativeWindow hooks not found in {}, using libfas default symbols", lib_path);
    }

    // Only queue to queue frame time is used, skip the dequeue probes
    // The game may still be starting up, retry attach for a few seconds.
    for (int attempt = 0; attempt < 5 && !stop_requested_.load(); attempt++) {
        fas_target target{};
        target.size = sizeof(target);
        target.pid = pid;
        target.lib_path = lib_path.c_str();
        target.queue_offset = q_off;
        target.mode = FAS_MODE_Q2Q;
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
    LOGI_TAG(FAS_TAG, "Attached to {} (PID: {}) via {} using {}", package, pid, fas_backend_name(info.backend),
             lib_path);

    fas_frame_ctx_t *an = fas_frame_create();
    if (!an) {
        fas_close(f);
        running_.store(false);
        return;
    }
    fas_frame_set_mode(an, info.mode);

    // Governor setup
    long max_khz = 0;
    for (const auto &p : policies) max_khz = std::max(max_khz, p.highest());

    FasGovernor::Params gp;
    gp.candidates = cfg.target_fps > 0 ? std::vector<int>{cfg.target_fps} : kAutoCandidates;
    gp.margin_fps = cfg.margin_fps;
    gp.max_freq_khz = max_khz;
    gp.min_freq_khz = max_khz * std::clamp(cfg.min_freq_percent, 10, 100) / 100;
    FasGovernor gov(gp);

    ThreadMonitor threads(pid);
    const auto thermal_zones = cfg.thermal_limit > 0 ? find_cpu_thermal_zones() : std::vector<std::string>{};
    const long thermal_limit_mc = static_cast<long>(cfg.thermal_limit) * 1000;
    LOGI_TAG(FAS_TAG, "Targets: {}, margin {} fps, floor {} MHz, thermal limit {} ({} zones)",
             cfg.target_fps > 0 ? std::to_string(cfg.target_fps) : std::string("auto"), cfg.margin_fps,
             gp.min_freq_khz / 1000, cfg.thermal_limit > 0 ? std::to_string(cfg.thermal_limit) + "C" : "off",
             thermal_zones.size());

    uint64_t last_tick = 0, last_util = 0, last_slow = 0;
    uint64_t primary_surface = 0;
    uint64_t game_cpus = 0;
    uint32_t frames_this_sec = 0;
    long temp_mc = 0;
    FasGovernor::Phase last_phase = gov.phase();
    std::unordered_map<uint64_t, uint32_t> surface_count;
    std::array<fas_event, 128> ev{};

    force_release_freq(policies);
    threads.rescan(now_ns());

    auto apply = [&]() {
        if (gov.pinning()) {
            apply_freq(policies, gov.freq(), game_cpus);
        } else {
            release_freq(policies);
        }
    };

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        int n = fas_wait(f, 50);
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

                gov.on_frame(fr.q2q_ns, now_ns());
                frames_this_sec++;
            }
            apply();
        }

        const uint64_t now = now_ns();

        if (now - last_tick >= kTickNs) {
            last_tick = now;
            if (!thermal_zones.empty()) gov.set_thermal_over(temp_mc > thermal_limit_mc);
            gov.on_tick(now);
            threads.sample_cpus();
            apply();

            if (gov.phase() != last_phase) {
                LOGD_TAG(FAS_TAG, "Phase {} -> {} (target {} fps)", phase_name(last_phase), phase_name(gov.phase()),
                         gov.target_fps());
                last_phase = gov.phase();
            }
            LOGT_TAG(FAS_TAG, "fps={:.1f} goal={:.1f} freq={} MHz cpus=0x{:x} janked={}", gov.fps_long(),
                     gov.goal_fps(), gov.freq() / 1000, game_cpus, gov.janked());
        }

        if (now - last_util >= kUtilIntervalNs) {
            last_util = now;
            gov.set_util(threads.util_max(now), current_freq_max(policies));
        }

        // Once per second: refresh threads, temperature, surface, status
        if (now - last_slow >= kSlowIntervalNs) {
            const double elapsed = last_slow ? static_cast<double>(now - last_slow) / kNsPerSec : 1.0;
            last_slow = now;

            threads.rescan(now);
            game_cpus = threads.take_cpus();
            if (!thermal_zones.empty()) temp_mc = max_temp_mc(thermal_zones);

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

            FasStatus st;
            st.package = package;
            st.pid = pid;
            st.phase = phase_name(gov.phase());
            st.target_fps = gov.target_fps();
            st.goal_fps = gov.goal_fps();
            st.fps = frames_this_sec / elapsed;
            st.cpu_cap = gov.pinning() ? 100.0 * static_cast<double>(gov.freq()) / static_cast<double>(max_khz) : 100.0;
            st.freq_mhz = gov.pinning() ? gov.freq() / 1000 : 0;
            st.temp_c = temp_mc / 1000;
            write_status("running", &st);
            frames_this_sec = 0;

            if (kill(pid, 0) != 0 && errno == ESRCH) break;
        }
    }

    fas_frame_destroy(an);
    fas_detach(f);
    fas_close(f);
    force_release_freq(policies);
    write_status("idle");
    LOGI_TAG(FAS_TAG, "Detached from {} (PID: {})", package, pid);
    running_.store(false);
}

#endif // ENCORE_HAVE_FAS
