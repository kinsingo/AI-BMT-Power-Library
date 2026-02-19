/**
 * @file power_monitor.h
 * @brief Header-only C++ GPU Power Monitoring Library
 *
 * C++ port of the Zeus project's power measurement functionality.
 * Reference: https://github.com/ml-energy/zeus
 *
 * Key Python source files this is based on:
 *   - zeus/monitor/energy.py   : ZeusMonitor class (begin_window / end_window)
 *   - zeus/device/gpu/nvidia.py: NVML API wrappers (energy counter, power usage)
 *   - zeus/monitor/power.py    : Background power polling for pre-Volta GPUs
 *
 * Uses NVIDIA NVML C API for GPU power/energy measurement.
 * Supports two measurement strategies:
 *   1. Volta+ GPUs: Hardware energy counter (nvmlDeviceGetTotalEnergyConsumption)
 *   2. Pre-Volta GPUs: Background polling + trapezoidal integration
 */

#pragma once

#include <nvml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace zeus {

// ============================================================================
// NVML Error Helper
// ============================================================================

/**
 * @brief Check NVML return code and throw on error.
 */
inline void nvml_check(nvmlReturn_t result, const std::string& context = "") {
    if (result != NVML_SUCCESS) {
        std::string msg = "NVML Error: ";
        msg += nvmlErrorString(result);
        if (!context.empty()) {
            msg += " [" + context + "]";
        }
        throw std::runtime_error(msg);
    }
}

// ============================================================================
// NVML Lifecycle (RAII Singleton)
// ============================================================================

/**
 * @brief RAII wrapper for NVML initialization/shutdown.
 *
 * Ensures nvmlInit() is called once and nvmlShutdown() on program exit.
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
    NvmlContext() { nvml_check(nvmlInit_v2(), "nvmlInit_v2"); }
    ~NvmlContext() { nvmlShutdown(); }
};

// ============================================================================
// Measurement Result
// ============================================================================

/**
 * @brief Result of a power measurement window.
 *
 * Corresponds to zeus.monitor.energy.Measurement in the Python version.
 * Contains per-GPU energy consumption in Joules.
 */
struct Measurement {
    /** GPU index -> energy consumed in Joules */
    std::map<int, double> gpu_energy;

    /**
     * @brief Total energy consumed across all monitored GPUs (Joules).
     *
     * Equivalent to Measurement.total_energy in Python Zeus.
     */
    double total_energy() const {
        double total = 0.0;
        for (const auto& kv : gpu_energy) {
            total += kv.second;
        }
        return total;
    }
};

// ============================================================================
// PowerMonitor - Main Library Class
// ============================================================================

/**
 * @brief GPU Power Monitor using NVIDIA NVML.
 *
 * C++ equivalent of zeus.monitor.energy.ZeusMonitor (power measurement only).
 *
 * Usage:
 * @code
 *   zeus::PowerMonitor monitor({0});          // Monitor GPU 0
 *   monitor.begin_window("train");            // Start measurement
 *   // ... GPU workload ...
 *   auto result = monitor.end_window("train"); // Stop & get result
 *   std::cout << result.total_energy() << " J" << std::endl;
 * @endcode
 *
 * Measurement strategy (same as Python Zeus):
 *   - Volta+ GPUs: Uses nvmlDeviceGetTotalEnergyConsumption (hardware counter)
 *   - Pre-Volta:   Background thread polls nvmlDeviceGetPowerUsage,
 *                   then applies trapezoidal integration to compute energy.
 */
class PowerMonitor {
public:
    // ---- Construction / Destruction ----

