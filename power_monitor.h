/**
 * @file power_monitor.h
 * @brief Header-only C++ Multi-Device Power Monitoring Library
 *
 * C++ port of the Zeus project's full measurement functionality.
 * Reference: https://github.com/ml-energy/zeus
 *
 * Supported devices (compile-time selectable):
 *   - NVIDIA GPU (NVML)     — requires CUDA Toolkit; disable with -DZEUS_NO_NVML
 *   - AMD GPU (ROCm SMI)    — enable with -DZEUS_USE_ROCM_SMI
 *   - Intel CPU/DRAM (RAPL) — Linux only, auto-detected via sysfs
 *
 * Key Python source files this is based on:
 *   - zeus/monitor/energy.py    : ZeusMonitor class (begin_window / end_window)
 *   - zeus/device/gpu/nvidia.py : NVML API wrappers
 *   - zeus/device/gpu/amd.py    : AMD SMI API wrappers
 *   - zeus/device/cpu/rapl.py   : Intel RAPL sysfs wrappers
 *   - zeus/monitor/power.py     : Background power polling for pre-Volta GPUs
 */

#pragma once

// ---------------------------------------------------------------------------
// Standard headers
// ---------------------------------------------------------------------------
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Auto-detect NVIDIA NVML
// ---------------------------------------------------------------------------
#ifndef ZEUS_NO_NVML
  #if defined(__has_include)
    #if __has_include(<nvml.h>)
      #include <nvml.h>
      #define ZEUS_HAS_NVML 1
    #endif
  #else
    // Assume available if not explicitly disabled
    #include <nvml.h>
    #define ZEUS_HAS_NVML 1
  #endif
#endif

// ---------------------------------------------------------------------------
// AMD ROCm SMI (opt-in via -DZEUS_USE_ROCM_SMI)
// ---------------------------------------------------------------------------
#ifdef ZEUS_USE_ROCM_SMI
  #if defined(__has_include)
    #if __has_include(<rocm_smi/rocm_smi.h>)
      #include <rocm_smi/rocm_smi.h>
      #define ZEUS_HAS_ROCM_SMI 1
    #endif
  #else
    #include <rocm_smi/rocm_smi.h>
    #define ZEUS_HAS_ROCM_SMI 1
  #endif
#endif

// ---------------------------------------------------------------------------
// Linux sysfs (RAPL for CPU, INA3221 for Jetson)
// ---------------------------------------------------------------------------
#ifdef __linux__
  #include <dirent.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #define ZEUS_HAS_RAPL 1
#endif

namespace zeus {

// ============================================================================
// Error Helpers
// ============================================================================

#ifdef ZEUS_HAS_NVML
/**
 * @brief Check NVML return code and throw on error.
 */
inline void nvml_check(nvmlReturn_t result, const std::string& context = "") {
    if (result != NVML_SUCCESS) {
        std::string msg = "NVML Error: ";
        msg += nvmlErrorString(result);
        if (!context.empty()) msg += " [" + context + "]";
        throw std::runtime_error(msg);
    }
}
#endif

#ifdef ZEUS_HAS_ROCM_SMI
/**
 * @brief Check ROCm SMI return code and throw on error.
 */
inline void rsmi_check(rsmi_status_t result, const std::string& context = "") {
    if (result != RSMI_STATUS_SUCCESS) {
        const char* err_str = nullptr;
        rsmi_status_string(result, &err_str);
        std::string msg = "ROCm SMI Error: ";
        msg += (err_str ? err_str : "unknown");
        if (!context.empty()) msg += " [" + context + "]";
        throw std::runtime_error(msg);
    }
}
#endif

// ============================================================================
// NVML Lifecycle (RAII Singleton)
// ============================================================================

#ifdef ZEUS_HAS_NVML
/**
 * @brief RAII wrapper for NVML initialization/shutdown.
 */
class NvmlContext {
public:
    static NvmlContext& instance() {
        static NvmlContext ctx;
        return ctx;
    }
    NvmlContext(const NvmlContext&) = delete;
    NvmlContext& operator=(const NvmlContext&) = delete;
private:
    NvmlContext()  { nvml_check(nvmlInit_v2(), "nvmlInit_v2"); }
    ~NvmlContext() { nvmlShutdown(); }
};
#endif

// ============================================================================
// ROCm SMI Lifecycle (RAII Singleton)
// ============================================================================

#ifdef ZEUS_HAS_ROCM_SMI
class RsmiContext {
public:
    static RsmiContext& instance() {
        static RsmiContext ctx;
        return ctx;
    }
    RsmiContext(const RsmiContext&) = delete;
    RsmiContext& operator=(const RsmiContext&) = delete;
private:
    RsmiContext()  { rsmi_check(rsmi_init(0), "rsmi_init"); }
    ~RsmiContext() { rsmi_shut_down(); }
};
#endif

// ============================================================================
// Measurement Result (Multi-Device)
// ============================================================================

/**
 * @brief Result of a power measurement window.
 *
 * Corresponds to zeus.monitor.energy.Measurement in the Python version.
 * Contains per-GPU, per-CPU, DRAM, and SoC energy consumption in Joules.
 */
struct Measurement {
    /** GPU index -> energy consumed in Joules */
    std::map<int, double> gpu_energy;

