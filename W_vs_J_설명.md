# W(와트) vs J(줄) — 전력과 에너지의 차이

> 이 문서는 본 프로젝트(AI-BMT-Power-Library)에서 실제로 사용하는 API 기반으로  
> W와 J가 어떻게 다르고, 어떻게 측정되는지를 설명합니다.

---

## 1. 개념 정의

| 항목 | W (와트, Watt)                        | J (줄, Joule)                            |
| ---- | ------------------------------------- | ---------------------------------------- |
| 의미 | **순간 전력** (Power)                 | **소비 에너지** (Energy)                 |
| 뜻   | 지금 이 순간 얼마나 전력을 소모하는가 | 일정 기간 동안 총 얼마나 에너지를 썼는가 |
| 비유 | 수도꼭지 수압 (속도)                  | 흘러간 물의 총량                         |
| 관계 | $P$                                   | $E = P \times t$                         |

$$E \text{ (J)} = P \text{ (W)} \times t \text{ (s)}$$

예) GPU가 **10 W**로 **5초** 동작 → 소비 에너지 = **50 J**

---

## 2. NVIDIA GPU — NVML API 기반 설명

### 2-1. W (순간 전력) 측정

```cpp
// device/gpu_nvidia.h
double get_instant_power_w(int gpu_index) const {
    unsigned int power_mw = 0;
    nvmlDeviceGetPowerUsage(handle, &power_mw);   // ← NVML API
    return static_cast<double>(power_mw) / 1000.0; // mW → W
}
```

| API                         | 반환값        | 설명                        |
| --------------------------- | ------------- | --------------------------- |
| `nvmlDeviceGetPowerUsage()` | mW (밀리와트) | 현재 순간의 GPU 전력 소모량 |

> 벤치마크 출력 예시:
>
> ```
> [0.00s] GPU 0 power: 5.51 W   ← 그 순간 GPU 0이 5.51W를 소모하고 있음
> [0.50s] GPU 0 power: 5.51 W
> [1.00s] GPU 0 power: 5.33 W
> ```

---

### 2-2. J (에너지) 측정 — 하드웨어 카운터 방식 (Volta+ 전용)

NVIDIA Volta 이후 GPU는 **하드웨어 누적 에너지 카운터**를 내장합니다.

```cpp
// device/gpu_nvidia.h — private 내부 메서드 (외부 API로 노출하지 않음)
double get_total_energy_mj(int gpu_index) const {
    unsigned long long energy_mj = 0;
    nvmlDeviceGetTotalEnergyConsumption(handle, &energy_mj); // ← NVML API
    return static_cast<double>(energy_mj); // 단위: mJ (드라이버 로드 시점부터 누적)
}
```

> ℹ️ `get_total_energy_mj()`는 **private** 메서드입니다.  
> raw 누적값 자체를 외부에 노출하면 혼란을 주므로, `snapshot_energy_j()` / `compute_energy_delta_j()` 내부에서만 호출됩니다.

에너지 **구간 측정**은 스냅샷 차분(delta)으로 계산합니다:

```
측정 시작 시점 → 카운터 값 저장 (start_snap)
         작업 실행
측정 종료 시점 → 카운터 값 저장 (end_snap)

소비 에너지(J) = (end_snap - start_snap) / 1000   [mJ → J]
```

```cpp
// device/gpu_nvidia.h — compute_energy_delta_j()
double end_e   = get_total_energy_mj(idx) / 1000.0;  // 현재 카운터 → J
double start_e = start_snap[idx];                      // 시작 카운터 → J
result[idx] = end_e - start_e;                         // delta = 소비 에너지
```

| API                                     | 반환값                        | 설명                   |
| --------------------------------------- | ----------------------------- | ---------------------- |
| `nvmlDeviceGetTotalEnergyConsumption()` | mJ (누적, 드라이버 로드 이후) | 하드웨어 에너지 카운터 |

> 벤치마크 출력 예시:
>
> ```
> [Energy Window, 3초 측정]
> GPU 0: 19.6230 J   ← 이 3초 동안 GPU 0이 소비한 에너지
> GPU 1: 45.5200 J   ← 이 3초 동안 GPU 1이 소비한 에너지
> ```

---

### 2-3. J 측정 — 폴링 적분 방식 (pre-Volta 폴백)

Volta 이전 GPU는 하드웨어 카운터가 없으므로,  
백그라운드에서 W를 주기적으로 샘플링한 뒤 **사다리꼴 적분**으로 J를 계산합니다.

$$E = \sum_{i} \frac{P_i + P_{i+1}}{2} \times \Delta t_i$$

```cpp
// polling_interval_(기본 0.1s)마다 nvmlDeviceGetPowerUsage() 호출 → 적분
if (!supports_energy_counter(idx)) {
    double energy = compute_energy_from_samples(idx, start_time, end_time);
    result[idx] = energy; // 적분값 → J
}
```

