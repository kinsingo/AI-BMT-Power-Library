# Zeus Multi-Device Power Monitor

**GPU, CPU, DRAM, SoC** 전력(Power) 및 에너지(Energy) 측정을 위한 **C++ header-only 라이브러리**.  
Python [Zeus 프로젝트](https://github.com/ml-energy/zeus)의 전력 측정 기능을 C++로 포팅한 것입니다.

> **"Zeus now supports CPU, DRAM, AMD GPU, Apple Silicon, and NVIDIA Jetson platform energy measurement"**

---

## 지원 디바이스

| 디바이스 | C++ 지원 | 시스템 API | 플랫폼 |
|---|---|---|---|
| **NVIDIA GPU** | ✅ `NvidiaGpuBackend` | NVML (`nvmlDeviceGetTotalEnergyConsumption` / `nvmlDeviceGetPowerUsage`) | Windows / Linux |
| **AMD GPU** | ✅ `AmdGpuBackend` | ROCm SMI (`rsmi_dev_energy_count_get` / `rsmi_dev_power_ave_get`) | Linux (ROCm >= 6.1) |
| **Intel CPU** | ✅ `RaplBackend` | RAPL sysfs (`/sys/class/powercap/intel-rapl/*/energy_uj`) | Linux |
| **Intel DRAM** | ✅ `RaplBackend` | RAPL `dram` sub-package | Linux |
| **Apple Silicon SoC** | ✅ `AppleSoCBackend` | IOReport private API via `dlsym` (CPU, GPU, ANE, DRAM) | macOS (ARM64) |
| **NVIDIA Jetson SoC** | ✅ `JetsonSoCBackend` | INA3221 sysfs (`/sys/bus/i2c/drivers/ina3221x/`) | Linux (aarch64) |

---

## 참고한 Python 소스 파일

| Python 파일 | 역할 | C++ 대응 |
|---|---|---|
| `zeus/monitor/energy.py` | `ZeusMonitor` (`begin_window` / `end_window`) | `monitor/energy_monitor.h` (`EnergyMonitor`) |
| `zeus/device/gpu/nvidia.py` | NVML API 래퍼 | `device/gpu_nvidia.h` (`NvidiaGpuBackend`) |
| `zeus/device/gpu/amd.py` | AMD SMI API 래퍼 | `device/gpu_amd.h` (`AmdGpuBackend`) |
| `zeus/device/cpu/rapl.py` | Intel RAPL sysfs 래퍼 | `device/cpu_rapl.h` (`RaplBackend`) |
| `zeus/device/soc/apple.py` | Apple Silicon 에너지 모니터 | `device/soc_apple.h` (`AppleSoCBackend`) |
| `zeus/device/soc/jetson.py` | Jetson INA3221 전력 레일 | `device/soc_jetson.h` (`JetsonSoCBackend`) |
| `zeus/monitor/power.py` | Pre-Volta GPU 전력 폴링 + 사다리꼴 적분 | `device/gpu_nvidia.h` (polling thread) |
| `zeus/device/gpu/common.py` | GPU 추상화 인터페이스 | `power_monitor.h` (facade) |
| `zeus/device/cpu/common.py` | CPU 추상화 (`CpuDramMeasurement`) | `monitor/measurement.h` (`Measurement`) |

---

## 아키텍처 개요

```

  User Code (main.cpp)                                            
    #include "power_monitor.h"                                    
    zeus::PowerMonitor monitor({zeus::DeviceType::NvidiaGPU,      
                                zeus::DeviceType::IntelCPU});     
    monitor.begin_window("train");                                
    // ... workload ...                                           
    auto result = monitor.end_window("train");                    

                      

  power_monitor.h  — Facade (DeviceType enum  backend 생성)      
   monitor/energy_monitor.h   begin_window / end_window 오케스트
   monitor/power_query.h      순간 전력 조회                    
                                                                  
   device/gpu_nvidia.h         
    Volta+: nvmlDeviceGetTotalEnergyConsumption()                
    Pre-Volta: polling thread  trapezoidal integration          
          
                                                                  
   device/gpu_amd.h         
    rsmi_dev_energy_count_get()  energy counter                 
    rsmi_dev_power_ave_get()     instant power                  
          
                                                                  
   device/cpu_rapl.h         
    /sys/class/powercap/intel-rapl/*/energy_uj                   
    Package energy + optional DRAM sub-package                   
    Wraparound detection via max_energy_range_uj                 
          
                                                                  
   device/soc_jetson.h         
    INA3221 sysfs power rails (polling + integration)            
    CPU rail + GPU rail + Total rail                             
          
                                                                  
   device/soc_apple.h         
    IOReport private API via dlsym (IOKit.framework)             
    CPU (E/P cores) + GPU + DRAM + ANE + GPU SRAM                
          

```

### 측정 방식 (Python Zeus와 동일)

| 디바이스 | 1차 방식 | 폴백 |
|---|---|---|
| **NVIDIA GPU (Volta+)** | 하드웨어 에너지 카운터 (mJ 단위, `end - start`) | — |
| **NVIDIA GPU (Pre-Volta)** | 백그라운드 스레드 폴링 -> 사다리꼴 적분 | `instant_power x elapsed_time` |
| **AMD GPU** | `rsmi_dev_energy_count_get` (uJ x resolution, 누적 카운터) | — |
| **Intel CPU/DRAM** | RAPL sysfs `energy_uj` (μJ 단위, 랩어라운드 보정 포함) | — |
| **Apple Silicon** | IOReport "Energy Model" 채널 그룹 (nJ 단위) | — |
| **Jetson** | INA3221 sysfs 전력 레일 폴링 -> `power_mW x dt` 적분 | Voltage x Current 계산 |

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
| 누적 에너지 | `rsmi_dev_energy_count_get()` | uJ (x counter_resolution) |
| 평균 전력 | `rsmi_dev_power_ave_get()` | μW |

### Intel RAPL (Linux sysfs)

| 용도 | 경로 | 단위 |
|---|---|---|
| CPU 패키지 에너지 | `/sys/class/powercap/intel-rapl/intel-rapl:{N}/energy_uj` | μJ |
| DRAM 에너지 | `intel-rapl:{N}/intel-rapl:{N}:{M}/energy_uj` (name=`dram`) | μJ |
| 최대 카운터 범위 | `max_energy_range_uj` (랩어라운드 감지용) | μJ |

### Apple IOReport (macOS ARM64)

| 용도 | 함수 (dlsym) | 비고 |
|---|---|---|
| 채널 구독 | `IOReportCopyChannelsInGroup("Energy Model")` | 에너지 채널 그룹 |
| 샘플 수집 | `IOReportCreateSamples()` | 시점별 스냅샷 |
| 델타 계산 | `IOReportCreateSamplesDelta()` | 두 샘플 간 차이 (nJ) |
| 채널 이름 | `IOReportChannelGetChannelName()` | ECPU, PCPU, GPU, DRAM, ANE |
| 값 읽기 | `IOReportSimpleGetIntegerValue()` | nJ -> J 변환 |

### Jetson INA3221 (Linux sysfs)

| 용도 | 경로 패턴 | 단위 |
|---|---|---|
| 직접 전력 | `power{N}_input` 또는 `in_power{N}_input` | mW |
| 전압 x 전류 | `in{N}_input` x `curr{N}_input` | mV x mA / 1000 = mW |
| 레일 이름 | `in{N}_label` 또는 `rail_name_{N}` | — |

---

## 파일 구조

```
AI-BMT-Power-Library/
 power_monitor.h              # Facade — 사용자가 include할 유일한 헤더
 device/
    device_type.h            # DeviceType enum (NvidiaGPU, AmdGPU, IntelCPU, ...)
    gpu_nvidia.h             # NVIDIA NVML 백엔드 (Volta+ hw 카운터 + Pre-Volta 폴링)
    gpu_amd.h                # AMD ROCm SMI 백엔드
    cpu_rapl.h               # Intel RAPL sysfs 백엔드 (랩어라운드 보정)
    soc_jetson.h             # Jetson INA3221 백엔드 (폴링 스레드)
    soc_apple.h              # Apple Silicon IOReport 백엔드 (dlsym)
 monitor/
    measurement.h            # Measurement 결과 구조체
    energy_monitor.h         # begin_window / end_window 오케스트레이터
    power_query.h            # 순간 전력 및 디바이스 정보 조회
 main.cpp                     # C++ 예제 프로그램 (8개 예제)
 CMakeLists.txt               # CMake 빌드 설정
 README.md                    # 이 문서
 original_zeus_python_project/  # 참고용 Python Zeus 원본
```

### 설계 원칙

- **Header-only**: 별도 컴파일 불필요, `#include "power_monitor.h"`만으로 사용
- **Facade 패턴**: `PowerMonitor`가 모든 백엔드를 통합 관리
- **Enum 기반 디바이스 선택**: `DeviceType` enum으로 어떤 장비를 측정할지 지정
- **#ifdef 캡슐화**: 사용자 코드에는 `#ifdef` 없음, 모든 플랫폼 가드는 device 파일 내부에 격리
- **에러 처리**: 측정 불가 시 `std::runtime_error` throw  사용자가 catch하여 skip 결정

---

## API Reference

### C++ — `zeus::PowerMonitor`

```cpp
#include "power_monitor.h"

// === 생성자 (Enum 기반) ===

// NVIDIA GPU + Intel CPU 동시 모니터링
try {
    zeus::PowerMonitor monitor({zeus::DeviceType::NvidiaGPU,
                                zeus::DeviceType::IntelCPU});
} catch (const std::runtime_error& e) {
    std::cout << "디바이스 불가: " << e.what() << std::endl;
}

// Jetson SoC만
zeus::PowerMonitor monitor({zeus::DeviceType::JetsonSoC});

// Apple Silicon SoC만
zeus::PowerMonitor monitor({zeus::DeviceType::AppleSoC});

// Config로 세부 설정
zeus::PowerMonitor::Config cfg;
cfg.gpu_indices = {0, 1};          // 특정 GPU만
cfg.polling_interval_s = 0.05;     // 50ms 폴링 (Pre-Volta / Jetson)
zeus::PowerMonitor monitor({zeus::DeviceType::NvidiaGPU}, cfg);
```

#### 측정 윈도우 API

```cpp
monitor.begin_window("my_window");
// ... 작업 수행 (GPU + CPU + DRAM + SoC 동시 측정) ...
zeus::Measurement result = monitor.end_window("my_window");

// 결과 확인
double total   = result.total_energy();       // 전체 에너지 (J) = GPU+CPU+DRAM+SoC
double gpu_j   = result.total_gpu_energy();   // GPU 합계 (J)
double cpu_j   = result.total_cpu_energy();   // CPU 합계 (J)
double dram_j  = result.total_dram_energy();  // DRAM 합계 (J)
double soc_j   = result.total_soc_energy();   // SoC 합계 (J)
double gpu0_j  = result.gpu_energy[0];        // GPU 0 에너지 (J)
double time_s  = result.elapsed_time;         // 측정 시간 (초)

// SoC 에너지 상세 (Jetson / Apple)
for (const auto& [key, joules] : result.soc_energy) {
    // key: "jetson_cpu", "jetson_gpu", "apple_cpu_total", "apple_gpu", ...
    std::cout << key << ": " << joules << " J\n";
}
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
bool         has_soc = monitor.has_soc();       // SoC 백엔드 활성 여부
std::string  type    = monitor.gpu_type();      // "NVIDIA", "AMD", "None"
auto         gpus    = monitor.gpu_indices();   // 모니터링 중인 GPU 목록
auto         cpus    = monitor.cpu_indices();   // 모니터링 중인 CPU 소켓 목록
auto         soc_m   = monitor.soc_metrics();   // SoC 메트릭 키 집합
```

#### 정적 유틸리티

```cpp
int         count = zeus::PowerMonitor::get_device_count();       // NVIDIA + AMD
std::string name  = zeus::PowerMonitor::get_device_name(0);
std::string arch  = zeus::PowerMonitor::get_architecture_name(0);

// 개별 백엔드 가용성 확인 (PowerMonitor 생성 전)
bool nv   = zeus::NvidiaGpuBackend::is_available();
bool amd  = zeus::AmdGpuBackend::is_available();
bool rapl = zeus::RaplBackend::is_available();
bool jts  = zeus::JetsonSoCBackend::is_available();
bool apl  = zeus::AppleSoCBackend::is_available();
```

### C++ — `zeus::Measurement`

```cpp
struct Measurement {
    std::map<int, double>         gpu_energy;    // GPU index  Joules
    std::map<int, double>         cpu_energy;    // CPU socket index  Joules
    std::map<int, double>         dram_energy;   // CPU socket index  DRAM Joules
    std::map<std::string, double> soc_energy;    // SoC metric  Joules
    double                        elapsed_time;  // 측정 시간 (초)

    double total_energy() const;       // 전체 합계 (GPU+CPU+DRAM+SoC)
    double total_gpu_energy() const;
    double total_cpu_energy() const;
    double total_dram_energy() const;
    double total_soc_energy() const;
};
```

### C++ — `zeus::DeviceType`

```cpp
enum class DeviceType {
    NvidiaGPU,  // NVIDIA GPU via NVML
    AmdGPU,     // AMD GPU via ROCm SMI
    IntelCPU,   // Intel CPU/DRAM via RAPL
    JetsonSoC,  // NVIDIA Jetson SoC via INA3221
    AppleSoC,   // Apple Silicon via IOReport
};
```

---

## 빌드 방법

### 사전 요구사항

- **CMake**  3.18
- **C++17** 지원 컴파일러 (MSVC 2019+, GCC 7+, Clang 5+)
- **NVIDIA CUDA Toolkit** (NVML 사용 시, 기본 활성화)
- **ROCm**  6.1 (AMD GPU 사용 시, 수동 활성화)
- **IOKit.framework** (Apple Silicon, macOS에서 자동 링크)

### CMake 옵션

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `ZEUS_USE_NVML` | `ON` | NVIDIA NVML 지원 (CUDA Toolkit 필요) |
| `ZEUS_USE_ROCM_SMI` | `OFF` | AMD ROCm SMI 지원 (ROCm 필요) |

CPU (RAPL), Jetson (INA3221), Apple Silicon (IOReport)은 자동 감지됩니다.

### Windows (Visual Studio) — NVIDIA

```powershell
mkdir build; cd build
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

### macOS (Apple Silicon)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DZEUS_USE_NVML=OFF
make -j$(nproc)
./power_monitor_example
```

### 다른 프로젝트에서 사용 (CMake)

```cmake
add_subdirectory(path/to/AI-BMT-Power-Library)
target_link_libraries(my_app PRIVATE zeus_power_monitor)
```

---

## 예제 출력 (참고)

### C++ (NVIDIA GPU + Linux RAPL)

```
============================================
 Zeus C++ Power Monitor - Multi-Device
============================================
 Based on: github.com/ml-energy/zeus
 Devices:  NVIDIA GPU, AMD GPU, CPU (RAPL),
           Jetson SoC, Apple Silicon

========================================
 Example 1: Device Detection
========================================
  GPUs: 1 detected
    [GPU 0] NVIDIA GeForce RTX 4090 (Ada Lovelace)
  CPUs (RAPL): 1 socket(s) [0]
  Jetson SoC: not detected
  Apple SoC:  not detected

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
  Duration: 3.0012 s
  GPU energy: 135.6920 J
    GPU 0: 135.6920 J
  CPU energy: 42.3150 J
    CPU 0: 42.3150 J
  DRAM energy: 8.1230 J
    DRAM 0: 8.1230 J
  Total energy: 186.1300 J
```

---

## Python vs C++ 비교

| 항목 | Python (Zeus) | C++ (이 라이브러리) |
|---|---|---|
| 에너지 단위 | GPU/CPU: Joules, SoC: mJ | **모두 Joules (통일)** |
| `total_energy` | GPU 에너지만 합산 | **GPU+CPU+DRAM+SoC 전체 합산** |
| RAPL 랩어라운드 | 별도 프로세스로 카운터 감시 | `max_energy_range_uj` 기반 단일 랩어라운드 보정 |
| Pre-Volta 폴링 | 별도 프로세스 (multiprocessing) | 백그라운드 스레드 (std::thread) |
| Apple SoC | `zeus-apple-silicon` CPython 확장 | `dlsym` 직접 호출 (IOReport) |
| Jetson SoC | `multiprocessing` + asyncio | `std::thread` 폴링 |
| GPU 설정 변경 | 지원 (clock, power limit 등) | 미지원 (읽기 전용 모니터링) |

---

## 라이센스

이 구현은 [Zeus 프로젝트](https://github.com/ml-energy/zeus) (Apache 2.0 License)의 전력 측정 아키텍처를 참고하여 작성되었습니다.
