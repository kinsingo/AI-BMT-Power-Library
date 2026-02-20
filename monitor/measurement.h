/**
 * @file monitor/measurement.h
 * @brief Measurement result struct for energy monitoring.
 *
 * Pure data structure — contains NO #ifdef guards or platform-specific code.
 * Corresponds to zeus.monitor.energy.Measurement in the Python Zeus project.
 */

#pragma once

#include <map>
#include <string>

namespace zeus {

/**
 * @brief Result of an energy measurement window.
 *
 * Contains per-device energy consumption in Joules, organized by device type.
 * All energy values are in Joules. Elapsed time is in seconds.
 */
struct Measurement {
    /** GPU index -> energy consumed in Joules */
    std::map<int, double> gpu_energy;

    /** CPU socket index -> energy consumed in Joules (RAPL package) */
    std::map<int, double> cpu_energy;

    /** CPU socket index -> DRAM energy in Joules (RAPL dram sub-package) */
    std::map<int, double> dram_energy;

    /**
     * SoC metric name -> energy in Joules.
     *
     * Jetson keys:  "jetson_cpu", "jetson_gpu", "jetson_total"
     * Apple keys:   "apple_cpu_total", "apple_gpu", "apple_dram",
     *               "apple_gpu_sram", "apple_ane"
     */
    std::map<std::string, double> soc_energy;

    /** Wall-clock time of the measurement window (seconds). */
    double elapsed_time = 0.0;

    // ---- Aggregation helpers ----

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

} // namespace zeus
