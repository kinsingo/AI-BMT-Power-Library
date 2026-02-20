/**
 * @file monitor/energy_monitor.h
 * @brief Window-based energy measurement across multiple device backends.
 *
 * Reference: zeus/monitor/energy.py (ZeusMonitor.begin_window / end_window)
 *
 * This class orchestrates begin_window/end_window across all active backends.
 * It does NOT own the backends — it receives non-owning pointers from the
 * PowerMonitor facade, which manages backend lifetimes.
 *
 * Contains NO #ifdef guards. All platform logic is in the device backends.
 */

#pragma once

#include <chrono>
#include <map>
#include <stdexcept>
#include <string>

#include "../device/gpu_nvidia.h"
#include "../device/gpu_amd.h"
#include "../device/soc_jetson.h"
#include "../device/soc_apple.h"
#include "measurement.h"

namespace zeus {

/**
 * @brief Manages energy measurement windows across all device backends.
 *
 * Provides begin_window/end_window API that snapshots and diffs energy
 * counters from all registered backends.
 */
class EnergyMonitor {
public:
    /** Non-owning pointers to active backends (nullptr = not active). */
    struct BackendPtrs {
        NvidiaGpuBackend*  nvidia  = nullptr;
        AmdGpuBackend*     amd     = nullptr;
        JetsonSoCBackend*  jetson  = nullptr;
        AppleSoCBackend*   apple   = nullptr;
    };

    explicit EnergyMonitor(BackendPtrs backends)
        : backends_(backends) {}

    /**
     * @brief Start a measurement window.
     *
     * Takes energy counter snapshots from all active backends.
     * @param key Unique name for this window. Must not already be active.
     */
    void begin_window(const std::string& key) {
        if (windows_.count(key)) {
            throw std::runtime_error(
                "Window '" + key + "' already active. "
                "Call end_window(\"" + key + "\") first.");
        }

        WindowState state;
        state.start_time = get_timestamp();

        // GPU snapshots
        if (backends_.nvidia) {
            state.nvidia_snap = backends_.nvidia->snapshot_energy_j();
        }
        if (backends_.amd) {
            state.amd_snap = backends_.amd->snapshot_energy_j();
        }

        // SoC snapshots
        if (backends_.jetson) {
            state.jetson_snap = backends_.jetson->snapshot_energy_j();
        }
        if (backends_.apple) {
            state.apple_snap = backends_.apple->snapshot_energy_j();
        }

        windows_[key] = std::move(state);
    }

    /**
     * @brief End a measurement window and return the result.
     *
     * Computes energy deltas from all active backends since begin_window.
     * @param key Name of the window started with begin_window().
     * @return Measurement with per-device energy in Joules.
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
        if (backends_.nvidia) {
            auto gpu_delta = backends_.nvidia->compute_energy_delta_j(
                state.nvidia_snap, state.start_time, end_time, result.elapsed_time);
            for (const auto& kv : gpu_delta) {
                result.gpu_energy[kv.first] = kv.second;
            }
        }
        if (backends_.amd) {
            auto gpu_delta = backends_.amd->compute_energy_delta_j(state.amd_snap);
            for (const auto& kv : gpu_delta) {
                result.gpu_energy[kv.first] = kv.second;
            }
        }

        // === SoC energy ===
        if (backends_.jetson) {
            auto soc_delta = backends_.jetson->compute_energy_delta_j(state.jetson_snap);
            for (const auto& kv : soc_delta) {
                result.soc_energy[kv.first] = kv.second;
            }
        }
        if (backends_.apple) {
            auto soc_delta = backends_.apple->compute_energy_delta_j(state.apple_snap);
            for (const auto& kv : soc_delta) {
                result.soc_energy[kv.first] = kv.second;
            }
        }

        windows_.erase(it);
        return result;
    }

private:
    BackendPtrs backends_;

    /** Per-window state: start timestamp + backend snapshots. */
    struct WindowState {
        double start_time = 0.0;

        // GPU snapshots (gpu_index -> Joules)
        std::map<int, double> nvidia_snap;
        std::map<int, double> amd_snap;

        // SoC snapshots (metric_name -> Joules)
        std::map<std::string, double> jetson_snap;
        std::map<std::string, double> apple_snap;
    };

    std::map<std::string, WindowState> windows_;

    static double get_timestamp() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now.time_since_epoch()).count();
    }
};

} // namespace zeus