    /** CPU socket index -> energy consumed in Joules (RAPL package) */
    std::map<int, double> cpu_energy;

    /** CPU socket index -> DRAM energy in Joules (RAPL dram sub-package) */
    std::map<int, double> dram_energy;

    /** SoC metric name -> energy in Joules (Jetson INA3221 / Apple Silicon) */
    std::map<std::string, double> soc_energy;

    /** Wall-clock time of the measurement window (seconds). */
    double elapsed_time = 0.0;

    /** Total energy across ALL device types (Joules). */
    double total_energy() const {
        return total_gpu_energy() + total_cpu_energy()
             + total_dram_energy() + total_soc_energy();
    }

    double total_gpu_energy() const {
        double t = 0.0;
        for (const auto& kv : gpu_energy) t += kv.second;
        return t;
    }

    double total_cpu_energy() const {
        double t = 0.0;
        for (const auto& kv : cpu_energy) t += kv.second;
        return t;
    }

    double total_dram_energy() const {
        double t = 0.0;
        for (const auto& kv : dram_energy) t += kv.second;
        return t;
    }

    double total_soc_energy() const {
        double t = 0.0;
        for (const auto& kv : soc_energy) t += kv.second;
        return t;
    }
};

// ============================================================================
// RAPL CPU Backend (Linux only)
// ============================================================================

#ifdef ZEUS_HAS_RAPL
/**
 * @brief Intel RAPL CPU/DRAM backend via Linux sysfs.
 *
 * Reference: zeus/device/cpu/rapl.py
 *
 * Reads cumulative energy from:
 *   /sys/class/powercap/intel-rapl/intel-rapl:{cpu}/energy_uj
 *   /sys/class/powercap/intel-rapl/intel-rapl:{cpu}/intel-rapl:{cpu}:{sub}/energy_uj
 */
class RaplBackend {
public:
    struct Snapshot {
        std::map<int, double> cpu;   ///< CPU socket -> Joules
        std::map<int, double> dram;  ///< CPU socket -> Joules
    };

    RaplBackend() {
        discover_cpus("/sys/class/powercap/intel-rapl");
        if (cpu_dirs_.empty()) {
            // Try Zeus container mount point
            discover_cpus("/zeus_sys/class/powercap/intel-rapl");
        }
    }

    bool available() const { return !cpu_dirs_.empty(); }

    std::vector<int> cpu_indices() const {
        std::vector<int> indices;
        indices.reserve(cpu_dirs_.size());
        for (const auto& kv : cpu_dirs_) indices.push_back(kv.first);
        return indices;
    }

    bool supports_dram(int cpu_index) const {
        return dram_dirs_.count(cpu_index) > 0;
    }

    double read_cpu_energy_j(int cpu_index) const {
        return read_energy_uj(cpu_dirs_.at(cpu_index) + "/energy_uj") / 1e6;
    }

