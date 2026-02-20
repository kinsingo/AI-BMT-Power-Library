/**
 * @file power_monitor.h
 * @brief Facade header for the Zeus C++ Power Monitoring Library.
 *
 * This is the ONLY header users need to include.
 * It aggregates all device backends and monitor classes into a unified
 * PowerMonitor interface.
 *
 * Architecture:
 *   power_monitor.h (facade — this file)
 *   ├── device/device_type.h      DeviceType enum
 *   ├── device/gpu_nvidia.h       NVIDIA NVML backend
 *   ├── device/gpu_amd.h          AMD ROCm SMI backend
 *   ├── device/soc_jetson.h       Jetson INA3221 backend
 *   ├── device/soc_apple.h        Apple Silicon backend
 *   ├── monitor/measurement.h     Measurement result struct
 *   ├── monitor/energy_monitor.h  Window-based energy measurement
 *   └── monitor/power_query.h     Instant power queries
 *
 * All #ifdef guards are contained inside device backend files.
 * This facade and the user-facing API are completely #ifdef-free.
 *
 * Usage:
 * @code
 *   #include "power_monitor.h"
 *
 *   // Enum-based device selection — throws if device unavailable
 *   try {
 *       zeus::PowerMonitor monitor({zeus::DeviceType::NvidiaGPU});
 *       monitor.begin_window("train");
 *       // ... workload ...
 *       auto result = monitor.end_window("train");
 *       std::cout << "GPU: " << result.total_gpu_energy() << " J\n";
 *   } catch (const std::runtime_error& e) {
 *       std::cout << "Device unavailable: " << e.what() << std::endl;
 *       // Continue without monitoring
 *   }
 * @endcode
 *
 * Reference: https://github.com/ml-energy/zeus
 */

#pragma once

// ---------------------------------------------------------------------------
// Include all sub-modules
// ---------------------------------------------------------------------------
#include "device/device_type.h"
#include "device/gpu_nvidia.h"
#include "device/gpu_amd.h"
#include "device/soc_jetson.h"
#include "device/soc_apple.h"
#include "monitor/measurement.h"
#include "monitor/energy_monitor.h"
#include "monitor/power_query.h"