    /**
     * @brief Construct a PowerMonitor.
     * @param gpu_indices GPU indices to monitor. Empty = all GPUs.
     * @param polling_interval_s Polling interval in seconds for pre-Volta GPUs (default: 0.1s).
     */
    explicit PowerMonitor(std::vector<int> gpu_indices = {},
                          double polling_interval_s = 0.1)
        : polling_active_(false)
        , polling_interval_(polling_interval_s)
    {
        // Ensure NVML is initialized (singleton)
        NvmlContext::instance();

        // Get device count
        unsigned int device_count = 0;
        nvml_check(nvmlDeviceGetCount(&device_count), "nvmlDeviceGetCount");

        // If no indices given, monitor all GPUs
        if (gpu_indices.empty()) {
            for (unsigned int i = 0; i < device_count; ++i) {
                gpu_indices_.push_back(static_cast<int>(i));
            }
        } else {
            gpu_indices_ = std::move(gpu_indices);
        }

        // Validate indices and get device handles
        bool need_polling = false;
        for (int idx : gpu_indices_) {
            if (idx < 0 || static_cast<unsigned int>(idx) >= device_count) {
                throw std::runtime_error(
                    "GPU index " + std::to_string(idx) + " out of range [0, "
                    + std::to_string(device_count) + ")");
            }

            nvmlDevice_t handle;
            nvml_check(
                nvmlDeviceGetHandleByIndex(static_cast<unsigned int>(idx), &handle),
                "nvmlDeviceGetHandleByIndex(" + std::to_string(idx) + ")");
            handles_[idx] = handle;

            // Check Volta+ (architecture >= NVML_DEVICE_ARCH_VOLTA = 5)
            bool supports = check_energy_counter_support(handle);
            supports_energy_counter_[idx] = supports;
            if (!supports) {
                need_polling = true;
            }
        }

        // Start background polling thread for pre-Volta GPUs
        if (need_polling) {
            start_polling();
        }
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
     * Corresponds to ZeusMonitor.begin_window() in Python Zeus.
     * Snapshots the GPU energy counter (Volta+) or records the start timestamp.
     *
     * @param key Unique name for this measurement window.
     * @throws std::runtime_error if the window key already exists.
     */
    void begin_window(const std::string& key) {
        if (windows_.count(key)) {
            throw std::runtime_error(
                "Measurement window '" + key + "' already active. "
                "Call end_window(\"" + key + "\") first.");
        }

        WindowState state;
        state.start_time = get_timestamp();

        // Snapshot energy counters for Volta+ GPUs
        for (int idx : gpu_indices_) {
            if (supports_energy_counter_[idx]) {
                // nvmlDeviceGetTotalEnergyConsumption returns millijoules
                state.start_energy[idx] = get_total_energy_mj(idx) / 1000.0; // mJ -> J
            }
        }

        windows_[key] = std::move(state);
    }

    /**
     * @brief Mark the end of a measurement window and return the result.
     *
     * Corresponds to ZeusMonitor.end_window() in Python Zeus.
     *
     * For Volta+ GPUs: energy = end_counter - start_counter
     * For pre-Volta:   energy = trapezoidal integration of polled power samples
     *                  Fallback: instant_power × elapsed_time
     *
     * @param key Name of the window started with begin_window().
     * @return Measurement result with per-GPU energy in Joules.
     * @throws std::runtime_error if the window key does not exist.
     */
    Measurement end_window(const std::string& key) {
        auto it = windows_.find(key);
        if (it == windows_.end()) {
            throw std::runtime_error(
                "Measurement window '" + key + "' not found. "
                "Call begin_window(\"" + key + "\") first.");
        }

        double end_time = get_timestamp();
        WindowState& state = it->second;
        Measurement result;

        for (int idx : gpu_indices_) {
            if (supports_energy_counter_[idx]) {
                // PATH A: Volta+ hardware energy counter (primary path)
                double end_energy = get_total_energy_mj(idx) / 1000.0; // mJ -> J
                result.gpu_energy[idx] = end_energy - state.start_energy[idx];
            } else {
                // PATH B: Pre-Volta trapezoidal integration
                double energy = compute_energy_from_samples(
                    idx, state.start_time, end_time);

                // Fallback: if insufficient samples, use instant power * time
                if (energy <= 0.0) {
                    double power_w = get_instant_power_w(idx);
                    double elapsed = end_time - state.start_time;
                    energy = power_w * elapsed;
                }
                result.gpu_energy[idx] = energy;
            }
        }

        windows_.erase(it);
        return result;
    }

    // ---- Power / Energy Query Functions ----

    /**
     * @brief Get instantaneous power draw for a GPU.
     *
     * Uses nvmlDeviceGetPowerUsage (available on all NVIDIA GPUs).
     *
     * @param gpu_index GPU index.
     * @return Power in Watts.
     */
    double get_instant_power(int gpu_index) const {
        return get_instant_power_w(gpu_index);
    }

    /**
     * @brief Get cumulative total energy consumption since driver load (Volta+ only).
     *
     * Uses nvmlDeviceGetTotalEnergyConsumption.
     *
     * @param gpu_index GPU index.
     * @return Energy in Joules.
     */
    double get_total_energy(int gpu_index) const {
        return get_total_energy_mj(gpu_index) / 1000.0;
    }

    /**
     * @brief Check if a GPU supports the hardware energy counter (Volta+).
     * @param gpu_index GPU index.
     * @return true if Volta or newer architecture.
     */
    bool supports_energy_counter(int gpu_index) const {
        auto it = supports_energy_counter_.find(gpu_index);
        if (it == supports_energy_counter_.end()) {
            throw std::runtime_error(
                "GPU " + std::to_string(gpu_index) + " is not being monitored.");
        }
        return it->second;
    }

    /**
     * @brief Get the power management limit for a GPU.
     * @param gpu_index GPU index.
     * @return Power limit in Watts.
     */
    double get_power_limit(int gpu_index) const {
        auto it = handles_.find(gpu_index);
        if (it == handles_.end()) {
            throw std::runtime_error(
                "GPU " + std::to_string(gpu_index) + " is not being monitored.");
        }
        unsigned int limit_mw = 0;
        nvml_check(nvmlDeviceGetPowerManagementLimit(it->second, &limit_mw),
                   "nvmlDeviceGetPowerManagementLimit");
        return static_cast<double>(limit_mw) / 1000.0; // mW -> W
    }

    /** @brief Get the list of monitored GPU indices. */
    const std::vector<int>& gpu_indices() const { return gpu_indices_; }

    // ---- Static Utility Functions ----

    /** @brief Get the number of NVIDIA GPUs in the system. */
    static int get_device_count() {
        NvmlContext::instance();
        unsigned int count = 0;
        nvml_check(nvmlDeviceGetCount(&count), "nvmlDeviceGetCount");
        return static_cast<int>(count);
    }

    /** @brief Get the name of a GPU by index. */
    static std::string get_device_name(int gpu_index) {
        NvmlContext::instance();
        nvmlDevice_t handle;
        nvml_check(nvmlDeviceGetHandleByIndex(
            static_cast<unsigned int>(gpu_index), &handle));
        char name[256];
        nvml_check(nvmlDeviceGetName(handle, name, sizeof(name)));
        return std::string(name);
    }

    /**
     * @brief Get the architecture name of a GPU.
     *
     * Uses nvmlDeviceGetArchitecture. This is the same check used internally
     * to determine Volta+ support for the hardware energy counter.
     */
    static std::string get_architecture_name(int gpu_index) {
        NvmlContext::instance();
        nvmlDevice_t handle;
        nvml_check(nvmlDeviceGetHandleByIndex(
            static_cast<unsigned int>(gpu_index), &handle));
        nvmlDeviceArchitecture_t arch;
        nvml_check(nvmlDeviceGetArchitecture(handle, &arch));
        return architecture_to_string(arch);
    }

private:
    // ---- Internal Types ----

    struct PowerSample {
        double timestamp; // seconds (monotonic)
        int    gpu_index;
        double power_w;   // Watts
    };

    struct WindowState {
        double                  start_time;    // monotonic timestamp
        std::map<int, double>   start_energy;  // Joules (Volta+ only)
    };

    // ---- State ----

    std::vector<int>            gpu_indices_;
    std::map<int, nvmlDevice_t> handles_;
    std::map<int, bool>         supports_energy_counter_;
    std::map<std::string, WindowState> windows_;

    // Background polling state (pre-Volta)
    std::thread                 polling_thread_;
    std::atomic<bool>           polling_active_;
    std::mutex                  samples_mutex_;
    std::vector<PowerSample>    samples_;
    double                      polling_interval_; // seconds

    // ---- NVML Wrappers ----

    /**
     * Check if GPU supports hardware energy counter.
     * Volta (arch=5) and newer architectures support nvmlDeviceGetTotalEnergyConsumption.
     * Reference: nvidia.py -> NVIDIAGPU.supports_get_total_energy_consumption()
     */
    static bool check_energy_counter_support(nvmlDevice_t handle) {
        nvmlDeviceArchitecture_t arch;
        nvmlReturn_t ret = nvmlDeviceGetArchitecture(handle, &arch);
        if (ret != NVML_SUCCESS) return false;
        // NVML_DEVICE_ARCH_VOLTA = 5
        return static_cast<unsigned int>(arch) >= 5;
    }

    /**
     * Get cumulative energy in millijoules.
     * Reference: nvidia.py -> NVIDIAGPU.get_total_energy_consumption()
     */
    double get_total_energy_mj(int gpu_index) const {
        auto it = handles_.find(gpu_index);
        if (it == handles_.end()) {
            throw std::runtime_error(
                "GPU " + std::to_string(gpu_index) + " is not being monitored.");
        }
        unsigned long long energy_mj = 0;
        nvml_check(nvmlDeviceGetTotalEnergyConsumption(it->second, &energy_mj),
                   "nvmlDeviceGetTotalEnergyConsumption");
        return static_cast<double>(energy_mj);
    }

    /**
     * Get instantaneous power in Watts.
     * Reference: nvidia.py -> NVIDIAGPU.get_instant_power_usage()
     * Note: We use nvmlDeviceGetPowerUsage for broad compatibility.
     *       Python Zeus uses nvmlDeviceGetFieldValues(NVML_FI_DEV_POWER_INSTANT)
     *       which is equivalent but newer.
     */
    double get_instant_power_w(int gpu_index) const {
        auto it = handles_.find(gpu_index);
        if (it == handles_.end()) {
            throw std::runtime_error(
                "GPU " + std::to_string(gpu_index) + " is not being monitored.");
        }
        unsigned int power_mw = 0;
        nvml_check(nvmlDeviceGetPowerUsage(it->second, &power_mw),
                   "nvmlDeviceGetPowerUsage");
        return static_cast<double>(power_mw) / 1000.0; // mW -> W
    }

    // ---- Timestamp ----

    static double get_timestamp() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now.time_since_epoch()).count();
    }

    // ---- Background Polling (Pre-Volta) ----
    // Reference: zeus/monitor/power.py -> _domain_polling_process()

    void start_polling() {
        polling_active_ = true;
        polling_thread_ = std::thread([this]() {
            while (polling_active_.load()) {
                double ts = get_timestamp();
                for (int idx : gpu_indices_) {
                    if (!supports_energy_counter_[idx]) {
                        try {
                            double power = get_instant_power_w(idx);
                            if (power > 0.0) {
                                std::lock_guard<std::mutex> lock(samples_mutex_);
                                samples_.push_back({ts, idx, power});
                            }
                        } catch (...) {
                            // Skip failed readings
                        }
                    }
                }
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(polling_interval_));
            }
        });
    }

    void stop_polling() {
        if (polling_active_.load()) {
            polling_active_ = false;
            if (polling_thread_.joinable()) {
                polling_thread_.join();
            }
        }
    }

    /**
     * Compute energy from polled power samples using trapezoidal integration.
     * Reference: zeus/monitor/power.py -> PowerMonitor.get_energy()
     *            which uses sklearn.metrics.auc() on (timestamp, power) pairs.
     */
    double compute_energy_from_samples(int gpu_index,
                                       double start_time,
                                       double end_time) {
        std::lock_guard<std::mutex> lock(samples_mutex_);

        // Collect relevant samples for this GPU in the time window
        std::vector<std::pair<double, double>> timeline; // (time, power_w)
        for (const auto& s : samples_) {
            if (s.gpu_index == gpu_index &&
                s.timestamp >= start_time &&
                s.timestamp <= end_time) {
                timeline.emplace_back(s.timestamp, s.power_w);
            }
        }

        if (timeline.size() < 2) return 0.0;

        // Sort by timestamp
        std::sort(timeline.begin(), timeline.end());

        // Trapezoidal integration: energy = sum( (t[i+1]-t[i]) * (P[i+1]+P[i]) / 2 )
        double energy = 0.0;
        for (size_t i = 1; i < timeline.size(); ++i) {
            double dt = timeline[i].first - timeline[i - 1].first;
            double avg_power = (timeline[i].second + timeline[i - 1].second) / 2.0;
            energy += avg_power * dt;
        }

        return energy;
    }

    // ---- Architecture Name Helper ----

    static std::string architecture_to_string(nvmlDeviceArchitecture_t arch) {
        // Numeric values: Kepler=2, Maxwell=3, Pascal=4, Volta=5,
        //                 Turing=6, Ampere=7, Ada=8, Hopper=9, Blackwell=10
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
};

} // namespace zeus