    double read_dram_energy_j(int cpu_index) const {
        auto it = dram_dirs_.find(cpu_index);
        if (it == dram_dirs_.end()) return 0.0;
        return read_energy_uj(it->second + "/energy_uj") / 1e6;
    }

    Snapshot take_snapshot() const {
        Snapshot snap;
        for (const auto& kv : cpu_dirs_) {
            snap.cpu[kv.first] = read_cpu_energy_j(kv.first);
            if (supports_dram(kv.first)) {
                snap.dram[kv.first] = read_dram_energy_j(kv.first);
            }
        }
        return snap;
    }

private:
    std::map<int, std::string> cpu_dirs_;   ///< cpu_idx -> sysfs path
    std::map<int, std::string> dram_dirs_;  ///< cpu_idx -> dram sysfs path

    void discover_cpus(const std::string& base) {
        DIR* dir = opendir(base.c_str());
        if (!dir) return;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            // Match "intel-rapl:N" (not sub-packages "intel-rapl:N:M")
            if (name.find("intel-rapl:") != 0) continue;
            std::string suffix = name.substr(11);
            if (suffix.find(':') != std::string::npos) continue;

            int cpu_idx;
            try { cpu_idx = std::stoi(suffix); }
            catch (...) { continue; }

            std::string cpu_path = base + "/" + name;
            std::string energy_file = cpu_path + "/energy_uj";

            struct stat st;
            if (stat(energy_file.c_str(), &st) != 0) continue;

            cpu_dirs_[cpu_idx] = cpu_path;

            // Search for DRAM sub-package
            DIR* sub_dir = opendir(cpu_path.c_str());
            if (!sub_dir) continue;

            struct dirent* sub_entry;
            while ((sub_entry = readdir(sub_dir)) != nullptr) {
                std::string sub_name = sub_entry->d_name;
                if (sub_name.find("intel-rapl:") != 0) continue;
                // Sub-packages have format intel-rapl:N:M
                if (sub_name.find(':', 11) == std::string::npos) continue;

                std::string sub_path = cpu_path + "/" + sub_name;
                std::string name_file = sub_path + "/name";
                std::ifstream nf(name_file);
                if (nf.is_open()) {
                    std::string pkg_name;
                    std::getline(nf, pkg_name);
                    // Trim whitespace
                    while (!pkg_name.empty()
                           && (pkg_name.back() == '\n'
                               || pkg_name.back() == '\r'
                               || pkg_name.back() == ' '))
                        pkg_name.pop_back();
                    if (pkg_name == "dram") {
                        dram_dirs_[cpu_idx] = sub_path;
                        break;
                    }
                }
            }
            closedir(sub_dir);
        }
        closedir(dir);
    }

    static double read_energy_uj(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error("Cannot read RAPL energy: " + path);
        }
        unsigned long long energy_uj = 0;
        f >> energy_uj;
        return static_cast<double>(energy_uj);
    }
};
#endif // ZEUS_HAS_RAPL

// ============================================================================
// PowerMonitor - Main Library Class (Multi-Device)
// ============================================================================

/**
 * @brief Multi-device power/energy monitor.
 *
 * C++ equivalent of zeus.monitor.energy.ZeusMonitor (Python).
 *
 * Auto-detects and uses available backends at compile time:
 *   - NVIDIA GPU (NVML)     — default if CUDA Toolkit is present
 *   - AMD GPU (ROCm SMI)    — enabled with ZEUS_USE_ROCM_SMI
 *   - Intel CPU/DRAM (RAPL) — automatic on Linux
 *
 * Usage:
 * @code
 *   zeus::PowerMonitor monitor({0});            // Monitor GPU 0 + all CPUs
 *   monitor.begin_window("train");              // Start measurement
 *   // ... workload ...
 *   auto result = monitor.end_window("train");  // Stop & get result
 *   std::cout << "GPU: " << result.total_gpu_energy() << " J\n";
 *   std::cout << "CPU: " << result.total_cpu_energy() << " J\n";
 * @endcode
 */
class PowerMonitor {
public:
    // ---- Configuration ----

