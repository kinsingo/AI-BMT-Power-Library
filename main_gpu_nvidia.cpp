/**
 * @file main_gpu_nvidia.cpp
 * @brief NVIDIA GPU 전용 전력/에너지 측정 예제.
 *
 * NVML 백엔드를 사용하여 NVIDIA GPU의 전력 및 에너지를 측정합니다.
 *   - 디바이스 감지 및 정보 조회
 *   - 순간 전력 / 누적 에너지 / 전력 제한 조회
 *   - 에너지 윈도우 측정 (begin_window / end_window)
 *   - 멀티 GPU 측정
 *   - 연속 전력 모니터링
 *
 * Build:
 *   mkdir build && cd build
 *   cmake .. -DCMAKE_BUILD_TYPE=Release
 *   make -j$(nproc)
 *
 * Run:
 *   ./bench_gpu_nvidia
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
    std::cout << " [NVIDIA] Device Detection\n";
    std::cout << "========================================\n";

    if (!zeus::NvidiaGpuBackend::is_available()) {
        std::cout << "  NVIDIA GPU not available. Exiting.\n";
        return;
    }

    int count = zeus::NvidiaGpuBackend::device_count();
    std::cout << "  NVIDIA GPUs: " << count << " detected\n";
    for (int i = 0; i < count; ++i) {
        std::cout << "    [GPU " << i << "] "
                  << zeus::NvidiaGpuBackend::get_device_name(i)
                  << " (" << zeus::NvidiaGpuBackend::get_architecture_name(i)
                  << ")\n";
    }
}

// ---------------------------------------------------------------------------
// 2. GPU 전력 조회
// ---------------------------------------------------------------------------
static void example_power_query() {
    std::cout << "\n========================================\n";
    std::cout << " [NVIDIA] Power Query\n";
    std::cout << "========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::NvidiaGPU});

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  GPU type: " << monitor.gpu_type() << "\n";

        auto indices = monitor.gpu_indices();
        for (int idx : indices) {
            std::cout << "\n  --- GPU " << idx << " ---\n";
            std::cout << "    Instant power:   "
                      << monitor.get_instant_power(idx) << " W\n";
            std::cout << "    Energy counter:  "
                      << (monitor.supports_energy_counter(idx) ? "Yes (Volta+)" : "No")
                      << "\n";

            if (monitor.supports_energy_counter(idx)) {
                std::cout << "    Cumulative energy: "
                          << monitor.get_total_energy(idx)
                          << " J (since driver load)\n";
            }

            try {
                std::cout << "    Power limit:     "
                          << std::setprecision(1)
                          << monitor.get_power_limit(idx) << " W\n";
            } catch (const std::exception& e) {
                std::cout << "    Power limit:     N/A (" << e.what() << ")\n";
            }
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
    std::cout << " [NVIDIA] Energy Window Measurement\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::NvidiaGPU});

        monitor.begin_window("nvidia_bench");
        simulate_workload(3000);
        zeus::Measurement result = monitor.end_window("nvidia_bench");

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
    int count = zeus::NvidiaGpuBackend::device_count();
    if (count < 2) {
        std::cout << "\n==========================================\n";
        std::cout << " [NVIDIA] Multi-GPU (skipped, need 2+ GPUs)\n";
        std::cout << "==========================================\n";
        return;
    }

    std::cout << "\n==========================================\n";
    std::cout << " [NVIDIA] Multi-GPU Monitoring\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor monitor({zeus::DeviceType::NvidiaGPU});

        monitor.begin_window("multi_gpu");
        simulate_workload(2000);
        zeus::Measurement result = monitor.end_window("multi_gpu");

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
    std::cout << " [NVIDIA] Continuous Power Monitoring\n";
    std::cout << "==========================================\n";

    try {
        zeus::PowerMonitor::Config cfg;
        cfg.gpu_indices = {0};
        zeus::PowerMonitor monitor({zeus::DeviceType::NvidiaGPU}, cfg);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Reading GPU 0 power every 500ms for 3 seconds...\n";

        for (int i = 0; i < 6; ++i) {
            double power = monitor.get_instant_power(0);
            std::cout << "    [" << std::setprecision(2)
                      << (i * 0.5) << "s] GPU 0 power: "
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
        std::cout << " NVIDIA GPU Power/Energy Benchmark\n";
        std::cout << "============================================\n";
        std::cout << " Backend: NVML (CUDA Toolkit)\n";

        if (!zeus::NvidiaGpuBackend::is_available()) {
            std::cerr << "\n[ERROR] No NVIDIA GPU detected.\n";
            return 1;
        }

        example_device_detection();
        example_power_query();
        example_energy_window();
        example_multi_gpu();
        example_continuous_monitoring();

        std::cout << "\n============================================\n";
        std::cout << " All NVIDIA examples completed.\n";
        std::cout << "============================================\n";

    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
