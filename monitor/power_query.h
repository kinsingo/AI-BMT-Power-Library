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

#include <stdexcept>
#include <string>
#include <vector>

#include "../device/gpu_nvidia.h"
#include "../device/gpu_amd.h"

namespace zeus {

/**
 * @brief Provides instant power readings and device capability queries.
 *
 * Delegates to GPU backends. Receives non-owning pointers from PowerMonitor.
 */
class PowerQuery {
public:
    struct BackendPtrs {
        NvidiaGpuBackend* nvidia = nullptr;
        AmdGpuBackend*    amd    = nullptr;
    };

    explicit PowerQuery(BackendPtrs backends)
        : backends_(backends) {}

    // ---- Power queries ----

    /**
     * @brief Get instantaneous GPU power draw (Watts).
     *
     * NVIDIA: nvmlDeviceGetPowerUsage
     * AMD:    rsmi_dev_power_ave_get
     */
    double get_instant_power(int gpu_index) const {
        if (backends_.nvidia)
            return backends_.nvidia->get_instant_power_w(gpu_index);
        if (backends_.amd)
            return backends_.amd->get_instant_power_w(gpu_index);
        throw std::runtime_error("No GPU backend available for power query");
    }

    /**
     * @brief Get cumulative total energy (Joules) since driver load.
     *
     * NVIDIA Volta+ only.
     */
    double get_total_energy(int gpu_index) const {
        if (backends_.nvidia)
            return backends_.nvidia->get_total_energy_mj(gpu_index) / 1000.0;
        throw std::runtime_error(
            "Total energy counter only available on NVIDIA Volta+ GPUs");
    }

    /** @brief Check if a GPU supports the hardware energy counter. */
    bool supports_energy_counter(int gpu_index) const {
        if (backends_.nvidia)
            return backends_.nvidia->supports_energy_counter(gpu_index);
        return false;
    }

    /**
     * @brief Get the power management limit for a GPU (Watts).
     *
     * NVIDIA only.
     */
    double get_power_limit(int gpu_index) const {
        if (backends_.nvidia)
            return backends_.nvidia->get_power_limit_w(gpu_index);
        throw std::runtime_error(
            "Power limit query only available on NVIDIA GPUs");
    }

    // ---- Device info ----

    /** @brief Whether any GPU backend is active. */
    bool has_gpu() const {
        return backends_.nvidia || backends_.amd;
    }

    /** @brief GPU backend type string ("NVIDIA", "AMD", or "None"). */
    std::string gpu_type() const {
        if (backends_.nvidia) return "NVIDIA";
        if (backends_.amd)    return "AMD";
        return "None";
    }

    /** @brief List of monitored GPU indices. */
    std::vector<int> gpu_indices() const {
        if (backends_.nvidia) {
            const auto& idx = backends_.nvidia->gpu_indices();
            return std::vector<int>(idx.begin(), idx.end());
        }
        if (backends_.amd) {
            const auto& idx = backends_.amd->gpu_indices();
            return std::vector<int>(idx.begin(), idx.end());
        }
        return {};
    }

    // ---- Static utility ----

    /** @brief Get total GPU count (NVIDIA + AMD). */
    static int get_device_count() {
        int count = 0;
        try { count += NvidiaGpuBackend::device_count(); } catch (...) {}
        try { count += AmdGpuBackend::device_count(); } catch (...) {}
        return count;
    }

    /** @brief Get the name of a GPU by index. */
    static std::string get_device_name(int gpu_index) {
        if (NvidiaGpuBackend::is_available()) {
            try { return NvidiaGpuBackend::get_device_name(gpu_index); }
            catch (...) {}
        }
        if (AmdGpuBackend::is_available()) {
            try { return AmdGpuBackend::get_device_name(gpu_index); }
            catch (...) {}
        }
        return "Unknown GPU " + std::to_string(gpu_index);
    }

    /** @brief Get architecture name (NVIDIA only). */
    static std::string get_architecture_name(int gpu_index) {
        if (NvidiaGpuBackend::is_available()) {
            try { return NvidiaGpuBackend::get_architecture_name(gpu_index); }
            catch (...) {}
        }
        return "Unknown";
    }

private:
    BackendPtrs backends_;
};

} // namespace zeus
