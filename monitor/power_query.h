/**
 * @file monitor/power_query.h
 * @brief Instantaneous power and device capability queries.
 *
 * Separates "power measurement" (instant readings in Watts) from
 * "energy measurement" (windowed readings in Joules) per the user's
 * architecture design.
 *
 * Contains NO #ifdef guards. All platform logic is in the device backends.
 */

#pragma once

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "../device/gpu_nvidia.h"
#include "../device/soc_jetson.h"
#include "../device/soc_apple.h"

namespace zeus
{

    /**
     * @brief Provides instant power readings and device capability queries.
     *
     * Delegates to GPU and SoC backends.
     * Receives non-owning pointers from PowerMonitor.
     */
    class PowerQuery
    {
    public:
        struct BackendPtrs
        {
            NvidiaGpuBackend *nvidia = nullptr;
            JetsonSoCBackend *jetson = nullptr;
            AppleSoCBackend *apple = nullptr;
        };

        explicit PowerQuery(BackendPtrs backends)
            : backends_(backends) {}

        // ---- Power queries ----

        /**
         * @brief Get instantaneous GPU power draw (Watts).
         *
         * NVIDIA: nvmlDeviceGetPowerUsage
         */
        double get_instant_power(int gpu_index) const
        {
            if (backends_.nvidia)
                return backends_.nvidia->get_instant_power_w(gpu_index);
            throw std::runtime_error("No GPU backend available for power query");
        }

        /**
         * @brief Get the power management limit for a GPU (Watts).
         *
         * NVIDIA only.
         */
        double get_power_limit(int gpu_index) const
        {
            if (backends_.nvidia)
                return backends_.nvidia->get_power_limit_w(gpu_index);
            throw std::runtime_error(
                "Power limit query only available on NVIDIA GPUs");
        }

        // ---- SoC power queries ----

        /**
         * @brief Get instantaneous SoC power (Watts) for a specific metric.
         *
         * Jetson: reads INA3221 sysfs directly.
         * Apple:  takes two IOReport samples ~50ms apart → power = dE/dt.
         *
         * @param metric  SoC metric key (e.g., "jetson_cpu", "apple_gpu")
         */
        double get_instant_soc_power(const std::string &metric) const
        {
            if (backends_.jetson)
                return backends_.jetson->get_instant_power_w(metric);
            if (backends_.apple)
                return backends_.apple->get_instant_power_w(metric);
            throw std::runtime_error("No SoC backend available for power query");
        }

        // ---- Device info (GPU) ----

        /** @brief Whether any GPU backend is active. */
        bool has_gpu() const
        {
            return backends_.nvidia != nullptr;
        }

        /** @brief GPU backend type string ("NVIDIA", or "None"). */
        std::string gpu_type() const
        {
            if (backends_.nvidia)
                return "NVIDIA";
            return "None";
        }

        /** @brief List of monitored GPU indices. */
        std::vector<int> gpu_indices() const
        {
            if (backends_.nvidia)
            {
                const auto &idx = backends_.nvidia->gpu_indices();
                return std::vector<int>(idx.begin(), idx.end());
            }
            return {};
        }

        // ---- Device info (SoC) ----

        /** @brief Whether any SoC backend is active. */
        bool has_soc() const
        {
            return backends_.jetson || backends_.apple;
        }

        /** @brief SoC backend type string ("Jetson", "Apple", or "None"). */
        std::string soc_type() const
        {
            if (backends_.jetson)
                return "Jetson";
            if (backends_.apple)
                return "Apple";
            return "None";
        }

        /** @brief Set of available SoC metric keys. */
        std::set<std::string> soc_metrics() const
        {
            std::set<std::string> metrics;
            if (backends_.jetson)
            {
                auto m = backends_.jetson->available_metrics();
                metrics.insert(m.begin(), m.end());
            }
            if (backends_.apple)
            {
                auto m = backends_.apple->available_metrics();
                metrics.insert(m.begin(), m.end());
            }
            return metrics;
        }

        // ---- Static utility ----

        /** @brief Get total GPU count (NVIDIA). */
        static int get_device_count()
        {
            int count = 0;
            try
            {
                count += NvidiaGpuBackend::device_count();
            }
            catch (...)
            {
            }
            return count;
        }

        /** @brief Get the name of a GPU by index. */
        static std::string get_device_name(int gpu_index)
        {
            if (NvidiaGpuBackend::is_available())
            {
                try
                {
                    return NvidiaGpuBackend::get_device_name(gpu_index);
                }
                catch (...)
                {
                }
            }
            return "Unknown GPU " + std::to_string(gpu_index);
        }

        /** @brief Get architecture name (NVIDIA only). */
        static std::string get_architecture_name(int gpu_index)
        {
            if (NvidiaGpuBackend::is_available())
            {
                try
                {
                    return NvidiaGpuBackend::get_architecture_name(gpu_index);
                }
                catch (...)
                {
                }
            }
            return "Unknown";
        }

    private:
        BackendPtrs backends_;
    };

} // namespace zeus