#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace zeus
{

    /**
     * @brief Configuration for PowerMonitor construction.
     *
     * Defined at namespace scope to work around a GCC 11 limitation with
     * nested aggregate structs used as inline default parameters.
     * Accessible as zeus::PowerMonitor::Config via the using alias below.
     */
    struct PowerMonitorConfig
    {
        std::vector<int> gpu_indices = {}; ///< Empty = all GPUs
        double polling_interval_s = 0.1;   ///< Pre-Volta / Jetson polling rate
    };

    /**
     * @brief Unified multi-device power/energy monitor (facade).
     *
     * Users specify which devices to monitor via DeviceType enum values.
     * If a requested device is unavailable (not compiled or not present),
     * the constructor throws std::runtime_error with a descriptive message.
     *
     * Pattern: catch the error → skip monitoring, or select different devices.
     */
    class PowerMonitor
    {
    public:
        /** @brief Alias so existing code using PowerMonitor::Config still works. */
        using Config = PowerMonitorConfig;

        // ================================================================
        // Construction
        // ================================================================

        /**
         * @brief Construct a PowerMonitor with enum-based device selection.
         *
         * @param devices  Which device types to monitor.
         * @param cfg      Optional configuration (GPU indices, polling rate, etc.).
         *
         * @throws std::runtime_error if any requested device is unavailable.
         *
         * Usage:
         * @code
         *   try {
         *       zeus::PowerMonitor mon({zeus::DeviceType::NvidiaGPU});
         *       mon.begin_window("train");
         *       // ... work ...
         *       auto m = mon.end_window("train");
         *   } catch (const std::runtime_error& e) {
         *       std::cout << e.what() << std::endl;
         *   }
         * @endcode
         */
        explicit PowerMonitor(std::vector<DeviceType> devices,
                              Config cfg = Config{})
        {
            if (devices.empty())
            {
                throw std::runtime_error(
                    "PowerMonitor: at least one DeviceType must be specified.");
            }

            for (auto dev : devices)
            {
                switch (dev)
                {
                case DeviceType::NvidiaGPU:
                    nvidia_ = std::make_unique<NvidiaGpuBackend>(
                        cfg.gpu_indices, cfg.polling_interval_s);
                    break;

                case DeviceType::AmdGPU:
                    amd_ = std::make_unique<AmdGpuBackend>(cfg.gpu_indices);
                    break;

                case DeviceType::JetsonSoC:
                    jetson_ = std::make_unique<JetsonSoCBackend>(
                        cfg.polling_interval_s);
                    break;

                case DeviceType::AppleSoC:
                    apple_ = std::make_unique<AppleSoCBackend>();
                    break;

                default:
                    throw std::runtime_error(
                        "PowerMonitor: unknown DeviceType value.");
                }
            }

            // Build internal monitor/query objects
            rebuild_internals();
        }

        ~PowerMonitor() = default;

        // Non-copyable
        PowerMonitor(const PowerMonitor &) = delete;
        PowerMonitor &operator=(const PowerMonitor &) = delete;

        // ================================================================
        // Energy Measurement (delegates to EnergyMonitor)
        // ================================================================

        /** @brief Start a measurement window. */
        void begin_window(const std::string &key)
        {
            energy_monitor_->begin_window(key);
        }

        /** @brief End a measurement window and return energy results. */
        Measurement end_window(const std::string &key)
        {
            return energy_monitor_->end_window(key);
        }

        // ================================================================
        // Power Queries — GPU (delegates to PowerQuery)
        // ================================================================

        /** @brief Instantaneous GPU power draw (Watts). */
        double get_instant_power(int gpu_index) const
        {
            return power_query_->get_instant_power(gpu_index);
        }

        /** @brief Power management limit for a GPU (Watts). NVIDIA only. */
        double get_power_limit(int gpu_index) const
        {
            return power_query_->get_power_limit(gpu_index);
        }

        // ================================================================
        // Power Queries — SoC (delegates to PowerQuery)
        // ================================================================

        /**
         * @brief Instantaneous SoC power (Watts) for a specific metric.
         *
         * Jetson: reads INA3221 sysfs directly.
         * Apple:  two IOReport samples ~50ms apart → power = dE/dt.
         *
         * @param metric  SoC metric key (e.g., "jetson_cpu", "apple_gpu")
         */
        double get_instant_soc_power(const std::string &metric) const
        {
            return power_query_->get_instant_soc_power(metric);
        }

        // ================================================================
        // Device Info
        // ================================================================

        /** @brief Whether any GPU backend is active. */
        bool has_gpu() const { return power_query_->has_gpu(); }

        /** @brief Whether any SoC backend is active. */
        bool has_soc() const { return power_query_->has_soc(); }

        /** @brief GPU backend type string ("NVIDIA", "AMD", or "None"). */
        std::string gpu_type() const { return power_query_->gpu_type(); }

        /** @brief SoC backend type string ("Jetson", "Apple", or "None"). */
        std::string soc_type() const { return power_query_->soc_type(); }

        /** @brief List of monitored GPU indices. */
        std::vector<int> gpu_indices() const { return power_query_->gpu_indices(); }

        /** @brief Set of SoC metric keys (e.g., "jetson_cpu", "apple_gpu"). */
        std::set<std::string> soc_metrics() const
        {
            return power_query_->soc_metrics();
        }

        // ================================================================
        // Static Utilities
        // ================================================================

        /** @brief Get total GPU count (NVIDIA + AMD). */
        static int get_device_count()
        {
            return PowerQuery::get_device_count();
        }

        /** @brief Get the name of a GPU by index. */
        static std::string get_device_name(int gpu_index)
        {
            return PowerQuery::get_device_name(gpu_index);
        }

        /** @brief Get the architecture name of a GPU (NVIDIA only). */
        static std::string get_architecture_name(int gpu_index)
        {
            return PowerQuery::get_architecture_name(gpu_index);
        }

    private:
        // ---- Owned backends ----
        std::unique_ptr<NvidiaGpuBackend> nvidia_;
        std::unique_ptr<AmdGpuBackend> amd_;
        std::unique_ptr<JetsonSoCBackend> jetson_;
        std::unique_ptr<AppleSoCBackend> apple_;

        // ---- Internal monitor/query objects ----
        std::unique_ptr<EnergyMonitor> energy_monitor_;
        std::unique_ptr<PowerQuery> power_query_;

        void rebuild_internals()
        {
            // Wire up backend pointers for EnergyMonitor
            EnergyMonitor::BackendPtrs eptrs;
            eptrs.nvidia = nvidia_.get();
            eptrs.amd = amd_.get();
            eptrs.jetson = jetson_.get();
            eptrs.apple = apple_.get();
            energy_monitor_ = std::make_unique<EnergyMonitor>(eptrs);

            // Wire up backend pointers for PowerQuery
            PowerQuery::BackendPtrs pptrs;
            pptrs.nvidia = nvidia_.get();
            pptrs.amd = amd_.get();
            pptrs.jetson = jetson_.get();
            pptrs.apple = apple_.get();
            power_query_ = std::make_unique<PowerQuery>(pptrs);
        }
    };

} // namespace zeus