    /**
     * @brief Configuration for PowerMonitor construction.
     */
    struct Config {
        std::vector<int> gpu_indices = {};     ///< Empty = all GPUs
        std::vector<int> cpu_indices = {};     ///< Empty = all CPUs (RAPL)
        bool monitor_gpu = true;
        bool monitor_cpu = true;
        double polling_interval_s = 0.1;       ///< Pre-Volta polling rate
    };

    // ---- Construction / Destruction ----

    /**
     * @brief Construct a PowerMonitor (original simple interface).
     * @param gpu_indices GPU indices to monitor. Empty = all GPUs.
     * @param polling_interval_s Polling interval for pre-Volta GPUs (seconds).
     *
     * Also auto-enables CPU (RAPL) monitoring on Linux.
     */
    explicit PowerMonitor(std::vector<int> gpu_indices = {},
                          double polling_interval_s = 0.1)
        : polling_active_(false)
        , polling_interval_(polling_interval_s)
    {
        init_gpu_backend(gpu_indices);
        init_cpu_backend({});
    }

    /**
     * @brief Construct a PowerMonitor from Config.
     * @param cfg Configuration struct.
     */
    explicit PowerMonitor(const Config& cfg)
        : polling_active_(false)
        , polling_interval_(cfg.polling_interval_s)
    {
        if (cfg.monitor_gpu) init_gpu_backend(cfg.gpu_indices);
        if (cfg.monitor_cpu) init_cpu_backend(cfg.cpu_indices);
    }

    ~PowerMonitor() {
        stop_polling();
    }

    // Non-copyable
    PowerMonitor(const PowerMonitor&) = delete;
    PowerMonitor& operator=(const PowerMonitor&) = delete;

    // ---- Main Measurement API ----

    /**
     * @brief Mark the beginning of a measurement window.
     *
     * Snapshots energy counters across all detected device types.
     *
     * @param key Unique name for this measurement window.
     */
    void begin_window(const std::string& key) {
        if (windows_.count(key)) {
            throw std::runtime_error(
                "Window '" + key + "' already active. "
                "Call end_window(\"" + key + "\") first.");
        }

        WindowState state;
        state.start_time = get_timestamp();

        // GPU snapshot
#ifdef ZEUS_HAS_NVML
        if (has_nvml_) {
            for (int idx : nvml_gpu_indices_) {
                if (nvml_supports_energy_[idx]) {
                    state.nvml_start_energy[idx] =
                        get_nvml_total_energy_mj(idx) / 1000.0;  // mJ -> J
                }
            }
        }
#endif
#ifdef ZEUS_HAS_ROCM_SMI
        if (has_rocm_) {
            for (int idx : rocm_gpu_indices_) {
                state.rocm_start_energy[idx] = get_rocm_total_energy_j(idx);
            }
        }
#endif

        // CPU snapshot
#ifdef ZEUS_HAS_RAPL
        if (rapl_ && rapl_->available()) {
            state.rapl_snapshot = rapl_->take_snapshot();
        }
#endif

        windows_[key] = std::move(state);
    }

    /**
     * @brief Mark the end of a measurement window and return the result.
     *
     * Computes energy deltas across all detected device types.
     *
     * @param key Name of the window started with begin_window().
     * @return Measurement result with per-device energy in Joules.
     */
    Measurement end_window(const std::string& key) {
        auto it = windows_.find(key);
        if (it == windows_.end()) {
            throw std::runtime_error(
                "Window '" + key + "' not found. "
                "Call begin_window(\"" + key + "\") first.");
        }

        double end_time = get_timestamp();
        WindowState& state = it->second;
        Measurement result;
        result.elapsed_time = end_time - state.start_time;

        // === GPU energy ===
#ifdef ZEUS_HAS_NVML
        if (has_nvml_) {
            for (int idx : nvml_gpu_indices_) {
                if (nvml_supports_energy_[idx]) {
                    // PATH A: Volta+ hardware energy counter
                    double end_e = get_nvml_total_energy_mj(idx) / 1000.0;
                    result.gpu_energy[idx] =
                        end_e - state.nvml_start_energy[idx];
                } else {
                    // PATH B: Pre-Volta trapezoidal integration
                    double energy = compute_energy_from_samples(
                        idx, state.start_time, end_time);
                    if (energy <= 0.0) {
                        double power_w = get_nvml_instant_power_w(idx);
                        energy = power_w * result.elapsed_time;
                    }
                    result.gpu_energy[idx] = energy;
                }
            }
        }
#endif
#ifdef ZEUS_HAS_ROCM_SMI
        if (has_rocm_) {
            for (int idx : rocm_gpu_indices_) {
                double end_e = get_rocm_total_energy_j(idx);
                result.gpu_energy[idx] =
                    end_e - state.rocm_start_energy[idx];
            }
        }
#endif

        // === CPU / DRAM energy ===
#ifdef ZEUS_HAS_RAPL
        if (rapl_ && rapl_->available()) {
            auto end_snap = rapl_->take_snapshot();
            for (const auto& kv : end_snap.cpu) {
                result.cpu_energy[kv.first] =
                    kv.second - state.rapl_snapshot.cpu[kv.first];
            }
            for (const auto& kv : end_snap.dram) {
                result.dram_energy[kv.first] =
                    kv.second - state.rapl_snapshot.dram[kv.first];
            }
        }
#endif

        windows_.erase(it);
        return result;
    }

