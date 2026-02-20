/**
 * @file main.cpp
 * @brief Example usage of the zeus::PowerMonitor C++ library.
 *
 * Demonstrates enum-based multi-device power/energy measurement:
 *   - NVIDIA GPU (NVML)
 *   - AMD GPU (ROCm SMI)
 *   - Intel CPU/DRAM (RAPL on Linux)
 *   - Jetson SoC (INA3221 on Linux)
 *   - Apple Silicon (IOKit on macOS)
 *
 * Users select devices via DeviceType enum and catch runtime_error
 * if a device is unavailable.
 *
 * Build:
 *   mkdir build && cd build
 *   cmake .. && cmake --build .
 *
 * Run:
 *   ./power_monitor_example
 */

#include "power_monitor.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Simulated GPU Workload
// ---------------------------------------------------------------------------
static void simulate_workload(int duration_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
}

// ---------------------------------------------------------------------------
// Example 1: Device Detection (static queries, no enum needed)
// ---------------------------------------------------------------------------
static void example_detect_devices() {
    std::cout << "\n========================================\n";
    std::cout << " Example 1: Device Detection\n";
    std::cout << "========================================\n";

    // GPU detection
    int gpu_count = zeus::PowerMonitor::get_device_count();
    if (gpu_count > 0) {
        std::cout << "  GPUs: " << gpu_count << " detected\n";
        for (int i = 0; i < gpu_count; ++i) {
            std::cout << "    [GPU " << i << "] "
                      << zeus::PowerMonitor::get_device_name(i)
                      << " (" << zeus::PowerMonitor::get_architecture_name(i)
                      << ")\n";
        }
    } else {
        std::cout << "  GPUs: none detected\n";
    }

    // CPU detection (try creating with IntelCPU, catch if unavailable)
    try {
        zeus::PowerMonitor cpu_probe({zeus::DeviceType::IntelCPU});
        auto cpus = cpu_probe.cpu_indices();
        std::cout << "  CPUs (RAPL): " << cpus.size() << " socket(s) [";
        for (size_t i = 0; i < cpus.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << cpus[i];
        }
        std::cout << "]\n";
    } catch (const std::runtime_error& e) {
        std::cout << "  CPUs (RAPL): not available (" << e.what() << ")\n";
    }

    // SoC detection
    std::cout << "  Jetson SoC: "
              << (zeus::JetsonSoCBackend::is_available() ? "available" : "not detected")
              << "\n";
    std::cout << "  Apple SoC:  "
              << (zeus::AppleSoCBackend::is_available() ? "available" : "not detected")
              << "\n";
}