---

## 3. Apple Silicon SoC — IOReport API 기반 설명

### 3-1. J (에너지) 측정 — IOReport 기본값

Apple IOReport는 **J(에너지)를 기본**으로 제공합니다.  
W는 IOReport에서 직접 제공하지 않고, J로부터 파생 계산합니다.

```cpp
// device/soc_apple.h — snapshot_energy_j()
CFDictionaryRef current = IOReportCreateSamples(...);       // 현재 샘플
CFDictionaryRef delta   = IOReportCreateSamplesDelta(       // baseline과의 차분
    baseline_sample_, current, nullptr);
// 각 채널의 raw값을 nJ/mJ 단위 변환 → J
```

내부 흐름:

```
[생성자]  IOReportCreateSamples() → baseline_sample_ 저장

[측정 시] IOReportCreateSamples()         → current 샘플
          IOReportCreateSamplesDelta()     → (current - baseline) = 누적 에너지 delta
          단위 라벨(nJ, mJ 등) 확인 후 → J 변환
```

| API                                           | 역할                          |
| --------------------------------------------- | ----------------------------- |
| `IOReportCopyChannelsInGroup("Energy Model")` | "Energy Model" 채널 목록 획득 |
| `IOReportCreateSubscription()`                | 채널 구독 생성                |
| `IOReportCreateSamples()`                     | 현재 에너지 샘플 수집         |
| `IOReportCreateSamplesDelta()`                | 두 샘플 간 delta 계산         |
| `IOReportChannelGetUnitLabel()`               | 단위(nJ/mJ) 확인 → J 변환     |

> 벤치마크 출력 예시:
>
> ```
> [Energy Window, 3초 측정]
> apple_ane:       0.4980 J   ← ANE(Apple Neural Engine)가 3초간 소비한 에너지
> apple_cpu_total: 20.5110 J  ← CPU 전체가 3초간 소비한 에너지
> apple_dram:      1.6110 J   ← DRAM이 3초간 소비한 에너지
> apple_gpu:       0.0037 J   ← GPU가 3초간 소비한 에너지
> ```

---

### 3-2. W (순간 전력) 측정 — 두 샘플 차분

IOReport는 순간 전력을 직접 제공하지 않으므로,  
**50ms 간격으로 두 번 샘플링** 후 나누어 W를 계산합니다.

```cpp
// device/soc_apple.h — get_instant_power_w()
auto snap1 = snapshot_energy_j();   // 첫 번째 에너지 스냅샷
sleep(50ms);
auto snap2 = snapshot_energy_j();   // 두 번째 에너지 스냅샷

double dt = (t2 - t1).count();      // 경과 시간 (초)
return (snap2[metric] - snap1[metric]) / dt;  // ΔE / Δt = P (W)
```

$$P \text{ (W)} = \frac{\Delta E \text{ (J)}}{\Delta t \text{ (s)}}$$

> 벤치마크 출력 예시:
>
> ```
> (IOReport: ~50ms sampling interval per query)
> apple_ane:       3.21 W   ← 50ms 구간 평균 전력
> apple_cpu_total: 1.19 W
> apple_dram:      2.08 W
> apple_gpu:       0.06 W
> ```

---

## 4. W vs J — 실전 사용 시나리오 비교

| 목적                                      | 적합한 단위 | 이유                          |
| ----------------------------------------- | ----------- | ----------------------------- |
| 현재 GPU가 얼마나 뜨거운지 확인           | **W**       | 순간 전력 → 발열과 직결       |
| AI 모델 추론 1회에 얼마나 에너지를 쓰는지 | **J**       | 작업 전체 누적값              |
| 전력 한도 초과 여부 모니터링              | **W**       | 순간값이 limit 초과 여부 판단 |
| 모델 A vs 모델 B 에너지 효율 비교         | **J**       | 같은 작업에 총 에너지 비교    |
| AI 추론 효율 지표                         | **J/token** | 토큰 1개 생성당 에너지        |

---

## 5. 요약

```
W (와트)  = 지금 이 순간의 소비 속도
J (줄)    = 일정 구간 동안 소비한 총량

J = W × 시간(s)
W = J / 시간(s)  ← Apple IOReport에서 W를 이렇게 파생 계산함
```

| 플랫폼             | W 측정 방법                           | J 측정 방법                                   |
| ------------------ | ------------------------------------- | --------------------------------------------- |
| NVIDIA (Volta+)    | `nvmlDeviceGetPowerUsage()` 직접 호출 | `nvmlDeviceGetTotalEnergyConsumption()` delta |
| NVIDIA (pre-Volta) | `nvmlDeviceGetPowerUsage()` 직접 호출 | W 폴링 후 사다리꼴 적분                       |
| Apple Silicon      | IOReport 두 샘플 차분 ÷ 시간          | `IOReportCreateSamplesDelta()` nJ→J 변환      |