    // ---- Power / Energy Query Functions ----

    /**
     * @brief Get instantaneous GPU power draw (Watts).
     *
     * Uses nvmlDeviceGetPowerUsage (NVIDIA) or rsmi_dev_power_ave_get (AMD).
     */
    double get_instant_power(int gpu_index) const {
#ifdef ZEUS_HAS_NVML
        if (has_nvml_) return get_nvml_instant_power_w(gpu_index);
#endif
#ifdef ZEUS_HAS_ROCM_SMI
        if (has_rocm_) return get_rocm_power_w(gpu_index);
#endif
        throw std::runtime_error("No GPU backend available");
    }

    /**
     * @brief Get cumulative total energy (Joules) since driver load.
     *
     * NVIDIA Volta+ only (nvmlDeviceGetTotalEnergyConsumption).
     */
    double get_total_energy(int gpu_index) const {
#ifdef ZEUS_HAS_NVML
        if (has_nvml_) return get_nvml_total_energy_mj(gpu_index) / 1000.0;
#endif
        throw std::runtime_error(
            "Total energy counter only available on NVIDIA Volta+ GPUs");
    }

    /** @brief Check if a GPU supports the hardware energy counter. */
    bool supports_energy_counter(int gpu_index) const {
#ifdef ZEUS_HAS_NVML
        if (has_nvml_) {
            auto it = nvml_supports_energy_.find(gpu_index);
            return it != nvml_supports_energy_.end() && it->second;
        }
#endif
        return false;
    }

    /**
     * @brief Get the power management limit for a GPU (Watts).
     *
     * NVIDIA only (nvmlDeviceGetPowerManagementLimit).
     */
    double get_power_limit(int gpu_index) const {
#ifdef ZEUS_HAS_NVML
        if (has_nvml_) {
            auto it = nvml_handles_.find(gpu_index);
            if (it == nvml_handles_.end())
                throw std::runtime_error("GPU not monitored");
            unsigned int limit_mw = 0;
            nvml_check(nvmlDeviceGetPowerManagementLimit(
                it->second, &limit_mw));
            return static_cast<double>(limit_mw) / 1000.0;
        }
#endif
        throw std::runtime_error(
            "Power limit query only available on NVIDIA GPUs");
    }

    // ---- Device Info ----

    /** @brief Whether any GPU backend is active. */
    bool has_gpu() const {
        bool result = false;
#ifdef ZEUS_HAS_NVML
        if (has_nvml_) result = true;
#endif
#ifdef ZEUS_HAS_ROCM_SMI
        if (has_rocm_) result = true;
#endif
        return result;
    }

    /** @brief Whether CPU (RAPL) backend is active. */
    bool has_cpu() const {
#ifdef ZEUS_HAS_RAPL
        return rapl_ && rapl_->available();
#else
        return false;
#endif
    }

