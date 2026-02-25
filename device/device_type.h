/**
 * @file device/device_type.h
 * @brief DeviceType enum for selecting which hardware to monitor.
 *
 * Users pass a vector of DeviceType values to PowerMonitor to specify
 * which devices to initialize. If a requested device is unavailable
 * (not compiled or not present), the constructor throws std::runtime_error.
 *
 * This file contains NO #ifdef guards — all platform logic is encapsulated
 * inside the individual device backend headers.
 */

#pragma once

#include <string>
#include <vector>

namespace zeus {

/**
 * @brief Hardware device types supported by the power monitoring library.
 *
 * Usage:
 * @code
 *   // Monitor NVIDIA GPU
 *   zeus::PowerMonitor monitor({zeus::DeviceType::NvidiaGPU});
 *
 *   // Monitor Jetson SoC only
 *   zeus::PowerMonitor monitor({zeus::DeviceType::JetsonSoC});
 * @endcode
 */
enum class DeviceType {
    NvidiaGPU,  ///< NVIDIA GPU via NVML (requires CUDA Toolkit at compile time)
    JetsonSoC,  ///< NVIDIA Jetson SoC via INA3221 sensor (Linux only)
    AppleSoC,   ///< Apple Silicon via IOKit (macOS only)
};

/**
 * @brief Convert DeviceType to a human-readable string.
 */
inline std::string device_type_to_string(DeviceType type) {
    switch (type) {
        case DeviceType::NvidiaGPU: return "NVIDIA GPU (NVML)";
        case DeviceType::JetsonSoC: return "Jetson SoC (INA3221)";
        case DeviceType::AppleSoC:  return "Apple Silicon (IOKit)";
        default:                    return "Unknown Device";
    }
}

} // namespace zeus
