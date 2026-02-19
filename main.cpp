/**
 * @file main.cpp
 * @brief Example usage of the zeus::PowerMonitor C++ library.
 *
 * Demonstrates GPU power/energy measurement using NVML via the
 * header-only power_monitor.h library.
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
// The sleep simulates wall-clock time during which the GPU draws power.
static void simulate_workload(int duration_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
}

// ---------------------------------------------------------------------------
// Example 1: Basic Power Query
// ---------------------------------------------------------------------------
static void example_basic_query() {
    std::cout << "\n========================================\n";
    std::cout << " Example 1: Basic GPU Info & Power Query\n";
    std::cout << "========================================\n";

    int count = zeus::PowerMonitor::get_device_count();
    std::cout << "Detected " << count << " GPU(s):\n";

    for (int i = 0; i < count; ++i) {
        std::cout << "  [GPU " << i << "] "
                  << zeus::PowerMonitor::get_device_name(i)
                  << "  (Architecture: "
                  << zeus::PowerMonitor::get_architecture_name(i) << ")\n";
    }

    // Quick instant power reading
    zeus::PowerMonitor monitor({0});
    std::cout << "\n  Instant power (GPU 0): "
              << std::fixed << std::setprecision(2)
              << monitor.get_instant_power(0) << " W\n";

    std::cout << "  Energy counter supported (Volta+): "
              << (monitor.supports_energy_counter(0) ? "Yes" : "No") << "\n";

    if (monitor.supports_energy_counter(0)) {
        std::cout << "  Cumulative energy (GPU 0): "
                  << std::setprecision(2) << monitor.get_total_energy(0)
                  << " J (since driver load)\n";
    }

    try {
        std::cout << "  Power limit (GPU 0): "
                  << std::setprecision(1) << monitor.get_power_limit(0)
                  << " W\n";
    } catch (const std::exception& e) {
        std::cout << "  Power limit: N/A (" << e.what() << ")\n";
    }
}

// ---------------------------------------------------------------------------
// Example 2: Single Measurement Window
// ---------------------------------------------------------------------------
static void example_single_window() {
    std::cout << "\n==========================================\n";
    std::cout << " Example 2: Single Measurement Window\n";
    std::cout << "==========================================\n";

    zeus::PowerMonitor monitor({0});

    monitor.begin_window("workload");
    simulate_workload(3000); // 3 seconds
    zeus::Measurement result = monitor.end_window("workload");

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  Total energy consumed: "
              << result.total_energy() << " J\n";

    for (const auto& kv : result.gpu_energy) {
        std::cout << "    GPU " << kv.first << ": "
                  << kv.second << " J\n";
    }
}

// ---------------------------------------------------------------------------
// Example 3: Nested Windows (Epoch + Steps, like Python Zeus example)
// ---------------------------------------------------------------------------
static void example_epoch_steps() {
    std::cout << "\n==========================================\n";
    std::cout << " Example 3: Epoch + Step Measurement\n";
    std::cout << "==========================================\n";

    zeus::PowerMonitor monitor({0});

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

        // Compute average step energy
        double avg_step_energy = 0.0;
        for (const auto& m : step_results) {
            avg_step_energy += m.total_energy();
        }
        avg_step_energy /= static_cast<double>(step_results.size());

        std::cout << "  Epoch " << epoch
                  << " | Total: " << epoch_result.total_energy() << " J"
                  << " | Avg step: " << avg_step_energy << " J\n";
    }
}

// ---------------------------------------------------------------------------
// Example 4: Multi-GPU Monitoring
// ---------------------------------------------------------------------------
static void example_multi_gpu() {
    int count = zeus::PowerMonitor::get_device_count();
    if (count < 2) {
        std::cout << "\n==========================================\n";
        std::cout << " Example 4: Multi-GPU (skipped, need 2+ GPUs)\n";
        std::cout << "==========================================\n";
        return;
    }

    std::cout << "\n==========================================\n";
    std::cout << " Example 4: Multi-GPU Monitoring\n";
    std::cout << "==========================================\n";

    // Monitor all GPUs
    zeus::PowerMonitor monitor; // empty = all GPUs

    monitor.begin_window("multi");
    simulate_workload(2000);
    zeus::Measurement result = monitor.end_window("multi");

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  Total energy (all GPUs): "
              << result.total_energy() << " J\n";

    for (const auto& kv : result.gpu_energy) {
        std::cout << "    GPU " << kv.first << ": "
                  << kv.second << " J\n";
    }
}

// ---------------------------------------------------------------------------
// Example 5: Continuous Power Monitoring (polling)
// ---------------------------------------------------------------------------
static void example_continuous_monitoring() {
    std::cout << "\n==========================================\n";
    std::cout << " Example 5: Continuous Power Monitoring\n";
    std::cout << "==========================================\n";

    zeus::PowerMonitor monitor({0});

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Reading power every 500ms for 3 seconds...\n";

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
        std::cout << " Zeus C++ Power Monitor - Example Program\n";
        std::cout << "============================================\n";
        std::cout << " Based on: github.com/ml-energy/zeus\n";
        std::cout << " Library:  power_monitor.h (header-only)\n";

        example_basic_query();
        example_single_window();
        example_epoch_steps();
        example_multi_gpu();
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