    /** @brief GPU backend type string ("NVIDIA", "AMD", or "None"). */
    std::string gpu_type() const {
#ifdef ZEUS_HAS_NVML
        if (has_nvml_) return "NVIDIA";
#endif
#ifdef ZEUS_HAS_ROCM_SMI
        if (has_rocm_) return "AMD";
#endif
        return "None";
    }

    /** @brief List of monitored GPU indices. */
    const std::vector<int>& gpu_indices() const {
#ifdef ZEUS_HAS_NVML
        if (has_nvml_) return nvml_gpu_indices_;
#endif
#ifdef ZEUS_HAS_ROCM_SMI
        if (has_rocm_) return rocm_gpu_indices_;
#endif
        static const std::vector<int> empty;
        return empty;
    }

    /** @brief List of monitored CPU socket indices (RAPL). */
    std::vector<int> cpu_indices() const {
#ifdef ZEUS_HAS_RAPL
        if (rapl_) return rapl_->cpu_indices();
#endif
        return {};
    }

    // ---- Static Utility Functions ----

    /** @brief Get the number of NVIDIA GPUs. */
    static int get_nvml_device_count() {
#ifdef ZEUS_HAS_NVML
        NvmlContext::instance();
        unsigned int count = 0;
        nvml_check(nvmlDeviceGetCount(&count));
        return static_cast<int>(count);
#else
        return 0;
#endif
    }

    /** @brief Get total GPU count (NVIDIA + AMD). */
    static int get_device_count() {
        int count = 0;
#ifdef ZEUS_HAS_NVML
        try { count += get_nvml_device_count(); } catch (...) {}
#endif
#ifdef ZEUS_HAS_ROCM_SMI
        try {
            RsmiContext::instance();
            uint32_t n = 0;
            rsmi_num_monitor_devices(&n);
            count += static_cast<int>(n);
        } catch (...) {}
#endif
        return count;
    }

    /** @brief Get the name of a GPU by index. */
    static std::string get_device_name(int gpu_index) {
#ifdef ZEUS_HAS_NVML
        try {
            NvmlContext::instance();
            nvmlDevice_t handle;
            nvml_check(nvmlDeviceGetHandleByIndex(
                static_cast<unsigned>(gpu_index), &handle));
            char name[256];
            nvml_check(nvmlDeviceGetName(handle, name, sizeof(name)));
            return std::string(name);
        } catch (...) {}
#endif
#ifdef ZEUS_HAS_ROCM_SMI
        try {
            RsmiContext::instance();
            char name[256];
            rsmi_check(rsmi_dev_name_get(gpu_index, name, sizeof(name)));
            return std::string(name);
        } catch (...) {}
#endif
        return "Unknown GPU " + std::to_string(gpu_index);
    }

    /**
     * @brief Get the architecture name of a GPU.
     *
     * NVIDIA only. Returns "Unknown" for AMD GPUs.
     */
    static std::string get_architecture_name(int gpu_index) {
#ifdef ZEUS_HAS_NVML
        try {
            NvmlContext::instance();
            nvmlDevice_t handle;
            nvml_check(nvmlDeviceGetHandleByIndex(
                static_cast<unsigned>(gpu_index), &handle));
            nvmlDeviceArchitecture_t arch;
            nvml_check(nvmlDeviceGetArchitecture(handle, &arch));
            return architecture_to_string(arch);
        } catch (...) {}
#endif
        return "Unknown";
    }

private:
    // ---- Internal Types ----

    struct PowerSample {
        double timestamp;  ///< seconds (monotonic)
        int    gpu_index;
        double power_w;    ///< Watts
    };

    struct WindowState {
        double start_time = 0.0;  ///< monotonic timestamp
#ifdef ZEUS_HAS_NVML
        std::map<int, double> nvml_start_energy;  ///< Joules (Volta+)
#endif
#ifdef ZEUS_HAS_ROCM_SMI
        std::map<int, double> rocm_start_energy;  ///< Joules
#endif
#ifdef ZEUS_HAS_RAPL
        RaplBackend::Snapshot rapl_snapshot;
#endif
    };

    // ---- State ----

