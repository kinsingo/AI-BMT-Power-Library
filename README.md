# Zeus Multi-Device Power Monitor

**GPU, CPU, DRAM, SoC** 전력(Power) 및 에너지(Energy) 측정을 위한 **C++ header-only 라이브러리** + **Python 구현**.  
Python [Zeus 프로젝트](https://github.com/ml-energy/zeus)의 전력 측정 기능을 C++와 Python으로 포팅한 것입니다.

> **"Zeus now supports CPU, DRAM, AMD GPU, Apple Silicon, and NVIDIA Jetson platform energy measurement"**

---

## 지원 디바이스

| 디바이스 | C++ (`power_monitor.h`) | Python (`main.py`) | 시스템 API | 플랫폼 |
|---|---|---|---|---|
| **NVIDIA GPU** | ✅ NVML | ✅ pynvml | `nvmlDeviceGetTotalEnergyConsumption` / `nvmlDeviceGetPowerUsage` | Windows / Linux |
| **AMD GPU** | ✅ ROCm SMI | ✅ amdsmi | `amdsmi_get_energy_count` / `amdsmi_get_power_info` | Linux (ROCm ≥ 6.1) |
| **Intel CPU** | ✅ RAPL sysfs | ✅ RAPL sysfs | `/sys/class/powercap/intel-rapl/*/energy_uj` | Linux |
| **Intel DRAM** | ✅ RAPL sysfs | ✅ RAPL sysfs | RAPL `dram` sub-package | Linux |
| **Apple Silicon SoC** | — | ✅ zeus_apple_silicon | 네이티브 에너지 모니터 (CPU, GPU, ANE, DRAM) | macOS (ARM) |
| **NVIDIA Jetson SoC** | — | ✅ INA3221 sysfs | `/sys/bus/i2c/drivers/ina3221x/*/iio:device*` | Linux (aarch64) |

---

## 참고한 Python 소스 파일

| Python 파일 | 역할 | C++ 대응 |
|---|---|---|
| `zeus/monitor/energy.py` | `ZeusMonitor` 클래스 (`begin_window` / `end_window`) | `zeus::PowerMonitor` |
| `zeus/device/gpu/nvidia.py` | NVML API 래퍼 | `power_monitor.h` 내 NVML 호출 |
| `zeus/device/gpu/amd.py` | AMD SMI API 래퍼 | `power_monitor.h` 내 ROCm SMI 호출 |
| `zeus/device/cpu/rapl.py` | Intel RAPL sysfs 래퍼 | `zeus::RaplBackend` (Linux) |
| `zeus/device/soc/apple.py` | Apple Silicon 에너지 모니터 | Python 전용 (`_AppleSiliconBackend`) |
| `zeus/device/soc/jetson.py` | Jetson INA3221 전력 레일 | Python 전용 (`_JetsonBackend`) |
| `zeus/monitor/power.py` | Pre-Volta GPU 전력 폴링 + 사다리꼴 적분 | `PowerMonitor::polling_thread_` |
| `zeus/device/gpu/common.py` | GPU 추상화 인터페이스 | `zeus::PowerMonitor` 통합 |
| `zeus/device/cpu/common.py` | CPU 추상화 (`CpuDramMeasurement`) | `zeus::Measurement::cpu_energy` / `dram_energy` |

---

## 아키텍처 개요

```
┌──────────────────────────────────────────────────────────────────┐
│  User Code (main.cpp / main.py)                                  │
│    monitor.begin_window("train")                                 │
│    // ... workload (GPU + CPU + DRAM) ...                        │
│    result = monitor.end_window("train")                          │
└─────────────────────┬────────────────────────────────────────────┘
                      │
┌─────────────────────▼────────────────────────────────────────────┐
│  zeus::PowerMonitor  (power_monitor.h / PowerMonitor class)      │
│                                                                  │
│  ┌─ NVIDIA GPU (NVML) ──────────────────────────────────┐        │
│  │  Volta+: nvmlDeviceGetTotalEnergyConsumption()        │        │
│  │  Pre-Volta: polling nvmlDeviceGetPowerUsage()         │        │
│  │            → trapezoidal integration → Joules         │        │
│  └───────────────────────────────────────────────────────┘        │
│                                                                  │
│  ┌─ AMD GPU (ROCm SMI) ─────────────────────────────────┐        │
│  │  rsmi_dev_energy_count_get() → energy counter         │        │
│  │  rsmi_dev_power_ave_get()    → instant power          │        │
│  └───────────────────────────────────────────────────────┘        │
│                                                                  │
│  ┌─ Intel CPU/DRAM (RAPL) ──────────────────────────────┐        │
│  │  /sys/class/powercap/intel-rapl/*/energy_uj           │        │
│  │  Package energy + optional DRAM sub-package           │        │
│  └───────────────────────────────────────────────────────┘        │
│                                                                  │
│  ┌─ Apple Silicon SoC (Python only) ────────────────────┐        │
│  │  zeus_apple_silicon extension                         │        │
│  │  CPU (E/P cores) + GPU + DRAM + ANE                   │        │
│  └───────────────────────────────────────────────────────┘        │
│                                                                  │
│  ┌─ NVIDIA Jetson SoC (Python only) ────────────────────┐        │
│  │  INA3221 sysfs power rails (polling + integration)    │        │
│  │  CPU rail + GPU rail + Total rail                     │        │
│  └───────────────────────────────────────────────────────┘        │
└──────────────────────────────────────────────────────────────────┘
```

### 측정 방식 (Python Zeus와 동일)

| 디바이스 | 1차 방식 | 폴백 |
|---|---|---|
| **NVIDIA GPU (Volta+)** | 하드웨어 에너지 카운터 (mJ 단위, `end - start`) | — |
| **NVIDIA GPU (Pre-Volta)** | 백그라운드 스레드 폴링 → 사다리꼴 적분 | `instant_power × elapsed_time` |
| **AMD GPU** | `amdsmi_get_energy_count` (μJ 단위, 누적 카운터) | `power × elapsed_time` |
| **Intel CPU/DRAM** | RAPL sysfs `energy_uj` (μJ 단위, 누적 카운터) | — |
| **Apple Silicon** | `zeus_apple_silicon` 네이티브 확장 (mJ 단위) | — |
| **Jetson** | INA3221 sysfs 전력 레일 폴링 → `power_mW × dt` 적분 | Voltage × Current 계산 |

---

## 사용된 API

### NVIDIA NVML

| 용도 | 함수 | 단위 |
|---|---|---|
| 누적 에너지 (Volta+) | `nvmlDeviceGetTotalEnergyConsumption()` | mJ |
| 순간 전력 | `nvmlDeviceGetPowerUsage()` | mW |
| 아키텍처 확인 | `nvmlDeviceGetArchitecture()` | — |
| 전력 제한 | `nvmlDeviceGetPowerManagementLimit()` | mW |
| GPU 이름/수 | `nvmlDeviceGetName()` / `nvmlDeviceGetCount()` | — |

### AMD ROCm SMI

| 용도 | 함수 | 단위 |
|---|---|---|
| 누적 에너지 | `rsmi_dev_energy_count_get()` / `amdsmi_get_energy_count()` | μJ |
| 평균/순간 전력 | `rsmi_dev_power_ave_get()` / `amdsmi_get_power_info()` | μW / W |

### Intel RAPL (Linux sysfs)

| 용도 | 경로 | 단위 |
|---|---|---|
| CPU 패키지 에너지 | `/sys/class/powercap/intel-rapl/intel-rapl:{N}/energy_uj` | μJ |
| DRAM 에너지 | `intel-rapl:{N}/intel-rapl:{N}:{M}/energy_uj` (name=`dram`) | μJ |

---

## 파일 구조

```
zeus/
├── power_monitor.h      # Header-only C++ 라이브러리 (NVIDIA + AMD + RAPL)
├── main.cpp             # C++ 예제 프로그램 (7개 예제)
├── CMakeLists.txt       # CMake 빌드 설정 (NVML/ROCm 옵션)
├── main.py              # Python 구현 (5개 백엔드 통합, 8개 예제)
├── requirements.txt     # Python 의존성
└── README.md            # 이 문서
```

---

## API Reference

### C++ — `zeus::PowerMonitor`

```cpp
#include "power_monitor.h"

// === 생성자 ===

// 방법 1: 간단 — GPU 인덱스 지정 (빈 벡터 = 모든 GPU), CPU(RAPL) 자동 활성화
zeus::PowerMonitor monitor({0});           // GPU 0 + 모든 CPU
zeus::PowerMonitor monitor;                // 모든 GPU + 모든 CPU
zeus::PowerMonitor monitor({0, 1}, 0.05);  // GPU 0,1 + 폴링 50ms

// 방법 2: Config 구조체 — GPU/CPU 개별 제어
zeus::PowerMonitor::Config cfg;
cfg.monitor_gpu = true;
cfg.monitor_cpu = true;                    // RAPL (Linux only)
cfg.gpu_indices = {0};
zeus::PowerMonitor monitor(cfg);

// CPU만 모니터링 (GPU 비활성화)
zeus::PowerMonitor::Config cpu_only;
cpu_only.monitor_gpu = false;
cpu_only.monitor_cpu = true;
zeus::PowerMonitor monitor(cpu_only);
```

#### 측정 윈도우 API

```cpp
monitor.begin_window("my_window");

// ... 작업 수행 (GPU + CPU + DRAM 동시 측정) ...

zeus::Measurement result = monitor.end_window("my_window");

// 결과 확인
double total   = result.total_energy();       // 전체 에너지 (J)
double gpu_j   = result.total_gpu_energy();   // GPU 합계 (J)
double cpu_j   = result.total_cpu_energy();   // CPU 합계 (J)
double dram_j  = result.total_dram_energy();  // DRAM 합계 (J)
double gpu0_j  = result.gpu_energy[0];        // GPU 0 에너지 (J)
double cpu0_j  = result.cpu_energy[0];        // CPU 소켓 0 에너지 (J)
double time_s  = result.elapsed_time;         // 측정 시간 (초)
```

#### 직접 조회 함수

```cpp
double power_w  = monitor.get_instant_power(0);       // 순간 전력 (W)
double energy_j = monitor.get_total_energy(0);         // 누적 에너지 (J, Volta+)
double limit_w  = monitor.get_power_limit(0);          // 전력 제한 (W)
bool   volta    = monitor.supports_energy_counter(0);  // Volta+ 여부
```

#### 디바이스 정보

```cpp
bool         has_gpu = monitor.has_gpu();       // GPU 백엔드 활성 여부
bool         has_cpu = monitor.has_cpu();       // CPU 백엔드 활성 여부
std::string  type    = monitor.gpu_type();      // "NVIDIA", "AMD", "None"
auto         gpus    = monitor.gpu_indices();   // 모니터링 중인 GPU 목록
auto         cpus    = monitor.cpu_indices();   // 모니터링 중인 CPU 소켓 목록
```

#### 정적 유틸리티

```cpp
int         count = zeus::PowerMonitor::get_device_count();       // NVIDIA + AMD
int         nv    = zeus::PowerMonitor::get_nvml_device_count();  // NVIDIA만
std::string name  = zeus::PowerMonitor::get_device_name(0);
std::string arch  = zeus::PowerMonitor::get_architecture_name(0);
```

### C++ — `zeus::Measurement`

```cpp
struct Measurement {
    std::map<int, double>         gpu_energy;    // GPU index → Joules
    std::map<int, double>         cpu_energy;    // CPU socket index → Joules
    std::map<int, double>         dram_energy;   // CPU socket index → DRAM Joules
    std::map<std::string, double> soc_energy;    // SoC metric → Joules
    double                        elapsed_time;  // 측정 시간 (초)

    double total_energy() const;       // 전체 합계 (GPU+CPU+DRAM+SoC)
    double total_gpu_energy() const;
    double total_cpu_energy() const;
    double total_dram_energy() const;
    double total_soc_energy() const;
};
```

### Python — `PowerMonitor`

```python
from main import PowerMonitor, Measurement

# 전체 디바이스 자동 감지
monitor = PowerMonitor()

# 특정 디바이스만
monitor = PowerMonitor(gpu_indices=[0], monitor_cpu=True, monitor_soc=True)

# CPU만 (GPU/SoC 비활성화)
monitor = PowerMonitor(monitor_gpu=False, monitor_soc=False)

# 측정
monitor.begin_window("train")
# ... workload ...
result = monitor.end_window("train")

print(f"Total:  {result.total_energy:.4f} J")
print(f"GPU:    {result.total_gpu_energy:.4f} J")
print(f"CPU:    {result.total_cpu_energy:.4f} J")
print(f"DRAM:   {result.total_dram_energy:.4f} J")
print(f"SoC:    {result.total_soc_energy:.4f} J")
```

### Python — `Measurement`

```python
@dataclass
class Measurement:
    gpu_energy:   Dict[int, float]           # GPU index → Joules
    cpu_energy:   Optional[Dict[int, float]] # CPU socket index → Joules (RAPL)
    dram_energy:  Optional[Dict[int, float]] # CPU socket index → DRAM Joules (RAPL)
    soc_energy:   Optional[Dict[str, float]] # SoC metric → Joules (Apple/Jetson)
    elapsed_time: float                      # 측정 시간 (초)

    total_energy: float      # property: GPU+CPU+DRAM+SoC 합계
    total_gpu_energy: float  # property
    total_cpu_energy: float  # property
    total_dram_energy: float # property
    total_soc_energy: float  # property
```

---

## 빌드 방법 (C++)

### 사전 요구사항

- **CMake** ≥ 3.18
- **C++17** 지원 컴파일러 (MSVC 2019+, GCC 7+, Clang 5+)
- **NVIDIA CUDA Toolkit** (NVML 사용 시, 기본 활성화)
- **ROCm** ≥ 6.1 (AMD GPU 사용 시, 수동 활성화)

### CMake 옵션

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `ZEUS_USE_NVML` | `ON` | NVIDIA NVML 지원 (CUDA Toolkit 필요) |
| `ZEUS_USE_ROCM_SMI` | `OFF` | AMD ROCm SMI 지원 (ROCm 필요) |

CPU (RAPL)는 Linux에서 자동 감지됩니다 (sysfs).

### Windows (Visual Studio) — NVIDIA

```powershell
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
.\Release\power_monitor_example.exe
```

### Linux — NVIDIA + RAPL

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./power_monitor_example
```

### Linux — AMD + RAPL

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DZEUS_USE_NVML=OFF -DZEUS_USE_ROCM_SMI=ON
make -j$(nproc)
./power_monitor_example
```

### Linux — CPU (RAPL) 전용

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DZEUS_USE_NVML=OFF
make -j$(nproc)
./power_monitor_example
```

### 다른 프로젝트에서 사용 (CMake)

```cmake
add_subdirectory(zeus)
target_link_libraries(my_app PRIVATE zeus_power_monitor)
```

---

## Python 실행 방법

```bash
# 기본 (NVIDIA GPU)
pip install -r requirements.txt
python main.py

# AMD GPU 추가
pip install amdsmi
python main.py

# Apple Silicon SoC 추가 (macOS ARM)
pip install zeus-apple-silicon
python main.py
```

---

## 예제 출력 (참고)

### C++ (NVIDIA GPU + Linux RAPL)

```
============================================
 Zeus C++ Power Monitor - Multi-Device
============================================
 Based on: github.com/ml-energy/zeus
 Supports: NVIDIA GPU, AMD GPU, CPU (RAPL)

========================================
 Example 1: Device Detection
========================================
  GPUs: 1 detected
    [GPU 0] NVIDIA GeForce RTX 4090 (Ada Lovelace)
  CPUs (RAPL): 1 socket(s) [0]

========================================
 Example 2: GPU Power Query
========================================
  GPU type: NVIDIA
  Instant power (GPU 0): 45.23 W
  Energy counter (Volta+): Yes
  Cumulative energy (GPU 0): 123456.78 J (since driver load)
  Power limit (GPU 0): 450.0 W

==========================================
 Example 3: Multi-Device Measurement
==========================================
  Duration: 3.00 s
  GPU energy: 135.6920 J
    GPU 0: 135.6920 J
  CPU energy: 42.3150 J
    CPU 0: 42.3150 J
  DRAM energy: 8.1230 J
    DRAM 0: 8.1230 J
  Total energy (all devices): 186.1300 J

==========================================
 Example 4: Epoch + Step Measurement
==========================================
  Epoch 0 | Total: 155.08 J | GPU: 113.08 J | CPU: 35.00 J | DRAM: 7.00 J | Avg step: 31.02 J
  Epoch 1 | Total: 155.07 J | GPU: 113.08 J | CPU: 35.00 J | DRAM: 6.99 J | Avg step: 31.01 J
  ...
```

### Python (NVIDIA GPU + 디바이스 검출)

```
============================================
 Zeus Power Monitor - Multi-Device Example
============================================
 Supports: NVIDIA GPU, AMD GPU, CPU (RAPL),
           Apple Silicon, NVIDIA Jetson

--- Detected Devices ---
  NVIDIA GPUs: 1
    [GPU 0] NVIDIA GeForce RTX 4090 (Ada Lovelace)
  AMD GPUs: not available (amdsmi not installed or no ROCm)
  CPUs (RAPL): not available (requires Linux, current: win32)
  Apple Silicon SoC: not available (not macOS)
  Jetson SoC: not available (not Jetson platform)
------------------------
```

---

## 라이센스

이 구현은 [Zeus 프로젝트](https://github.com/ml-energy/zeus) (Apache 2.0 License)의 전력 측정 아키텍처를 참고하여 작성되었습니다.
