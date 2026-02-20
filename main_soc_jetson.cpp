/**
 * @file main_soc_jetson.cpp
 * @brief NVIDIA Jetson SoC 전용 전력/에너지 측정 예제.
 *
 * INA3221 센서를 통해 Jetson 플랫폼의 전력 레일을 측정합니다.
 *   - Jetson SoC 감지 및 가용 메트릭 조회
 *   - 순간 전력 조회 (SoC 각 레일)
 *   - 에너지 윈도우 측정 (begin_window / end_window)
 *   - 연속 전력 모니터링 (폴링 기반)
 *
 * Build (on Jetson):
 *   mkdir build && cd build
 *   cmake .. -DCMAKE_BUILD_TYPE=Release -DZEUS_USE_NVML=OFF
 *   make -j$(nproc)
 *
 * Run:
 *   ./bench_soc_jetson
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
    std::cout << " [Jetson] Device Detection\n";
    std::cout << "========================================\n";

    if (!zeus::JetsonSoCBackend::is_available()) {
        std::cout << "  Jetson SoC not detected.\n";
        std::cout << "  Requires /usr/lib/aarch64-linux-gnu/tegra or\n";
        std::cout << "  /etc/nv_tegra_release and INA3221 sysfs.\n";
        return;
    }

    std::cout << "  Jetson SoC: available\n";

    // Show available metrics
    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::JetsonSoC});
        auto metrics = monitor.soc_metrics();
        std::cout << "  Available power rails (" << metrics.size() << "):\n";
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
    std::cout << " [Jetson] Power Query\n";
    std::cout << "========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::JetsonSoC});

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  SoC type: " << monitor.soc_type() << "\n";

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
    std::cout << " [Jetson] Energy Window Measurement\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor::Config cfg;
        cfg.polling_interval_s = 0.1;  // 100ms polling
        zeus::PowerMonitor monitor({zeus::DeviceType::JetsonSoC}, cfg);

        monitor.begin_window("jetson_bench");
        simulate_workload(3000);
        zeus::Measurement result = monitor.end_window("jetson_bench");

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "  Duration:         " << result.elapsed_time << " s\n";
        std::cout << "  Total SoC energy: " << result.total_soc_energy() << " J\n";
        std::cout << "  Per-rail breakdown:\n";
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
    std::cout << " [Jetson] Repeated Measurement (3 runs)\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor::Config cfg;
        cfg.polling_interval_s = 0.05;  // 50ms polling for better accuracy
        zeus::PowerMonitor monitor({zeus::DeviceType::JetsonSoC}, cfg);

        std::cout << std::fixed << std::setprecision(4);

        for (int run = 1; run <= 3; ++run) {
            std::string key = "run_" + std::to_string(run);
            monitor.begin_window(key);
            simulate_workload(2000);
            zeus::Measurement result = monitor.end_window(key);

            std::cout << "  Run " << run << ": "
                      << result.total_soc_energy() << " J  ("
                      << result.elapsed_time << " s)\n";
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
        std::cout << " Jetson SoC Power/Energy Benchmark\n";
        std::cout << "============================================\n";
        std::cout << " Backend: INA3221 sysfs\n";

        if (!zeus::JetsonSoCBackend::is_available()) {
            std::cerr << "\n[ERROR] No Jetson SoC detected.\n";
            std::cerr << "  This benchmark requires an NVIDIA Jetson platform\n";
            std::cerr << "  with INA3221 power sensors.\n";
            return 1;
        }

        example_device_detection();
        example_power_query();
        example_energy_window();
        example_repeated_measurement();

        std::cout << "\n============================================\n";
        std::cout << " All Jetson examples completed.\n";
        std::cout << "============================================\n";

    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
