/**
 * @file main.cpp
 * @brief Example usage of the zeus::PowerMonitor C++ library.
 *
 * Demonstrates multi-device power/energy measurement:
 *   - NVIDIA GPU (NVML)
 *   - AMD GPU (ROCm SMI)
 *   - Intel CPU/DRAM (RAPL on Linux)
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
// In a real scenario, replace this with actual GPU compute (CUDA kernels, etc.)
// The sleep simulates wall-clock time during which devices draw power.
static void simulate_workload(int duration_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
}

// ---------------------------------------------------------------------------
// Example 1: Detect All Available Devices
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

    // CPU detection (via Config with GPU disabled)
    zeus::PowerMonitor::Config cfg;
    cfg.monitor_gpu = false;
    cfg.monitor_cpu = true;
    zeus::PowerMonitor cpu_probe(cfg);

    auto cpus = cpu_probe.cpu_indices();
    if (!cpus.empty()) {
        std::cout << "  CPUs (RAPL): " << cpus.size() << " socket(s) [";
        for (size_t i = 0; i < cpus.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << cpus[i];
        }
        std::cout << "]\n";
    } else {
        std::cout << "  CPUs (RAPL): not available"
#ifndef __linux__
                  << " (requires Linux)"
#endif
                  << "\n";
    }
}

// ---------------------------------------------------------------------------
// Example 2: GPU Power Query
// ---------------------------------------------------------------------------
static void example_gpu_query() {
    std::cout << "\n========================================\n";
    std::cout << " Example 2: GPU Power Query\n";
    std::cout << "========================================\n";

    int gpu_count = zeus::PowerMonitor::get_device_count();
    if (gpu_count == 0) {
        std::cout << "  Skipped: no GPU detected\n";
        return;
    }

    zeus::PowerMonitor monitor({0});

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
}

// ---------------------------------------------------------------------------
// Example 3: Multi-Device Measurement Window
// ---------------------------------------------------------------------------
static void example_multi_device_window() {
    std::cout << "\n==========================================\n";
    std::cout << " Example 3: Multi-Device Measurement\n";
    std::cout << "==========================================\n";

    // Default constructor monitors all GPUs + all CPUs (RAPL)
    zeus::PowerMonitor monitor;

    monitor.begin_window("workload");
    simulate_workload(3000);
    zeus::Measurement result = monitor.end_window("workload");

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  Duration: " << result.elapsed_time << " s\n";

    if (!result.gpu_energy.empty()) {
        std::cout << "  GPU energy: " << result.total_gpu_energy() << " J\n";
        for (const auto& kv : result.gpu_energy) {
            std::cout << "    GPU " << kv.first << ": "
                      << kv.second << " J\n";
        }
    }

    if (!result.cpu_energy.empty()) {
        std::cout << "  CPU energy: " << result.total_cpu_energy() << " J\n";
        for (const auto& kv : result.cpu_energy) {
            std::cout << "    CPU " << kv.first << ": "
                      << kv.second << " J\n";
        }
    }

    if (!result.dram_energy.empty()) {
        std::cout << "  DRAM energy: " << result.total_dram_energy() << " J\n";
        for (const auto& kv : result.dram_energy) {
            std::cout << "    DRAM " << kv.first << ": "
                      << kv.second << " J\n";
        }
    }

    std::cout << "  Total energy (all devices): "
              << result.total_energy() << " J\n";
}

// ---------------------------------------------------------------------------
// Example 4: Epoch + Step Measurement
// ---------------------------------------------------------------------------
static void example_epoch_steps() {
    std::cout << "\n==========================================\n";
    std::cout << " Example 4: Epoch + Step Measurement\n";
    std::cout << "==========================================\n";

    zeus::PowerMonitor monitor;

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

        // Average step energy
        double avg_step = 0.0;
        for (const auto& m : step_results)
            avg_step += m.total_energy();
        avg_step /= static_cast<double>(step_results.size());

        std::cout << "  Epoch " << epoch
                  << " | Total: " << epoch_result.total_energy() << " J";
        if (!epoch_result.gpu_energy.empty())
            std::cout << " | GPU: " << epoch_result.total_gpu_energy() << " J";
        if (!epoch_result.cpu_energy.empty())
            std::cout << " | CPU: " << epoch_result.total_cpu_energy() << " J";
        if (!epoch_result.dram_energy.empty())
            std::cout << " | DRAM: " << epoch_result.total_dram_energy() << " J";
        std::cout << " | Avg step: " << avg_step << " J\n";
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

    zeus::PowerMonitor monitor;  // all GPUs

    monitor.begin_window("multi");
    simulate_workload(2000);
    zeus::Measurement result = monitor.end_window("multi");

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  Total GPU energy: "
              << result.total_gpu_energy() << " J\n";
    for (const auto& kv : result.gpu_energy) {
        std::cout << "    GPU " << kv.first << ": "
                  << kv.second << " J\n";
    }
}

// ---------------------------------------------------------------------------
// Example 6: CPU/DRAM Only Monitoring (RAPL)
// ---------------------------------------------------------------------------
static void example_cpu_only() {
    std::cout << "\n==========================================\n";
    std::cout << " Example 6: CPU/DRAM Only (RAPL)\n";
    std::cout << "==========================================\n";

    zeus::PowerMonitor::Config cfg;
    cfg.monitor_gpu = false;
    cfg.monitor_cpu = true;
    zeus::PowerMonitor monitor(cfg);

    if (!monitor.has_cpu()) {
        std::cout << "  Skipped: RAPL not available"
#ifndef __linux__
                  << " (requires Linux)"
#endif
                  << "\n";
        return;
    }

    auto cpus = monitor.cpu_indices();
    std::cout << "  Monitoring " << cpus.size() << " CPU socket(s)\n";

    monitor.begin_window("cpu_test");
    simulate_workload(2000);
    zeus::Measurement result = monitor.end_window("cpu_test");

    std::cout << std::fixed << std::setprecision(4);
    if (!result.cpu_energy.empty()) {
        std::cout << "  CPU energy: " << result.total_cpu_energy() << " J\n";
        for (const auto& kv : result.cpu_energy)
            std::cout << "    CPU " << kv.first << ": "
                      << kv.second << " J\n";
    }
    if (!result.dram_energy.empty()) {
        std::cout << "  DRAM energy: " << result.total_dram_energy() << " J\n";
        for (const auto& kv : result.dram_energy)
            std::cout << "    DRAM " << kv.first << ": "
                      << kv.second << " J\n";
    }
}

// ---------------------------------------------------------------------------
// Example 7: Continuous Power Monitoring
// ---------------------------------------------------------------------------
static void example_continuous_monitoring() {
    std::cout << "\n==========================================\n";
    std::cout << " Example 7: Continuous Power Monitoring\n";
    std::cout << "==========================================\n";

    int gpu_count = zeus::PowerMonitor::get_device_count();
    if (gpu_count == 0) {
        std::cout << "  Skipped: no GPU for continuous power reading\n";
        return;
    }

    zeus::PowerMonitor monitor({0});

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Reading GPU 0 power every 500ms for 3 seconds...\n";

    for (int i = 0; i < 6; ++i) {
        double power = monitor.get_instant_power(0);
        std::cout << "    [" << (i * 0.5) << "s] GPU 0 power: "
                  << power << " W\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
        std::cout << " Supports: NVIDIA GPU, AMD GPU, CPU (RAPL)\n";

        example_detect_devices();
        example_gpu_query();
        example_multi_device_window();
        example_epoch_steps();
        example_multi_gpu();
        example_cpu_only();
        example_continuous_monitoring();

        std::cout << "\n============================================\n";
        std::cout << " All examples completed successfully.\n";
        std::cout << "============================================\n";

    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