    // NVML
#ifdef ZEUS_HAS_NVML
    bool has_nvml_ = false;
    std::vector<int> nvml_gpu_indices_;
    std::map<int, nvmlDevice_t> nvml_handles_;
    std::map<int, bool> nvml_supports_energy_;
#endif

    // ROCm SMI
#ifdef ZEUS_HAS_ROCM_SMI
    bool has_rocm_ = false;
    std::vector<int> rocm_gpu_indices_;
#endif

    // RAPL
#ifdef ZEUS_HAS_RAPL
    std::unique_ptr<RaplBackend> rapl_;
#endif

    // Window state
    std::map<std::string, WindowState> windows_;

    // Background polling (pre-Volta NVML)
    std::thread polling_thread_;
    std::atomic<bool> polling_active_;
    std::mutex samples_mutex_;
    std::vector<PowerSample> samples_;
    double polling_interval_;

    // ---- Initialization ----

    void init_gpu_backend(const std::vector<int>& gpu_indices) {
#ifdef ZEUS_HAS_NVML
        try {
            NvmlContext::instance();
            unsigned int count = 0;
            nvml_check(nvmlDeviceGetCount(&count));

            std::vector<int> indices = gpu_indices;
            if (indices.empty()) {
                for (unsigned i = 0; i < count; ++i)
                    indices.push_back(static_cast<int>(i));
            }

            bool need_polling = false;
            for (int idx : indices) {
                if (idx < 0 || static_cast<unsigned>(idx) >= count) {
                    throw std::runtime_error(
                        "GPU " + std::to_string(idx) + " out of range [0, "
                        + std::to_string(count) + ")");
                }
                nvmlDevice_t handle;
                nvml_check(nvmlDeviceGetHandleByIndex(
                    static_cast<unsigned>(idx), &handle));
                nvml_handles_[idx] = handle;

                // Volta (arch=5) and newer support energy counter
                nvmlDeviceArchitecture_t arch;
                bool supports =
                    (nvmlDeviceGetArchitecture(handle, &arch) == NVML_SUCCESS)
                    && (static_cast<unsigned>(arch) >= 5);
                nvml_supports_energy_[idx] = supports;
                if (!supports) need_polling = true;
            }
            nvml_gpu_indices_ = std::move(indices);
            has_nvml_ = true;
            if (need_polling) start_polling();
        } catch (const std::exception&) {
            has_nvml_ = false;
        }
#endif

#ifdef ZEUS_HAS_ROCM_SMI
        // Only try AMD if NVIDIA was not found
        bool try_amd = true;
#ifdef ZEUS_HAS_NVML
        try_amd = !has_nvml_;
#endif
        if (try_amd) {
            try {
                RsmiContext::instance();
                uint32_t count = 0;
                rsmi_num_monitor_devices(&count);

                std::vector<int> indices = gpu_indices;
                if (indices.empty()) {
                    for (uint32_t i = 0; i < count; ++i)
                        indices.push_back(static_cast<int>(i));
                }
                rocm_gpu_indices_ = std::move(indices);
                has_rocm_ = true;
            } catch (...) {
                has_rocm_ = false;
            }
        }
#endif
        // Suppress unused parameter warning when no GPU backend compiled
        (void)gpu_indices;
    }

    void init_cpu_backend(const std::vector<int>& cpu_indices) {
#ifdef ZEUS_HAS_RAPL
        rapl_ = std::make_unique<RaplBackend>();
        // Future: filter to specific cpu_indices if needed
#endif
        (void)cpu_indices;
    }

    // ---- NVML Wrappers ----

#ifdef ZEUS_HAS_NVML
    double get_nvml_total_energy_mj(int gpu_index) const {
        auto it = nvml_handles_.find(gpu_index);
        if (it == nvml_handles_.end())
            throw std::runtime_error(
                "GPU " + std::to_string(gpu_index) + " not monitored");
        unsigned long long energy_mj = 0;
        nvml_check(nvmlDeviceGetTotalEnergyConsumption(
            it->second, &energy_mj),
            "nvmlDeviceGetTotalEnergyConsumption");
        return static_cast<double>(energy_mj);
    }

