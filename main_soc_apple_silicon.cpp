/**
 * @file main_soc_apple_silicon.cpp
 * @brief Apple Silicon SoC 전용 전력/에너지 측정 예제.
 *
 * IOReport API를 통해 Apple Silicon의 에너지를 측정합니다.
 *   - SoC 감지 및 가용 메트릭 조회
 *   - 순간 전력 조회 (SoC 각 메트릭)
 *   - 에너지 윈도우 측정 (begin_window / end_window)
 *   - 반복 측정
 *
 * Build (on macOS ARM64):
 *   mkdir build && cd build
 *   cmake .. -DCMAKE_BUILD_TYPE=Release -DZEUS_USE_NVML=OFF
 *   make -j$(sysctl -n hw.ncpu)
 *
 * Run:
 *   ./bench_soc_apple
 */

#include "power_monitor.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>

static void simulate_workload(int duration_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
}

// ---------------------------------------------------------------------------
// 1. 디바이스 감지
// ---------------------------------------------------------------------------
static void example_device_detection() {
    std::cout << "\n========================================\n";
    std::cout << " [Apple Silicon] Device Detection\n";
    std::cout << "========================================\n";

    if (!zeus::AppleSoCBackend::is_available()) {
        std::cout << "  Apple Silicon not detected.\n";
        std::cout << "  Requires macOS on ARM64 (M1/M2/M3/M4).\n";
        return;
    }

    std::cout << "  Apple Silicon SoC: available\n";

    // Show available metrics
    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::AppleSoC});
        auto metrics = monitor.soc_metrics();
        std::cout << "  Available energy metrics (" << metrics.size() << "):\n";
        for (const auto& m : metrics)
            std::cout << "    - " << m << "\n";
    } catch (const std::runtime_error& e) {
        std::cout << "  Error: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// 2. 순간 전력 조회
// ---------------------------------------------------------------------------
static void example_power_query() {
    std::cout << "\n========================================\n";
    std::cout << " [Apple Silicon] Power Query\n";
    std::cout << "========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::AppleSoC});

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  SoC type: " << monitor.soc_type() << "\n";
        std::cout << "  (IOReport: ~50ms sampling interval per query)\n";

        auto metrics = monitor.soc_metrics();
        for (const auto& m : metrics) {
            double power = monitor.get_instant_soc_power(m);
            std::cout << "    " << m << ": " << power << " W\n";
        }
    } catch (const std::runtime_error& e) {
        std::cout << "  Skipped: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// 3. 에너지 윈도우 측정
// ---------------------------------------------------------------------------
static void example_energy_window() {
    std::cout << "\n==========================================\n";
    std::cout << " [Apple Silicon] Energy Window Measurement\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::AppleSoC});

        monitor.begin_window("apple_bench");
        simulate_workload(3000);
        zeus::Measurement result = monitor.end_window("apple_bench");

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "  Duration:         " << result.elapsed_time << " s\n";
        std::cout << "  Total SoC energy: " << result.total_soc_energy() << " J\n";
        std::cout << "  Per-metric breakdown:\n";
        for (const auto& kv : result.soc_energy)
            std::cout << "    " << kv.first << ": " << kv.second << " J\n";
    } catch (const std::runtime_error& e) {
        std::cout << "  Error: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// 4. 반복 측정 (3회)
// ---------------------------------------------------------------------------
static void example_repeated_measurement() {
    std::cout << "\n==========================================\n";
    std::cout << " [Apple Silicon] Repeated Measurement (3 runs)\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::AppleSoC});

        std::cout << std::fixed << std::setprecision(4);

        for (int run = 1; run <= 3; ++run) {
            std::string key = "run_" + std::to_string(run);
            monitor.begin_window(key);
            simulate_workload(2000);
            zeus::Measurement result = monitor.end_window(key);

            std::cout << "  Run " << run << ": "
                      << result.total_soc_energy() << " J  ("
                      << result.elapsed_time << " s)\n";
            for (const auto& kv : result.soc_energy) {
                std::cout << "      " << kv.first << ": "
                          << kv.second << " J\n";
            }
        }
    } catch (const std::runtime_error& e) {
        std::cout << "  Error: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    try {
        std::cout << "============================================\n";
        std::cout << " Apple Silicon Power/Energy Benchmark\n";
        std::cout << "============================================\n";
        std::cout << " Backend: IOReport (IOKit)\n";

        if (!zeus::AppleSoCBackend::is_available()) {
            std::cerr << "\n[ERROR] Apple Silicon not detected.\n";
            std::cerr << "  This benchmark requires macOS on ARM64.\n";
            return 1;
        }

        example_device_detection();
        example_power_query();
        example_energy_window();
        example_repeated_measurement();

        std::cout << "\n============================================\n";
        std::cout << " All Apple Silicon examples completed.\n";
        std::cout << "============================================\n";

    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
