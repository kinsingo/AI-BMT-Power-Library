# Zeus C++ Power Monitor

NVIDIA GPU 전력(Power) 및 에너지(Energy) 측정을 위한 **C++ header-only 라이브러리**.  
Python [Zeus 프로젝트](https://github.com/ml-energy/zeus)의 전력 측정 기능을 C++로 포팅한 것입니다.

---

## 참고한 Python 소스 파일

| Python 파일 | 역할 | C++ 대응 |
|---|---|---|
| `zeus/monitor/energy.py` | `ZeusMonitor` 클래스 (`begin_window` / `end_window`) | `zeus::PowerMonitor` |
| `zeus/device/gpu/nvidia.py` | NVML API 래퍼 (`get_total_energy_consumption`, `get_instant_power_usage`) | `power_monitor.h` 내부 NVML 호출 |
| `zeus/monitor/power.py` | Pre-Volta GPU용 백그라운드 전력 폴링 + 사다리꼴 적분 | `PowerMonitor::polling_thread_` |
| `zeus/device/gpu/common.py` | GPU 추상화 인터페이스 | `zeus::PowerMonitor` 통합 |
| `examples/jax/measure_energy.py` | 간단한 사용 예제 | `main.cpp` |

---

## 아키텍처 개요

```
┌─────────────────────────────────────────────────────────┐
│  User Code (main.cpp)                                   │
│    monitor.begin_window("train")                        │
│    // ... GPU workload ...                              │
│    result = monitor.end_window("train")                 │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│  zeus::PowerMonitor  (power_monitor.h)                  │
│                                                         │
│  ┌─ Volta+ GPU ──────────────────────────────────┐      │
│  │  nvmlDeviceGetTotalEnergyConsumption()         │      │
│  │  → end_energy - start_energy = Joules          │      │
│  └────────────────────────────────────────────────┘      │
│                                                         │
│  ┌─ Pre-Volta GPU ───────────────────────────────┐      │
│  │  Background thread polls:                      │      │
│  │    nvmlDeviceGetPowerUsage() → milliwatts      │      │
│  │  Trapezoidal integration → Joules              │      │
│  └────────────────────────────────────────────────┘      │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│  NVIDIA NVML C API  (nvml.h / nvml.lib)                 │
└─────────────────────────────────────────────────────────┘
```

### 측정 방식 (Python Zeus와 동일)

1. **Volta+ GPU** (Architecture ≥ 5): `nvmlDeviceGetTotalEnergyConsumption()` 하드웨어 에너지 카운터 사용
   - `begin_window()`: 누적 에너지 값 스냅샷 (millijoules)
   - `end_window()`: `에너지 = end_snapshot - start_snapshot`
   - 가장 정확하고 오버헤드가 거의 없음

2. **Pre-Volta GPU** (Pascal 이하): 백그라운드 스레드가 `nvmlDeviceGetPowerUsage()`를 주기적으로 폴링
   - 수집된 `(timestamp, power)` 쌍을 사다리꼴 적분(trapezoidal integration)하여 에너지 계산
   - 샘플이 부족하면 `instant_power × elapsed_time`으로 폴백

---

## 사용된 NVML API

| 용도 | NVML 함수 | 단위 | 비고 |
|---|---|---|---|
| 누적 에너지 (Volta+) | `nvmlDeviceGetTotalEnergyConsumption()` | mJ | 드라이버 로드 이후 누적 |
| 순간 전력 | `nvmlDeviceGetPowerUsage()` | mW | 모든 GPU 지원 |
| 아키텍처 확인 | `nvmlDeviceGetArchitecture()` | — | Volta+ 여부 판단 |
| 전력 제한 | `nvmlDeviceGetPowerManagementLimit()` | mW | 설정된 TDP |
| GPU 이름 | `nvmlDeviceGetName()` | — | — |
| GPU 수 | `nvmlDeviceGetCount()` | — | — |

---

## 파일 구조

```
zeus/
├── power_monitor.h      # Header-only C++ 라이브러리 (핵심)
├── main.cpp             # C++ 예제 프로그램
├── CMakeLists.txt       # CMake 빌드 설정
├── main.py              # Python 동등 구현 (pynvml 직접 사용)
├── requirements.txt     # Python 의존성
└── README.md            # 이 문서
```

---

## API Reference

### `zeus::PowerMonitor`

```cpp
#include "power_monitor.h"

// 생성자: GPU 인덱스 지정 (빈 벡터 = 모든 GPU)
zeus::PowerMonitor monitor({0});         // GPU 0만 모니터링
zeus::PowerMonitor monitor;              // 모든 GPU 모니터링
zeus::PowerMonitor monitor({0, 1}, 0.05); // GPU 0,1, 폴링 50ms
```

#### 측정 윈도우 API

```cpp
// 측정 시작
monitor.begin_window("my_window");

// ... GPU 작업 수행 ...

// 측정 종료 및 결과 반환
zeus::Measurement result = monitor.end_window("my_window");

// 결과 확인
double total_joules = result.total_energy();       // 전체 에너지 (J)
double gpu0_joules  = result.gpu_energy[0];        // GPU 0 에너지 (J)
```

#### 직접 조회 함수

```cpp
double power_w  = monitor.get_instant_power(0);    // 순간 전력 (W)
double energy_j = monitor.get_total_energy(0);     // 누적 에너지 (J, Volta+)
double limit_w  = monitor.get_power_limit(0);      // 전력 제한 (W)
bool   volta    = monitor.supports_energy_counter(0); // Volta+ 여부
```

#### 정적 유틸리티

```cpp
int         count = zeus::PowerMonitor::get_device_count();
std::string name  = zeus::PowerMonitor::get_device_name(0);
std::string arch  = zeus::PowerMonitor::get_architecture_name(0);
```

### `zeus::Measurement`

```cpp
struct Measurement {
    std::map<int, double> gpu_energy;  // GPU index → Joules
    double total_energy() const;       // 전체 합계
};
```

---

## 빌드 방법

### 사전 요구사항

- **NVIDIA CUDA Toolkit** (NVML 포함, 보통 CUDA 설치 시 자동 포함)
- **CMake** ≥ 3.18
- **C++17** 지원 컴파일러 (MSVC 2019+, GCC 7+, Clang 5+)

### Windows (Visual Studio)

```powershell
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
.\Release\power_monitor_example.exe
```

### Windows (Ninja / Developer Command Prompt)

```powershell
mkdir build
cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .
.\power_monitor_example.exe
```

### Linux

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./power_monitor_example
```

### 다른 프로젝트에서 사용 (CMake)

```cmake
# power_monitor.h를 include 경로에 추가하고 NVML 링크
add_subdirectory(zeus)                    # 또는 직접 경로 지정
target_link_libraries(my_app PRIVATE zeus_power_monitor)
```

또는 직접:

```cmake
find_package(CUDAToolkit REQUIRED)
target_include_directories(my_app PRIVATE /path/to/zeus)
target_link_libraries(my_app PRIVATE CUDA::nvml)
```

---

## Python 동등 구현

`main.py`는 동일한 API를 Python(`pynvml`)으로 구현한 것입니다.

```bash
pip install -r requirements.txt
python main.py
```

---

## 예제 출력 (참고)

```
============================================
 Zeus C++ Power Monitor - Example Program
============================================
 Based on: github.com/ml-energy/zeus
 Library:  power_monitor.h (header-only)

========================================
 Example 1: Basic GPU Info & Power Query
========================================
Detected 1 GPU(s):
  [GPU 0] NVIDIA GeForce RTX 4090  (Architecture: Ada Lovelace)

  Instant power (GPU 0): 45.23 W
  Energy counter supported (Volta+): Yes
  Cumulative energy (GPU 0): 123456.78 J (since driver load)
  Power limit (GPU 0): 450.0 W

==========================================
 Example 2: Single Measurement Window
==========================================
  Total energy consumed: 135.6920 J
    GPU 0: 135.6920 J

==========================================
 Example 3: Epoch + Step Measurement
==========================================
  Epoch 0 | Total: 113.0768 J | Avg step: 22.6152 J
  Epoch 1 | Total: 113.0754 J | Avg step: 22.6148 J
  Epoch 2 | Total: 113.0762 J | Avg step: 22.6150 J
  ...
```

---

## 라이센스

이 C++ 구현은 [Zeus 프로젝트](https://github.com/ml-energy/zeus) (Apache 2.0 License)의 전력 측정 아키텍처를 참고하여 작성되었습니다.