    double get_nvml_instant_power_w(int gpu_index) const {
        auto it = nvml_handles_.find(gpu_index);
        if (it == nvml_handles_.end())
            throw std::runtime_error(
                "GPU " + std::to_string(gpu_index) + " not monitored");
        unsigned int power_mw = 0;
        nvml_check(nvmlDeviceGetPowerUsage(it->second, &power_mw),
                   "nvmlDeviceGetPowerUsage");
        return static_cast<double>(power_mw) / 1000.0;
    }
#endif

    // ---- ROCm SMI Wrappers ----

#ifdef ZEUS_HAS_ROCM_SMI
    double get_rocm_total_energy_j(int gpu_index) const {
        uint64_t energy_uj = 0;
        float counter_resolution = 0.0f;
        uint64_t timestamp = 0;
        rsmi_check(rsmi_dev_energy_count_get(
            static_cast<uint32_t>(gpu_index),
            &energy_uj, &counter_resolution, &timestamp),
            "rsmi_dev_energy_count_get");
        return static_cast<double>(energy_uj)
             * static_cast<double>(counter_resolution) / 1e6;
    }

    double get_rocm_power_w(int gpu_index) const {
        uint64_t power_uW = 0;
        rsmi_check(rsmi_dev_power_ave_get(
            static_cast<uint32_t>(gpu_index), 0, &power_uW),
            "rsmi_dev_power_ave_get");
        return static_cast<double>(power_uW) / 1e6;  // uW -> W
    }
#endif

    // ---- Timestamp ----

    static double get_timestamp() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now.time_since_epoch()).count();
    }

    // ---- Background Polling (Pre-Volta NVML) ----

    void start_polling() {
        polling_active_ = true;
        polling_thread_ = std::thread([this]() {
            while (polling_active_.load()) {
                double ts = get_timestamp();
#ifdef ZEUS_HAS_NVML
                for (int idx : nvml_gpu_indices_) {
                    if (!nvml_supports_energy_[idx]) {
                        try {
                            double power = get_nvml_instant_power_w(idx);
                            if (power > 0.0) {
                                std::lock_guard<std::mutex> lock(
                                    samples_mutex_);
                                samples_.push_back({ts, idx, power});
                            }
                        } catch (...) {}
                    }
                }
#endif
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(polling_interval_));
            }
        });
    }

    void stop_polling() {
        if (polling_active_.load()) {
            polling_active_ = false;
            if (polling_thread_.joinable()) polling_thread_.join();
        }
    }

    /**
     * Trapezoidal integration of polled power samples -> energy (Joules).
     * Reference: zeus/monitor/power.py -> PowerMonitor.get_energy()
     */
    double compute_energy_from_samples(int gpu_index,
                                       double start_time,
                                       double end_time) {
        std::lock_guard<std::mutex> lock(samples_mutex_);
        std::vector<std::pair<double, double>> timeline;
        for (const auto& s : samples_) {
            if (s.gpu_index == gpu_index
                && s.timestamp >= start_time
                && s.timestamp <= end_time) {
                timeline.emplace_back(s.timestamp, s.power_w);
            }
        }
        if (timeline.size() < 2) return 0.0;
        std::sort(timeline.begin(), timeline.end());
        double energy = 0.0;
        for (size_t i = 1; i < timeline.size(); ++i) {
            double dt  = timeline[i].first - timeline[i - 1].first;
            double avg = (timeline[i].second + timeline[i - 1].second) / 2.0;
            energy += avg * dt;
        }
        return energy;
    }

    // ---- Architecture Name ----

#ifdef ZEUS_HAS_NVML
    static std::string architecture_to_string(nvmlDeviceArchitecture_t arch) {
        unsigned int a = static_cast<unsigned int>(arch);
        switch (a) {
            case 2:  return "Kepler";
            case 3:  return "Maxwell";
            case 4:  return "Pascal";
            case 5:  return "Volta";
            case 6:  return "Turing";
            case 7:  return "Ampere";
            case 8:  return "Ada Lovelace";
            case 9:  return "Hopper";
            case 10: return "Blackwell";
            default: return "Unknown (arch=" + std::to_string(a) + ")";
        }
    }
#endif
};

} // namespace zeus