// ---------------------------------------------------------------------------
// Example 2: GPU Power Query (enum-based)
// ---------------------------------------------------------------------------
static void example_gpu_query() {
    std::cout << "\n========================================\n";
    std::cout << " Example 2: GPU Power Query\n";
    std::cout << "========================================\n";
    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::NvidiaGPU});

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  GPU type: " << monitor.gpu_type() << "\n";
        std::cout << "  Instant power (GPU 0): "
                  << monitor.get_instant_power(0) << " W\n";
        std::cout << "  Energy counter (Volta+): "
                  << (monitor.supports_energy_counter(0) ? "Yes" : "No") << "\n";

        if (monitor.supports_energy_counter(0)) {
            std::cout << "  Cumulative energy (GPU 0): "
                      << monitor.get_total_energy(0) << " J (since driver load)\n";
        }

        try {
            std::cout << "  Power limit (GPU 0): "
                      << std::setprecision(1)
                      << monitor.get_power_limit(0) << " W\n";
        } catch (const std::exception& e) {
            std::cout << "  Power limit: N/A (" << e.what() << ")\n";
        }
    } catch (const std::runtime_error& e) {
        std::cout << "  Skipped: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// Example 3: Multi-Device Measurement Window
// ---------------------------------------------------------------------------
static void example_multi_device_window() {
    std::cout << "\n==========================================\n";
    std::cout << " Example 3: Multi-Device Measurement\n";
    std::cout << "==========================================\n";

    // Try to monitor both NVIDIA GPU + Intel CPU
    std::vector<zeus::DeviceType> devices;
    if (zeus::NvidiaGpuBackend::is_available())
        devices.push_back(zeus::DeviceType::NvidiaGPU);
    if (zeus::AmdGpuBackend::is_available())
        devices.push_back(zeus::DeviceType::AmdGPU);
    if (zeus::RaplBackend::is_available())
        devices.push_back(zeus::DeviceType::IntelCPU);
    if (zeus::JetsonSoCBackend::is_available())
        devices.push_back(zeus::DeviceType::JetsonSoC);

    if (devices.empty()) {
        std::cout << "  Skipped: no devices available\n";
        return;
    }

    try {
        zeus::PowerMonitor monitor(devices);

        monitor.begin_window("workload");
        simulate_workload(3000);
        zeus::Measurement result = monitor.end_window("workload");

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "  Duration: " << result.elapsed_time << " s\n";

        if (!result.gpu_energy.empty()) {
            std::cout << "  GPU energy: " << result.total_gpu_energy() << " J\n";
            for (const auto& kv : result.gpu_energy)
                std::cout << "    GPU " << kv.first << ": " << kv.second << " J\n";
        }
        if (!result.cpu_energy.empty()) {
            std::cout << "  CPU energy: " << result.total_cpu_energy() << " J\n";
            for (const auto& kv : result.cpu_energy)
                std::cout << "    CPU " << kv.first << ": " << kv.second << " J\n";
        }
        if (!result.dram_energy.empty()) {
            std::cout << "  DRAM energy: " << result.total_dram_energy() << " J\n";
            for (const auto& kv : result.dram_energy)
                std::cout << "    DRAM " << kv.first << ": " << kv.second << " J\n";
        }
        if (!result.soc_energy.empty()) {
            std::cout << "  SoC energy: " << result.total_soc_energy() << " J\n";
            for (const auto& kv : result.soc_energy)
                std::cout << "    " << kv.first << ": " << kv.second << " J\n";
        }
        std::cout << "  Total energy: " << result.total_energy() << " J\n";
    } catch (const std::runtime_error& e) {
        std::cout << "  Error: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// Example 4: Epoch + Step Measurement
// ---------------------------------------------------------------------------
static void example_epoch_steps() {
    std::cout << "\n==========================================\n";
    std::cout << " Example 4: Epoch + Step Measurement\n";
    std::cout << "==========================================\n";

    std::vector<zeus::DeviceType> devices;
    if (zeus::NvidiaGpuBackend::is_available())
        devices.push_back(zeus::DeviceType::NvidiaGPU);
    if (zeus::RaplBackend::is_available())
        devices.push_back(zeus::DeviceType::IntelCPU);

    if (devices.empty()) {
        std::cout << "  Skipped: no devices available\n";
        return;
    }

    try {
        zeus::PowerMonitor monitor(devices);

        const int num_epochs = 3;
        const int steps_per_epoch = 5;
        const int step_duration_ms = 500;

        std::cout << std::fixed << std::setprecision(4);

        for (int epoch = 0; epoch < num_epochs; ++epoch) {
            monitor.begin_window("epoch");

            std::vector<zeus::Measurement> step_results;
            for (int step = 0; step < steps_per_epoch; ++step) {
                monitor.begin_window("step");
                simulate_workload(step_duration_ms);
                step_results.push_back(monitor.end_window("step"));
            }

            zeus::Measurement epoch_result = monitor.end_window("epoch");

            double avg_step = 0.0;
            for (const auto& m : step_results) avg_step += m.total_energy();
            avg_step /= static_cast<double>(step_results.size());

            std::cout << "  Epoch " << epoch
                      << " | Total: " << epoch_result.total_energy() << " J";
            if (!epoch_result.gpu_energy.empty())
                std::cout << " | GPU: " << epoch_result.total_gpu_energy() << " J";
            if (!epoch_result.cpu_energy.empty())
                std::cout << " | CPU: " << epoch_result.total_cpu_energy() << " J";
            std::cout << " | Avg step: " << avg_step << " J\n";
        }
    } catch (const std::runtime_error& e) {
        std::cout << "  Error: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// Example 5: Multi-GPU Monitoring
// ---------------------------------------------------------------------------
static void example_multi_gpu() {
    int count = zeus::PowerMonitor::get_device_count();
    if (count < 2) {
        std::cout << "\n==========================================\n";
        std::cout << " Example 5: Multi-GPU (skipped, need 2+ GPUs)\n";
        std::cout << "==========================================\n";
        return;
    }

    std::cout << "\n==========================================\n";
    std::cout << " Example 5: Multi-GPU Monitoring\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::NvidiaGPU});

        monitor.begin_window("multi");
        simulate_workload(2000);
        zeus::Measurement result = monitor.end_window("multi");

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "  Total GPU energy: "
                  << result.total_gpu_energy() << " J\n";
        for (const auto& kv : result.gpu_energy)
            std::cout << "    GPU " << kv.first << ": " << kv.second << " J\n";
    } catch (const std::runtime_error& e) {
        std::cout << "  Error: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// Example 6: CPU/DRAM Only Monitoring (RAPL)
// ---------------------------------------------------------------------------
static void example_cpu_only() {
    std::cout << "\n==========================================\n";
    std::cout << " Example 6: CPU/DRAM Only (RAPL)\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::IntelCPU});

        auto cpus = monitor.cpu_indices();
        std::cout << "  Monitoring " << cpus.size() << " CPU socket(s)\n";

        monitor.begin_window("cpu_test");
        simulate_workload(2000);
        zeus::Measurement result = monitor.end_window("cpu_test");

        std::cout << std::fixed << std::setprecision(4);
        if (!result.cpu_energy.empty()) {
            std::cout << "  CPU energy: " << result.total_cpu_energy() << " J\n";
            for (const auto& kv : result.cpu_energy)
                std::cout << "    CPU " << kv.first << ": " << kv.second << " J\n";
        }
        if (!result.dram_energy.empty()) {
            std::cout << "  DRAM energy: " << result.total_dram_energy() << " J\n";
            for (const auto& kv : result.dram_energy)
                std::cout << "    DRAM " << kv.first << ": " << kv.second << " J\n";
        }
    } catch (const std::runtime_error& e) {
        std::cout << "  Skipped: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// Example 7: Continuous Power Monitoring
// ---------------------------------------------------------------------------
static void example_continuous_monitoring() {
    std::cout << "\n==========================================\n";
    std::cout << " Example 7: Continuous Power Monitoring\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor::Config cfg;
        cfg.gpu_indices = {0};
        zeus::PowerMonitor monitor({zeus::DeviceType::NvidiaGPU}, cfg);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Reading GPU 0 power every 500ms for 3 seconds...\n";

        for (int i = 0; i < 6; ++i) {
            double power = monitor.get_instant_power(0);
            std::cout << "    [" << (i * 0.5) << "s] GPU 0 power: "
                      << power << " W\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    } catch (const std::runtime_error& e) {
        std::cout << "  Skipped: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// Example 8: Jetson SoC Monitoring
// ---------------------------------------------------------------------------
static void example_jetson_soc() {
    std::cout << "\n==========================================\n";
    std::cout << " Example 8: Jetson SoC Monitoring\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::JetsonSoC});

        auto metrics = monitor.soc_metrics();
        std::cout << "  Available SoC metrics: ";
        for (const auto& m : metrics) std::cout << m << " ";
        std::cout << "\n";

        monitor.begin_window("jetson_test");
        simulate_workload(2000);
        zeus::Measurement result = monitor.end_window("jetson_test");

        std::cout << std::fixed << std::setprecision(4);
        for (const auto& kv : result.soc_energy)
            std::cout << "    " << kv.first << ": " << kv.second << " J\n";
        std::cout << "  Total SoC energy: " << result.total_soc_energy() << " J\n";
    } catch (const std::runtime_error& e) {
        std::cout << "  Skipped: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    try {
        std::cout << "============================================\n";
        std::cout << " Zeus C++ Power Monitor - Multi-Device\n";
        std::cout << "============================================\n";
        std::cout << " Based on: github.com/ml-energy/zeus\n";
        std::cout << " Devices:  NVIDIA GPU, AMD GPU, CPU (RAPL),\n";
        std::cout << "           Jetson SoC, Apple Silicon\n";

        example_detect_devices();
        example_gpu_query();
        example_multi_device_window();
        example_epoch_steps();
        example_multi_gpu();
        example_cpu_only();
        example_continuous_monitoring();
        example_jetson_soc();

        std::cout << "\n============================================\n";
        std::cout << " All examples completed successfully.\n";
        std::cout << "============================================\n";

    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
