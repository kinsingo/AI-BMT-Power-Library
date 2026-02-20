/**
 * @file device/gpu_amd.h
 * @brief AMD GPU monitoring backend via ROCm SMI.
 *
 * Reference: zeus/device/gpu/amd.py
 *
 * All #ifdef guards are contained within this file.
 * The class AmdGpuBackend always exists — if ROCm SMI was not compiled,
 * the constructor throws std::runtime_error with a clear message.
 */

#pragma once

// ---------------------------------------------------------------------------
// Auto-detect ROCm SMI availability (opt-in via -DZEUS_USE_ROCM_SMI)
// ---------------------------------------------------------------------------
#ifdef ZEUS_USE_ROCM_SMI
  #if defined(__has_include)
    #if __has_include(<rocm_smi/rocm_smi.h>)
      #include <rocm_smi/rocm_smi.h>
      #ifndef ZEUS_HAS_ROCM_SMI
        #define ZEUS_HAS_ROCM_SMI 1
      #endif
    #endif
  #else
    #include <rocm_smi/rocm_smi.h>
    #ifndef ZEUS_HAS_ROCM_SMI
      #define ZEUS_HAS_ROCM_SMI 1
    #endif
  #endif
#endif

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace zeus {

// ---------------------------------------------------------------------------
// ROCm SMI helpers
// ---------------------------------------------------------------------------
#ifdef ZEUS_HAS_ROCM_SMI

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

#endif // ZEUS_HAS_ROCM_SMI

// ---------------------------------------------------------------------------
// AmdGpuBackend — always defined, throws if ROCm SMI not compiled
// ---------------------------------------------------------------------------

/**
 * @brief AMD GPU monitoring backend via ROCm SMI.
 *
 * If ROCm SMI was not compiled, is_compiled() returns false
 * and the constructor throws std::runtime_error.
 */
class AmdGpuBackend {
public:
    static bool is_compiled() {
#ifdef ZEUS_HAS_ROCM_SMI
        return true;
#else
        return false;
#endif
    }

    static bool is_available() {
#ifdef ZEUS_HAS_ROCM_SMI
        try {
            RsmiContext::instance();
            uint32_t count = 0;
            rsmi_num_monitor_devices(&count);
            return count > 0;
        } catch (...) { return false; }
#else
        return false;
#endif
    }

    static int device_count() {
#ifdef ZEUS_HAS_ROCM_SMI
        RsmiContext::instance();
        uint32_t count = 0;
        rsmi_num_monitor_devices(&count);
        return static_cast<int>(count);
#else
        return 0;
#endif
    }

    // ---- Construction ----

    explicit AmdGpuBackend(const std::vector<int>& gpu_indices = {}) {
#ifdef ZEUS_HAS_ROCM_SMI
        RsmiContext::instance();
        uint32_t count = 0;
        rsmi_num_monitor_devices(&count);

        std::vector<int> indices = gpu_indices;
        if (indices.empty()) {
            for (uint32_t i = 0; i < count; ++i)
                indices.push_back(static_cast<int>(i));
        }

        for (int idx : indices) {
            if (idx < 0 || static_cast<uint32_t>(idx) >= count) {
                throw std::runtime_error(
                    "AMD GPU " + std::to_string(idx) + " out of range [0, "
                    + std::to_string(count) + ")");
            }
        }
        gpu_indices_ = std::move(indices);
#else
        (void)gpu_indices;
        throw std::runtime_error(
            "AMD GPU monitoring requested but ROCm SMI was not compiled. "
            "Rebuild with ZEUS_USE_ROCM_SMI=ON (CMake).");
#endif
    }

    AmdGpuBackend(const AmdGpuBackend&) = delete;
    AmdGpuBackend& operator=(const AmdGpuBackend&) = delete;

    // ---- Query methods ----

    const std::vector<int>& gpu_indices() const {
#ifdef ZEUS_HAS_ROCM_SMI
        return gpu_indices_;
#else
        static const std::vector<int> empty;
        return empty;
#endif
    }

    double get_instant_power_w(int gpu_index) const {
#ifdef ZEUS_HAS_ROCM_SMI
        uint64_t power_uW = 0;
        rsmi_check(rsmi_dev_power_ave_get(
            static_cast<uint32_t>(gpu_index), 0, &power_uW),
            "rsmi_dev_power_ave_get");
        return static_cast<double>(power_uW) / 1e6;  // uW -> W
#else
        (void)gpu_index;
        throw std::runtime_error("ROCm SMI not compiled");
#endif
    }

    double get_total_energy_j(int gpu_index) const {
#ifdef ZEUS_HAS_ROCM_SMI
        uint64_t energy_uj = 0;
        float counter_resolution = 0.0f;
        uint64_t timestamp = 0;
        rsmi_check(rsmi_dev_energy_count_get(
            static_cast<uint32_t>(gpu_index),
            &energy_uj, &counter_resolution, &timestamp),
            "rsmi_dev_energy_count_get");
        return static_cast<double>(energy_uj)
             * static_cast<double>(counter_resolution) / 1e6;
#else
        (void)gpu_index;
        throw std::runtime_error("ROCm SMI not compiled");
#endif
    }

    // ---- Energy snapshot for windows ----

    std::map<int, double> snapshot_energy_j() const {
        std::map<int, double> snap;
#ifdef ZEUS_HAS_ROCM_SMI
        for (int idx : gpu_indices_) {
            snap[idx] = get_total_energy_j(idx);
        }
#endif
        return snap;
    }

    std::map<int, double> compute_energy_delta_j(
        const std::map<int, double>& start_snap) const
    {
        std::map<int, double> result;
#ifdef ZEUS_HAS_ROCM_SMI
        for (int idx : gpu_indices_) {
            double end_e = get_total_energy_j(idx);
            auto it = start_snap.find(idx);
            double start_e = (it != start_snap.end()) ? it->second : 0.0;
            result[idx] = end_e - start_e;
        }
#else
        (void)start_snap;
#endif
        return result;
    }

    // ---- Static device info ----

    static std::string get_device_name(int gpu_index) {
#ifdef ZEUS_HAS_ROCM_SMI
        RsmiContext::instance();
        char name[256];
        rsmi_check(rsmi_dev_name_get(gpu_index, name, sizeof(name)));
        return std::string(name);
#else
        (void)gpu_index;
        return "Unknown AMD GPU";
#endif
    }

private:
#ifdef ZEUS_HAS_ROCM_SMI
    std::vector<int> gpu_indices_;
#endif
};

} // namespace zeus
