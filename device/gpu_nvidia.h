/**
 * @file device/gpu_nvidia.h
 * @brief NVIDIA GPU monitoring backend via NVML.
 *
 * Reference: zeus/device/gpu/nvidia.py
 *
 * All #ifdef guards are contained within this file.
 * The class NvidiaGpuBackend always exists — if NVML was not compiled,
 * the constructor throws std::runtime_error with a clear message.
 *
 * Features:
 *   - Instantaneous power draw (all NVIDIA GPUs)
 *   - Hardware energy counter (Volta+, architecture >= 5)
 *   - Background power polling with trapezoidal integration (pre-Volta)
 *   - Power management limit query
 *   - Device name / architecture queries
 */

#pragma once

// ---------------------------------------------------------------------------
// Auto-detect NVML availability
// ---------------------------------------------------------------------------
#ifndef ZEUS_NO_NVML
#if defined(__has_include)
#if __has_include(<nvml.h>)
#include <nvml.h>
#ifndef ZEUS_HAS_NVML
#define ZEUS_HAS_NVML 1
#endif
#endif
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace zeus
{

// ---------------------------------------------------------------------------
// NVML helpers (only when compiled with NVML)
// ---------------------------------------------------------------------------
#ifdef ZEUS_HAS_NVML

    inline void nvml_check(nvmlReturn_t result, const std::string &context = "")
    {
        if (result != NVML_SUCCESS)
        {
            std::string msg = "NVML Error: ";
            msg += nvmlErrorString(result);
            if (!context.empty())
                msg += " [" + context + "]";
            throw std::runtime_error(msg);
        }
    }

    class NvmlContext
    {
    public:
        static NvmlContext &instance()
        {
            static NvmlContext ctx;
            return ctx;
        }
        NvmlContext(const NvmlContext &) = delete;
        NvmlContext &operator=(const NvmlContext &) = delete;

    private:
        NvmlContext() { nvml_check(nvmlInit_v2(), "nvmlInit_v2"); }
        ~NvmlContext() { nvmlShutdown(); }
    };

#endif // ZEUS_HAS_NVML

    // ---------------------------------------------------------------------------
    // NvidiaGpuBackend — always defined, throws if NVML not compiled
    // ---------------------------------------------------------------------------

    /**
     * @brief NVIDIA GPU monitoring backend.
     *
     * If NVML was not compiled (nvml.h not found), is_compiled() returns false
     * and the constructor throws std::runtime_error.
     */
    class NvidiaGpuBackend
    {
    public:
        struct PowerSample
        {
            double timestamp; ///< seconds (monotonic)
            int gpu_index;
            double power_w; ///< Watts
        };

        // ---- Compile/runtime availability ----

        static bool is_compiled()
        {
#ifdef ZEUS_HAS_NVML
            return true;
#else
            return false;
#endif
        }

        static bool is_available()
        {
#ifdef ZEUS_HAS_NVML
            try
            {
                NvmlContext::instance();
                unsigned int count = 0;
                nvml_check(nvmlDeviceGetCount(&count));
                return count > 0;
            }
            catch (...)
            {
                return false;
            }
#else
            return false;
#endif
        }

        static int device_count()
        {
#ifdef ZEUS_HAS_NVML
            NvmlContext::instance();
            unsigned int count = 0;
            nvml_check(nvmlDeviceGetCount(&count));
            return static_cast<int>(count);
#else
            return 0;
#endif
        }

        // ---- Construction ----

        explicit NvidiaGpuBackend(const std::vector<int> &gpu_indices = {},
                                  double polling_interval_s = 0.1)
            : polling_active_(false), polling_interval_(polling_interval_s)
        {
#ifdef ZEUS_HAS_NVML
            NvmlContext::instance();
            unsigned int count = 0;
            nvml_check(nvmlDeviceGetCount(&count));

            std::vector<int> indices = gpu_indices;
            if (indices.empty())
            {
                for (unsigned i = 0; i < count; ++i)
                    indices.push_back(static_cast<int>(i));
            }

            bool need_polling = false;
            for (int idx : indices)
            {
                if (idx < 0 || static_cast<unsigned>(idx) >= count)
                {
                    throw std::runtime_error(
                        "GPU " + std::to_string(idx) + " out of range [0, " + std::to_string(count) + ")");
                }
                nvmlDevice_t handle;
                nvml_check(nvmlDeviceGetHandleByIndex(
                    static_cast<unsigned>(idx), &handle));
                handles_[idx] = handle;

                // Volta (arch>=5) and newer support hardware energy counter
                nvmlDeviceArchitecture_t arch;
                bool supports =
                    (nvmlDeviceGetArchitecture(handle, &arch) == NVML_SUCCESS) && (static_cast<unsigned>(arch) >= 5);
                supports_energy_[idx] = supports;
                if (!supports)
                    need_polling = true;
            }
            gpu_indices_ = std::move(indices);
            if (need_polling)
                start_polling();
#else
            (void)gpu_indices;
            (void)polling_interval_s;
            throw std::runtime_error(
                "NVIDIA GPU monitoring requested but NVML was not compiled. "
                "Rebuild with ZEUS_USE_NVML=ON (CMake) or ensure <nvml.h> is available.");
#endif
        }

        ~NvidiaGpuBackend() { stop_polling(); }

        NvidiaGpuBackend(const NvidiaGpuBackend &) = delete;
        NvidiaGpuBackend &operator=(const NvidiaGpuBackend &) = delete;

        // ---- Query methods ----

        const std::vector<int> &gpu_indices() const
        {
#ifdef ZEUS_HAS_NVML
            return gpu_indices_;
#else
            static const std::vector<int> empty;
            return empty;
#endif
        }

        double get_instant_power_w(int gpu_index) const
        {
#ifdef ZEUS_HAS_NVML
            auto it = handles_.find(gpu_index);
            if (it == handles_.end())
                throw std::runtime_error("GPU " + std::to_string(gpu_index) + " not monitored");
            unsigned int power_mw = 0;
            nvml_check(nvmlDeviceGetPowerUsage(it->second, &power_mw),
                       "nvmlDeviceGetPowerUsage");
            return static_cast<double>(power_mw) / 1000.0;
#else
            (void)gpu_index;
            throw std::runtime_error("NVML not compiled");
#endif
        }

        bool supports_energy_counter(int gpu_index) const
        {
#ifdef ZEUS_HAS_NVML
            auto it = supports_energy_.find(gpu_index);
            return it != supports_energy_.end() && it->second;
#else
            (void)gpu_index;
            return false;
#endif
        }

        double get_power_limit_w(int gpu_index) const
        {
#ifdef ZEUS_HAS_NVML
            auto it = handles_.find(gpu_index);
            if (it == handles_.end())
                throw std::runtime_error("GPU " + std::to_string(gpu_index) + " not monitored");
            unsigned int limit_mw = 0;
            nvml_check(nvmlDeviceGetPowerManagementLimit(it->second, &limit_mw));
            return static_cast<double>(limit_mw) / 1000.0;
#else
            (void)gpu_index;
            throw std::runtime_error("NVML not compiled");
#endif
        }

        // ---- Energy snapshot for windows ----

        /** Snapshot the current cumulative energy (Joules) for all monitored GPUs with hw counters. */
        std::map<int, double> snapshot_energy_j() const
        {
            std::map<int, double> snap;
#ifdef ZEUS_HAS_NVML
            for (int idx : gpu_indices_)
            {
                if (supports_energy_counter(idx))
                {
                    snap[idx] = get_total_energy_mj(idx) / 1000.0;
                }
            }
#endif
            return snap;
        }

        /**
         * Compute GPU energy delta (Joules) between a start snapshot and now.
         * Uses hardware counters (Volta+) or trapezoidal integration (pre-Volta).
         */
        std::map<int, double> compute_energy_delta_j(
            const std::map<int, double> &start_snap,
            double start_time,
            double end_time,
            double elapsed_time) const
        {
            std::map<int, double> result;
#ifdef ZEUS_HAS_NVML
            for (int idx : gpu_indices_)
            {
                if (supports_energy_counter(idx))
                {
                    double end_e = get_total_energy_mj(idx) / 1000.0;
                    auto it = start_snap.find(idx);
                    double start_e = (it != start_snap.end()) ? it->second : 0.0;
                    result[idx] = end_e - start_e;
                }
                else
                {
                    double energy = compute_energy_from_samples(idx, start_time, end_time);
                    if (energy <= 0.0)
                    {
                        double power_w = get_instant_power_w(idx);
                        energy = power_w * elapsed_time;
                    }
                    result[idx] = energy;
                }
            }
#else
            (void)start_snap;
            (void)start_time;
            (void)end_time;
            (void)elapsed_time;
#endif
            return result;
        }

        // ---- Static device info ----

        static std::string get_device_name(int gpu_index)
        {
#ifdef ZEUS_HAS_NVML
            NvmlContext::instance();
            nvmlDevice_t handle;
            nvml_check(nvmlDeviceGetHandleByIndex(
                static_cast<unsigned>(gpu_index), &handle));
            char name[256];
            nvml_check(nvmlDeviceGetName(handle, name, sizeof(name)));
            return std::string(name);
#else
            (void)gpu_index;
            return "Unknown";
#endif
        }

        static std::string get_architecture_name(int gpu_index)
        {
#ifdef ZEUS_HAS_NVML
            NvmlContext::instance();
            nvmlDevice_t handle;
            nvml_check(nvmlDeviceGetHandleByIndex(
                static_cast<unsigned>(gpu_index), &handle));
            nvmlDeviceArchitecture_t arch;
            nvml_check(nvmlDeviceGetArchitecture(handle, &arch));
            return architecture_to_string(arch);
#else
            (void)gpu_index;
            return "Unknown";
#endif
        }

    private:
        std::atomic<bool> polling_active_;
        double polling_interval_;
        std::thread polling_thread_;
        mutable std::mutex samples_mutex_;
        std::vector<PowerSample> samples_;

#ifdef ZEUS_HAS_NVML
        std::vector<int> gpu_indices_;
        std::map<int, nvmlDevice_t> handles_;
        std::map<int, bool> supports_energy_;
#endif

        // ---- Background polling (pre-Volta) ----

        // ---- Internal: raw cumulative counter (used by snapshot/delta only) ----

        double get_total_energy_mj(int gpu_index) const
        {
#ifdef ZEUS_HAS_NVML
            auto it = handles_.find(gpu_index);
            if (it == handles_.end())
                throw std::runtime_error("GPU " + std::to_string(gpu_index) + " not monitored");
            unsigned long long energy_mj = 0;
            nvml_check(nvmlDeviceGetTotalEnergyConsumption(it->second, &energy_mj),
                       "nvmlDeviceGetTotalEnergyConsumption");
            return static_cast<double>(energy_mj);
#else
            (void)gpu_index;
            throw std::runtime_error("NVML not compiled");
#endif
        }

        void start_polling()
        {
#ifdef ZEUS_HAS_NVML
            polling_active_ = true;
            polling_thread_ = std::thread([this]()
                                          {
            while (polling_active_.load()) {
                double ts = get_timestamp();
                for (int idx : gpu_indices_) {
                    if (!supports_energy_[idx]) {
                        try {
                            double power = get_instant_power_w(idx);
                            if (power > 0.0) {
                                std::lock_guard<std::mutex> lock(samples_mutex_);
                                samples_.push_back({ts, idx, power});
                            }
                        } catch (...) {}
                    }
                }
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(polling_interval_));
            } });
#endif
        }

        void stop_polling()
        {
            if (polling_active_.load())
            {
                polling_active_ = false;
                if (polling_thread_.joinable())
                    polling_thread_.join();
            }
        }

        /**
         * Trapezoidal integration of polled power samples → energy (Joules).
         * Reference: zeus/monitor/power.py → PowerMonitor.get_energy()
         */
        double compute_energy_from_samples(int gpu_index,
                                           double start_time,
                                           double end_time) const
        {
            std::lock_guard<std::mutex> lock(samples_mutex_);
            std::vector<std::pair<double, double>> timeline;
            for (const auto &s : samples_)
            {
                if (s.gpu_index == gpu_index && s.timestamp >= start_time && s.timestamp <= end_time)
                {
                    timeline.emplace_back(s.timestamp, s.power_w);
                }
            }
            if (timeline.size() < 2)
                return 0.0;
            std::sort(timeline.begin(), timeline.end());
            double energy = 0.0;
            for (size_t i = 1; i < timeline.size(); ++i)
            {
                double dt = timeline[i].first - timeline[i - 1].first;
                double avg = (timeline[i].second + timeline[i - 1].second) / 2.0;
                energy += avg * dt;
            }
            return energy;
        }

        // ---- Timestamp ----

        static double get_timestamp()
        {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration<double>(now.time_since_epoch()).count();
        }

        // ---- Architecture name ----

#ifdef ZEUS_HAS_NVML
        static std::string architecture_to_string(nvmlDeviceArchitecture_t arch)
        {
            unsigned int a = static_cast<unsigned int>(arch);
            switch (a)
            {
            case 2:
                return "Kepler";
            case 3:
                return "Maxwell";
            case 4:
                return "Pascal";
            case 5:
                return "Volta";
            case 6:
                return "Turing";
            case 7:
                return "Ampere";
            case 8:
                return "Ada Lovelace";
            case 9:
                return "Hopper";
            case 10:
                return "Blackwell";
            default:
                return "Unknown (arch=" + std::to_string(a) + ")";
            }
        }
#endif
    };

} // namespace zeus
