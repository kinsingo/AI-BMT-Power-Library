/**
 * @file main_gpu_amd.cpp
 * @brief AMD GPU 전용 전력/에너지 측정 예제.
 *
 * ROCm SMI 백엔드를 사용하여 AMD GPU의 전력 및 에너지를 측정합니다.
 *   - 디바이스 감지 및 정보 조회
 *   - 순간 전력 조회
 *   - 에너지 윈도우 측정 (begin_window / end_window)
 *   - 멀티 GPU 측정
 *   - 연속 전력 모니터링
 *
 * Build:
 *   mkdir build && cd build
 *   cmake .. -DCMAKE_BUILD_TYPE=Release -DZEUS_USE_NVML=OFF -DZEUS_USE_ROCM_SMI=ON
 *   make -j$(nproc)
 *
 * Run:
 *   ./bench_gpu_amd
 */

#include "power_monitor.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

static void simulate_workload(int duration_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
}

// ---------------------------------------------------------------------------
// 1. 디바이스 감지
// ---------------------------------------------------------------------------
static void example_device_detection() {
    std::cout << "\n========================================\n";
    std::cout << " [AMD] Device Detection\n";
    std::cout << "========================================\n";

    if (!zeus::AmdGpuBackend::is_available()) {
        std::cout << "  AMD GPU not available (ROCm SMI not compiled or "
                  << "no AMD GPU detected).\n";
        return;
    }

    int count = zeus::AmdGpuBackend::device_count();
    std::cout << "  AMD GPUs: " << count << " detected\n";
    for (int i = 0; i < count; ++i) {
        std::cout << "    [GPU " << i << "] "
                  << zeus::AmdGpuBackend::get_device_name(i) << "\n";
    }
}

// ---------------------------------------------------------------------------
// 2. GPU 전력 조회
// ---------------------------------------------------------------------------
static void example_power_query() {
    std::cout << "\n========================================\n";
    std::cout << " [AMD] Power Query\n";
    std::cout << "========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::AmdGPU});

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  GPU type: " << monitor.gpu_type() << "\n";

        auto indices = monitor.gpu_indices();
        for (int idx : indices) {
            std::cout << "\n  --- GPU " << idx << " ---\n";
            std::cout << "    Instant power: "
                      << monitor.get_instant_power(idx) << " W\n";
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
    std::cout << " [AMD] Energy Window Measurement\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::AmdGPU});

        monitor.begin_window("amd_bench");
        simulate_workload(3000);
        zeus::Measurement result = monitor.end_window("amd_bench");

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "  Duration:         " << result.elapsed_time << " s\n";
        std::cout << "  Total GPU energy: " << result.total_gpu_energy() << " J\n";
        for (const auto& kv : result.gpu_energy)
            std::cout << "    GPU " << kv.first << ": " << kv.second << " J\n";
    } catch (const std::runtime_error& e) {
        std::cout << "  Error: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// 4. 멀티 GPU 측정
// ---------------------------------------------------------------------------
static void example_multi_gpu() {
    int count = zeus::AmdGpuBackend::device_count();
    if (count < 2) {
        std::cout << "\n==========================================\n";
        std::cout << " [AMD] Multi-GPU (skipped, need 2+ GPUs)\n";
        std::cout << "==========================================\n";
        return;
    }

    std::cout << "\n==========================================\n";
    std::cout << " [AMD] Multi-GPU Monitoring\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::AmdGPU});

        monitor.begin_window("amd_multi");
        simulate_workload(2000);
        zeus::Measurement result = monitor.end_window("amd_multi");

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "  Duration:         " << result.elapsed_time << " s\n";
        std::cout << "  Total GPU energy: " << result.total_gpu_energy() << " J\n";
        for (const auto& kv : result.gpu_energy)
            std::cout << "    GPU " << kv.first << ": " << kv.second << " J\n";
    } catch (const std::runtime_error& e) {
        std::cout << "  Error: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// 5. 연속 전력 모니터링
// ---------------------------------------------------------------------------
static void example_continuous_monitoring() {
    std::cout << "\n==========================================\n";
    std::cout << " [AMD] Continuous Power Monitoring\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor::Config cfg;
        cfg.gpu_indices = {0};
        zeus::PowerMonitor monitor({zeus::DeviceType::AmdGPU}, cfg);

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
// main
// ---------------------------------------------------------------------------
int main() {
    try {
        std::cout << "============================================\n";
        std::cout << " AMD GPU Power/Energy Benchmark\n";
        std::cout << "============================================\n";
        std::cout << " Backend: ROCm SMI\n";

        if (!zeus::AmdGpuBackend::is_available()) {
            std::cerr << "\n[ERROR] No AMD GPU detected.\n";
            std::cerr << "  Ensure ROCm SMI is installed and the library\n";
            std::cerr << "  was built with -DZEUS_USE_ROCM_SMI=ON\n";
            return 1;
        }

        example_device_detection();
        example_power_query();
        example_energy_window();
        example_multi_gpu();
        example_continuous_monitoring();

        std::cout << "\n============================================\n";
        std::cout << " All AMD examples completed.\n";
        std::cout << "============================================\n";

    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
