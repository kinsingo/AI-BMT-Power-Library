"""
Zeus Power Monitor - Multi-Device Python Implementation

Python implementation matching the Zeus project's full device support:
  - NVIDIA GPU  (via pynvml / NVML)
  - AMD GPU     (via amdsmi / ROCm SMI)
  - Intel CPU + DRAM (via RAPL sysfs on Linux)
  - Apple Silicon SoC (via zeus_apple_silicon extension)
  - NVIDIA Jetson SoC (via INA3221 sysfs on Linux)

This mirrors the C++ power_monitor.h for easy comparison and testing,
and matches the architecture of zeus.monitor.energy.ZeusMonitor.

Reference Python source files from the Zeus project:
  - zeus/monitor/energy.py    : ZeusMonitor (begin_window / end_window)
  - zeus/device/gpu/nvidia.py : NVML API wrappers
  - zeus/device/gpu/amd.py    : AMD SMI API wrappers
  - zeus/device/cpu/rapl.py   : Intel RAPL sysfs wrappers
  - zeus/device/soc/apple.py  : Apple Silicon energy monitor
  - zeus/device/soc/jetson.py : Jetson INA3221 power rails
  - zeus/monitor/power.py     : Background power polling for pre-Volta GPUs

Usage:
    pip install -r requirements.txt
    python main.py
"""

from __future__ import annotations

import glob
import os
import platform
import re
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

# ============================================================================
# Backend Availability Detection
# ============================================================================

_HAS_PYNVML = False
_HAS_AMDSMI = False
_HAS_RAPL = False
_HAS_APPLE_SILICON = False
_HAS_JETSON = False

_RAPL_BASE = ""  # Set below if RAPL is available

# --- NVIDIA GPU (pynvml) ---
try:
    import pynvml
    pynvml.nvmlInit()
    _HAS_PYNVML = True
except Exception:
    pynvml = None  # type: ignore

# --- AMD GPU (amdsmi) ---
try:
    import amdsmi  # type: ignore
    amdsmi.amdsmi_init()
    _HAS_AMDSMI = True
except Exception:
    amdsmi = None  # type: ignore

# --- Intel RAPL CPU (Linux sysfs) ---
_RAPL_DIR = "/sys/class/powercap/intel-rapl"
_RAPL_CONTAINER_DIR = "/zeus_sys/class/powercap/intel-rapl"  # Zeus container mount
if sys.platform == "linux":
    if os.path.isdir(_RAPL_DIR):
        _HAS_RAPL = True
        _RAPL_BASE = _RAPL_DIR
    elif os.path.isdir(_RAPL_CONTAINER_DIR):
        _HAS_RAPL = True
        _RAPL_BASE = _RAPL_CONTAINER_DIR

# --- Apple Silicon SoC ---
if sys.platform == "darwin" and platform.processor() == "arm":
    try:
        import zeus_apple_silicon  # type: ignore
        _HAS_APPLE_SILICON = True
    except ImportError:
        zeus_apple_silicon = None  # type: ignore

# --- NVIDIA Jetson SoC ---
_JETSON_INA_BASE = "/sys/bus/i2c/drivers/ina3221x"
if (
    sys.platform == "linux"
    and platform.machine() == "aarch64"
    and os.path.isdir(_JETSON_INA_BASE)
):
    _HAS_JETSON = True


# ============================================================================
# Measurement Result
# ============================================================================

@dataclass
class Measurement:
    """Result of a power measurement window.

    Corresponds to zeus.monitor.energy.Measurement and zeus::Measurement (C++).
    Extended to include CPU, DRAM, and SoC measurements.

    Attributes:
        gpu_energy:    GPU index -> energy consumed in Joules.
        cpu_energy:    CPU socket index -> energy consumed in Joules (RAPL).
        dram_energy:   CPU socket index -> DRAM energy in Joules (RAPL).
        soc_energy:    Metric name -> energy in Joules (Apple Silicon / Jetson).
        elapsed_time:  Wall-clock time of the measurement window in seconds.
    """

    gpu_energy: Dict[int, float] = field(default_factory=dict)
    cpu_energy: Optional[Dict[int, float]] = None
    dram_energy: Optional[Dict[int, float]] = None
    soc_energy: Optional[Dict[str, float]] = None
    elapsed_time: float = 0.0

    @property
    def total_energy(self) -> float:
        """Total energy across ALL device types (Joules)."""
        total = self.total_gpu_energy
        if self.cpu_energy:
            total += sum(self.cpu_energy.values())
        if self.dram_energy:
            total += sum(self.dram_energy.values())
        if self.soc_energy:
            total += sum(self.soc_energy.values())
        return total

    @property
    def total_gpu_energy(self) -> float:
        """Total GPU energy (Joules)."""
        return sum(self.gpu_energy.values())

    @property
    def total_cpu_energy(self) -> float:
        """Total CPU energy (Joules)."""
        return sum(self.cpu_energy.values()) if self.cpu_energy else 0.0

    @property
    def total_dram_energy(self) -> float:
        """Total DRAM energy (Joules)."""
        return sum(self.dram_energy.values()) if self.dram_energy else 0.0

    @property
    def total_soc_energy(self) -> float:
        """Total SoC energy (Joules)."""
        return sum(self.soc_energy.values()) if self.soc_energy else 0.0


# ============================================================================
# Architecture Constants
# ============================================================================

_NVIDIA_ARCH_NAMES = {
    2: "Kepler",
    3: "Maxwell",
    4: "Pascal",
    5: "Volta",
    6: "Turing",
    7: "Ampere",
    8: "Ada Lovelace",
    9: "Hopper",
    10: "Blackwell",
}

_NVIDIA_ARCH_VOLTA = 5


# ============================================================================
# Backend: NVIDIA GPU
# ============================================================================

class _NvidiaGpuBackend:
    """NVIDIA GPU backend using pynvml (NVML).

    Reference: zeus/device/gpu/nvidia.py
    """

    def __init__(self, gpu_indices: List[int]):
        assert _HAS_PYNVML, "pynvml not available"
        device_count = pynvml.nvmlDeviceGetCount()

        self.gpu_indices = gpu_indices
        self.handles: Dict[int, Any] = {}
        self.supports_energy: Dict[int, bool] = {}
        self._need_polling = False

        for idx in gpu_indices:
            if idx < 0 or idx >= device_count:
                raise ValueError(
                    f"NVIDIA GPU index {idx} out of range [0, {device_count})"
                )
            handle = pynvml.nvmlDeviceGetHandleByIndex(idx)
            self.handles[idx] = handle
            try:
                arch = pynvml.nvmlDeviceGetArchitecture(handle)
                supports = arch >= _NVIDIA_ARCH_VOLTA
            except pynvml.NVMLError:
                supports = False
            self.supports_energy[idx] = supports
            if not supports:
                self._need_polling = True

    def get_energy_snapshot(self) -> Dict[int, float]:
        """Read cumulative energy counters (Joules) for Volta+ GPUs."""
        snapshot = {}
        for idx in self.gpu_indices:
            if self.supports_energy[idx]:
                energy_mj = pynvml.nvmlDeviceGetTotalEnergyConsumption(
                    self.handles[idx]
                )
                snapshot[idx] = energy_mj / 1000.0  # mJ -> J
        return snapshot

    def get_instant_power(self, gpu_index: int) -> float:
        """Instantaneous power (Watts)."""
        return pynvml.nvmlDeviceGetPowerUsage(self.handles[gpu_index]) / 1000.0

    def get_total_energy(self, gpu_index: int) -> float:
        """Cumulative energy since driver load (Joules)."""
        return (
            pynvml.nvmlDeviceGetTotalEnergyConsumption(self.handles[gpu_index])
            / 1000.0
        )

    def get_power_limit(self, gpu_index: int) -> float:
        """Power limit (Watts)."""
        return (
            pynvml.nvmlDeviceGetPowerManagementLimit(self.handles[gpu_index])
            / 1000.0
        )

    @staticmethod
    def get_device_count() -> int:
        pynvml.nvmlInit()
        return pynvml.nvmlDeviceGetCount()

    @staticmethod
    def get_device_name(gpu_index: int) -> str:
        pynvml.nvmlInit()
        handle = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
        name = pynvml.nvmlDeviceGetName(handle)
        return name.decode("utf-8") if isinstance(name, bytes) else name

    @staticmethod
    def get_architecture_name(gpu_index: int) -> str:
        pynvml.nvmlInit()
        handle = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
        arch = pynvml.nvmlDeviceGetArchitecture(handle)
        return _NVIDIA_ARCH_NAMES.get(arch, f"Unknown (arch={arch})")


# ============================================================================
# Backend: AMD GPU
# ============================================================================

class _AmdGpuBackend:
    """AMD GPU backend using amdsmi.

    Reference: zeus/device/gpu/amd.py
    Requires ROCm >= 6.1.
    """

    def __init__(self, gpu_indices: List[int]):
        assert _HAS_AMDSMI, "amdsmi not available"

        self._processor_handles = amdsmi.amdsmi_get_processor_handles()
        device_count = len(self._processor_handles)

        self.gpu_indices = gpu_indices
        self.handles: Dict[int, Any] = {}
        self.supports_energy: Dict[int, bool] = {}

        for idx in gpu_indices:
            if idx < 0 or idx >= device_count:
                raise ValueError(
                    f"AMD GPU index {idx} out of range [0, {device_count})"
                )
            handle = self._processor_handles[idx]
            self.handles[idx] = handle
            self.supports_energy[idx] = self._check_energy_counter(handle)

    @staticmethod
    def _check_energy_counter(handle: Any) -> bool:
        """Check if energy counter returns valid data."""
        try:
            amdsmi.amdsmi_get_energy_count(handle)
            return True
        except Exception:
            return False

    def get_energy_snapshot(self) -> Dict[int, float]:
        """Read cumulative energy counters (Joules)."""
        snapshot = {}
        for idx in self.gpu_indices:
            if self.supports_energy[idx]:
                try:
                    info = amdsmi.amdsmi_get_energy_count(self.handles[idx])
                    # Newer amdsmi: energy_accumulator * counter_resolution (uJ)
                    if "energy_accumulator" in info and "counter_resolution" in info:
                        energy_uj = (
                            info["energy_accumulator"] * info["counter_resolution"]
                        )
                    elif "power" in info and "counter_resolution" in info:
                        energy_uj = info["power"] * info["counter_resolution"]
                    else:
                        continue
                    snapshot[idx] = energy_uj / 1_000_000.0  # uJ -> J
                except Exception:
                    pass
        return snapshot

    def get_instant_power(self, gpu_index: int) -> float:
        """Instantaneous power (Watts) — uses socket power info."""
        info = amdsmi.amdsmi_get_power_info(self.handles[gpu_index])
        for key in ("current_socket_power", "average_socket_power"):
            val = info.get(key)
            if val is not None and val != "N/A" and val > 0:
                return float(val)  # Already in Watts from amdsmi
        raise RuntimeError(f"Cannot read power for AMD GPU {gpu_index}")

    @staticmethod
    def get_device_count() -> int:
        amdsmi.amdsmi_init()
        return len(amdsmi.amdsmi_get_processor_handles())

    @staticmethod
    def get_device_name(gpu_index: int) -> str:
        handles = amdsmi.amdsmi_get_processor_handles()
        info = amdsmi.amdsmi_get_gpu_asic_info(handles[gpu_index])
        return info.get("market_name", f"AMD GPU {gpu_index}")


# ============================================================================
# Backend: Intel RAPL CPU
# ============================================================================

class _RaplCpuBackend:
    """Intel RAPL CPU/DRAM backend via Linux sysfs.

    Reference: zeus/device/cpu/rapl.py

    Reads cumulative energy from:
      /sys/class/powercap/intel-rapl/intel-rapl:{cpu}/energy_uj
      /sys/class/powercap/intel-rapl/intel-rapl:{cpu}/intel-rapl:{cpu}:{sub}/energy_uj (DRAM)
    """

    def __init__(self, cpu_indices: Optional[List[int]] = None):
        assert _HAS_RAPL, "RAPL not available"

        self._cpu_dirs: Dict[int, str] = {}
        self._dram_dirs: Dict[int, str] = {}
        self.cpu_indices: List[int] = []
        self.supports_dram: Dict[int, bool] = {}

        # Scan for intel-rapl:{N} directories
        rapl_pattern = os.path.join(_RAPL_BASE, "intel-rapl:*")
        for rapl_dir in sorted(glob.glob(rapl_pattern)):
            dirname = os.path.basename(rapl_dir)
            match = re.match(r"intel-rapl:(\d+)$", dirname)
            if not match:
                continue
            cpu_idx = int(match.group(1))
            energy_file = os.path.join(rapl_dir, "energy_uj")
            if not os.path.isfile(energy_file):
                continue

            self._cpu_dirs[cpu_idx] = rapl_dir

            # Check for DRAM sub-package
            dram_found = False
            sub_pattern = os.path.join(rapl_dir, f"intel-rapl:{cpu_idx}:*")
            for sub_dir in glob.glob(sub_pattern):
                name_file = os.path.join(sub_dir, "name")
                if os.path.isfile(name_file):
                    try:
                        with open(name_file) as f:
                            name = f.read().strip()
                        if name == "dram":
                            self._dram_dirs[cpu_idx] = sub_dir
                            dram_found = True
                            break
                    except OSError:
                        pass
            self.supports_dram[cpu_idx] = dram_found

        # Filter to requested indices
        available = sorted(self._cpu_dirs.keys())
        if cpu_indices is None:
            self.cpu_indices = available
        else:
            for idx in cpu_indices:
                if idx not in self._cpu_dirs:
                    raise ValueError(
                        f"CPU socket {idx} not found in RAPL. Available: {available}"
                    )
            self.cpu_indices = list(cpu_indices)

    def get_cpu_energy(self, cpu_index: int) -> float:
        """Read cumulative CPU package energy (Joules)."""
        energy_file = os.path.join(self._cpu_dirs[cpu_index], "energy_uj")
        with open(energy_file) as f:
            energy_uj = int(f.read().strip())
        return energy_uj / 1_000_000.0  # uJ -> J

    def get_dram_energy(self, cpu_index: int) -> Optional[float]:
        """Read cumulative DRAM energy (Joules), or None if not supported."""
        if cpu_index not in self._dram_dirs:
            return None
        energy_file = os.path.join(self._dram_dirs[cpu_index], "energy_uj")
        try:
            with open(energy_file) as f:
                energy_uj = int(f.read().strip())
            return energy_uj / 1_000_000.0
        except OSError:
            return None

    def get_energy_snapshot(self) -> Tuple[Dict[int, float], Dict[int, float]]:
        """Read all CPU and DRAM energy counters.

        Returns:
            (cpu_energy, dram_energy) dictionaries in Joules.
        """
        cpu_snap: Dict[int, float] = {}
        dram_snap: Dict[int, float] = {}
        for idx in self.cpu_indices:
            cpu_snap[idx] = self.get_cpu_energy(idx)
            dram_e = self.get_dram_energy(idx)
            if dram_e is not None:
                dram_snap[idx] = dram_e
        return cpu_snap, dram_snap


# ============================================================================
# Backend: Apple Silicon SoC
# ============================================================================

class _AppleSiliconBackend:
    """Apple Silicon SoC backend via zeus_apple_silicon extension.

    Reference: zeus/device/soc/apple.py

    Measures CPU, GPU, DRAM, ANE (Apple Neural Engine) energy on macOS.
    Requires the ``zeus_apple_silicon`` package (pip install zeus-apple-silicon).
    """

    def __init__(self):
        assert _HAS_APPLE_SILICON, "zeus_apple_silicon not available"
        self._monitor = zeus_apple_silicon.AppleEnergyMonitor()
        self._windows: Dict[str, Dict[str, float]] = {}

    def get_available_metrics(self) -> Set[str]:
        """Get names of available energy metrics."""
        metrics = self._monitor.get_cumulative_energy()
        return {k for k, v in metrics.items() if v is not None}

    def begin_window(self, key: str) -> None:
        """Snapshot cumulative energy for a measurement window."""
        snapshot = self._monitor.get_cumulative_energy()
        self._windows[key] = {
            k: v / 1000.0 for k, v in snapshot.items() if v is not None
        }  # mJ -> J

    def end_window(self, key: str) -> Dict[str, float]:
        """End window, return per-metric energy delta (Joules)."""
        start = self._windows.pop(key)
        end_raw = self._monitor.get_cumulative_energy()
        end = {k: v / 1000.0 for k, v in end_raw.items() if v is not None}
        return {k: end.get(k, 0.0) - start.get(k, 0.0) for k in start}


# ============================================================================
# Backend: NVIDIA Jetson SoC
# ============================================================================

class _JetsonBackend:
    """NVIDIA Jetson SoC backend via INA3221 sysfs power rails.

    Reference: zeus/device/soc/jetson.py

    Reads power from /sys/bus/i2c/drivers/ina3221x/*/iio:device*/
    and integrates over time using background polling.
    """

    def __init__(self, polling_interval: float = 0.1):
        assert _HAS_JETSON, "Jetson INA3221 not available"

        self._power_readers: Dict[str, Callable[[], float]] = {}
        self._polling_interval = polling_interval
        self._cumulative_energy: Dict[str, float] = {}  # metric -> mJ
        self._last_time: Optional[float] = None
        self._lock = threading.Lock()
        self._polling_active = False
        self._polling_thread: Optional[threading.Thread] = None
        self._windows: Dict[str, Dict[str, float]] = {}

        self._discover_rails()

        if self._power_readers:
            self._start_polling()

    def _discover_rails(self):
        """Scan INA3221 sysfs for power measurement rails.

        Classifies each rail as cpu, gpu, or total based on the rail label.
        """
        for ina_dir in glob.glob(os.path.join(_JETSON_INA_BASE, "*")):
            for iio_dir in glob.glob(os.path.join(ina_dir, "iio:device*")):
                for rail_idx in range(3):  # INA3221 has 3 channels
                    label_file = os.path.join(iio_dir, f"rail_name_{rail_idx}")
                    if not os.path.isfile(label_file):
                        continue
                    try:
                        with open(label_file) as f:
                            label = f.read().strip().lower()
                    except OSError:
                        continue

                    # Classify rail by label keywords
                    metric_name = None
                    if "gpu" in label:
                        metric_name = "gpu_energy"
                    elif "cpu" in label:
                        metric_name = "cpu_energy"
                    elif any(k in label for k in ("total", "system", "_in")):
                        metric_name = "total_energy"
                    if not metric_name:
                        continue

                    # Find power file
                    power_file = os.path.join(iio_dir, f"in_power{rail_idx}_input")
                    if os.path.isfile(power_file):
                        # Capture power_file in closure
                        self._power_readers[metric_name] = (
                            lambda pf=power_file: self._read_power_file(pf)
                        )
                        self._cumulative_energy[metric_name] = 0.0

    @staticmethod
    def _read_power_file(path: str) -> float:
        """Read power in milliwatts from sysfs file."""
        try:
            with open(path) as f:
                return float(f.read().strip())
        except (OSError, ValueError):
            return 0.0

    def _start_polling(self):
        self._polling_active = True
        self._last_time = time.monotonic()
        self._polling_thread = threading.Thread(
            target=self._polling_loop, daemon=True
        )
        self._polling_thread.start()

    def _stop_polling(self):
        self._polling_active = False
        if self._polling_thread and self._polling_thread.is_alive():
            self._polling_thread.join(timeout=2.0)

    def _polling_loop(self):
        """Background thread: read power rails and accumulate energy."""
        while self._polling_active:
            now = time.monotonic()
            with self._lock:
                dt = now - self._last_time if self._last_time else 0.0
                self._last_time = now
                for metric_name, reader in self._power_readers.items():
                    power_mw = reader()
                    if power_mw > 0 and dt > 0:
                        # mW * s = mJ
                        self._cumulative_energy[metric_name] += power_mw * dt
            time.sleep(self._polling_interval)

    def get_energy_snapshot(self) -> Dict[str, float]:
        """Get cumulative energy for all rails (Joules)."""
        with self._lock:
            return {
                k: v / 1000.0 for k, v in self._cumulative_energy.items()
            }  # mJ -> J

    def begin_window(self, key: str) -> None:
        self._windows[key] = self.get_energy_snapshot()

    def end_window(self, key: str) -> Dict[str, float]:
        start = self._windows.pop(key)
        end = self.get_energy_snapshot()
        return {k: end.get(k, 0.0) - start.get(k, 0.0) for k in start}

    def __del__(self):
        self._stop_polling()


# ============================================================================
# PowerMonitor - Main Library Class (Multi-Device)
# ============================================================================

class PowerMonitor:
    """Multi-device power/energy monitor.

    Auto-detects and uses all available backends:
      - NVIDIA GPU (pynvml)
      - AMD GPU (amdsmi)
      - Intel CPU/DRAM (RAPL sysfs)
      - Apple Silicon SoC (zeus_apple_silicon)
      - NVIDIA Jetson SoC (INA3221 sysfs)

    Corresponds to zeus.monitor.energy.ZeusMonitor in the original Zeus project.

    Example:
        >>> monitor = PowerMonitor(gpu_indices=[0])
        >>> monitor.begin_window("train")
        >>> # ... workload ...
        >>> result = monitor.end_window("train")
        >>> print(f"GPU: {result.total_gpu_energy:.2f} J")
        >>> print(f"CPU: {result.total_cpu_energy:.2f} J")
    """

    def __init__(
        self,
        gpu_indices: Optional[List[int]] = None,
        cpu_indices: Optional[List[int]] = None,
        monitor_gpu: bool = True,
        monitor_cpu: bool = True,
        monitor_soc: bool = True,
        polling_interval: float = 0.1,
    ):
        """Initialize the power monitor.

        Args:
            gpu_indices: GPU indices to monitor. None = all GPUs.
            cpu_indices: CPU socket indices. None = all (RAPL).
            monitor_gpu: Whether to enable GPU monitoring.
            monitor_cpu: Whether to enable CPU/DRAM monitoring.
            monitor_soc: Whether to enable SoC monitoring.
            polling_interval: Polling interval (seconds) for pre-Volta
                              NVIDIA GPUs and Jetson background threads.
        """
        self._nvidia: Optional[_NvidiaGpuBackend] = None
        self._amd: Optional[_AmdGpuBackend] = None
        self._rapl: Optional[_RaplCpuBackend] = None
        self._apple: Optional[_AppleSiliconBackend] = None
        self._jetson: Optional[_JetsonBackend] = None
        self._windows: Dict[str, dict] = {}
        self._polling_interval = polling_interval

        # Pre-Volta NVML polling state
        self._nvml_polling_active = False
        self._nvml_samples: list = []
        self._nvml_samples_lock = threading.Lock()
        self._nvml_polling_thread: Optional[threading.Thread] = None

        # --- GPU backends ---
        if monitor_gpu:
            # Try NVIDIA first
            if _HAS_PYNVML:
                try:
                    if gpu_indices is None:
                        nvidia_count = _NvidiaGpuBackend.get_device_count()
                        nvidia_indices = list(range(nvidia_count))
                    else:
                        nvidia_indices = list(gpu_indices)
                    if nvidia_indices:
                        self._nvidia = _NvidiaGpuBackend(nvidia_indices)
                        if self._nvidia._need_polling:
                            self._start_nvml_polling()
                except Exception:
                    self._nvidia = None

            # Try AMD (only if NVIDIA not found)
            if _HAS_AMDSMI and self._nvidia is None:
                try:
                    if gpu_indices is None:
                        amd_count = _AmdGpuBackend.get_device_count()
                        amd_indices = list(range(amd_count))
                    else:
                        amd_indices = list(gpu_indices)
                    if amd_indices:
                        self._amd = _AmdGpuBackend(amd_indices)
                except Exception:
                    self._amd = None

        # --- CPU backend ---
        if monitor_cpu and _HAS_RAPL:
            try:
                self._rapl = _RaplCpuBackend(cpu_indices)
            except Exception:
                self._rapl = None

        # --- SoC backends ---
        if monitor_soc:
            if _HAS_APPLE_SILICON:
                try:
                    self._apple = _AppleSiliconBackend()
                except Exception:
                    self._apple = None
            if _HAS_JETSON:
                try:
                    self._jetson = _JetsonBackend(polling_interval)
                except Exception:
                    self._jetson = None

    def __del__(self):
        self._stop_nvml_polling()

    # ---- Device info properties ----

    @property
    def gpu_type(self) -> Optional[str]:
        """Returns 'NVIDIA', 'AMD', or None."""
        if self._nvidia:
            return "NVIDIA"
        if self._amd:
            return "AMD"
        return None

    @property
    def has_gpu(self) -> bool:
        return self._nvidia is not None or self._amd is not None

    @property
    def has_cpu(self) -> bool:
        return self._rapl is not None

    @property
    def has_soc(self) -> bool:
        return self._apple is not None or self._jetson is not None

    @property
    def soc_type(self) -> Optional[str]:
        """Returns 'Apple Silicon', 'Jetson', or None."""
        if self._apple:
            return "Apple Silicon"
        if self._jetson:
            return "Jetson"
        return None

    @property
    def gpu_indices(self) -> List[int]:
        if self._nvidia:
            return self._nvidia.gpu_indices
        if self._amd:
            return self._amd.gpu_indices
        return []

    @property
    def cpu_indices(self) -> List[int]:
        return self._rapl.cpu_indices if self._rapl else []

    # ---- Main Measurement API ----

    def begin_window(self, key: str) -> None:
        """Mark the beginning of a measurement window.

        Corresponds to ZeusMonitor.begin_window() in the original Zeus project.
        Snapshots energy counters across all detected device types.

        Args:
            key: Unique name for this measurement window.
        """
        if key in self._windows:
            raise RuntimeError(
                f"Window '{key}' already active. "
                f"Call end_window('{key}') first."
            )

        state: dict = {"start_time": time.monotonic()}

        # GPU snapshot
        if self._nvidia:
            state["nvidia_energy"] = self._nvidia.get_energy_snapshot()
        if self._amd:
            state["amd_energy"] = self._amd.get_energy_snapshot()

        # CPU/DRAM snapshot
        if self._rapl:
            cpu_e, dram_e = self._rapl.get_energy_snapshot()
            state["cpu_energy"] = cpu_e
            state["dram_energy"] = dram_e

        # SoC snapshot
        if self._apple:
            self._apple.begin_window(key)
        if self._jetson:
            self._jetson.begin_window(key)

        self._windows[key] = state

    def end_window(self, key: str) -> Measurement:
        """Mark the end of a measurement window and return the result.

        Corresponds to ZeusMonitor.end_window() in the original Zeus project.
        Computes energy deltas across all detected device types.

        Args:
            key: Name of the window started with begin_window().

        Returns:
            Measurement result with per-device energy in Joules.
        """
        if key not in self._windows:
            raise RuntimeError(
                f"Window '{key}' not found. "
                f"Call begin_window('{key}') first."
            )

        end_time = time.monotonic()
        state = self._windows.pop(key)
        result = Measurement(elapsed_time=end_time - state["start_time"])

        # === GPU energy ===
        if self._nvidia:
            end_snap = self._nvidia.get_energy_snapshot()
            for idx in self._nvidia.gpu_indices:
                if self._nvidia.supports_energy[idx]:
                    result.gpu_energy[idx] = (
                        end_snap.get(idx, 0.0)
                        - state["nvidia_energy"].get(idx, 0.0)
                    )
                else:
                    # Pre-Volta: trapezoidal integration from polled samples
                    energy = self._compute_nvml_poll_energy(
                        idx, state["start_time"], end_time
                    )
                    if energy <= 0.0:
                        power_w = self._nvidia.get_instant_power(idx)
                        energy = power_w * result.elapsed_time
                    result.gpu_energy[idx] = energy

        if self._amd:
            end_snap = self._amd.get_energy_snapshot()
            for idx in self._amd.gpu_indices:
                if idx in end_snap and idx in state.get("amd_energy", {}):
                    result.gpu_energy[idx] = (
                        end_snap[idx] - state["amd_energy"][idx]
                    )
                else:
                    # Fallback: power × time
                    try:
                        power_w = self._amd.get_instant_power(idx)
                        result.gpu_energy[idx] = power_w * result.elapsed_time
                    except Exception:
                        result.gpu_energy[idx] = 0.0

        # === CPU / DRAM energy ===
        if self._rapl:
            end_cpu, end_dram = self._rapl.get_energy_snapshot()
            result.cpu_energy = {}
            for idx in self._rapl.cpu_indices:
                result.cpu_energy[idx] = (
                    end_cpu[idx] - state["cpu_energy"].get(idx, 0.0)
                )
            if end_dram:
                result.dram_energy = {}
                for idx, val in end_dram.items():
                    result.dram_energy[idx] = (
                        val - state["dram_energy"].get(idx, 0.0)
                    )

        # === SoC energy ===
        if self._apple:
            result.soc_energy = self._apple.end_window(key)
        if self._jetson:
            result.soc_energy = self._jetson.end_window(key)

        return result

    # ---- Power / Energy Query Functions ----

    def get_instant_power(self, gpu_index: int) -> float:
        """Get instantaneous GPU power draw (Watts).

        Uses nvmlDeviceGetPowerUsage (NVIDIA) or amdsmi_get_power_info (AMD).
        """
        if self._nvidia:
            return self._nvidia.get_instant_power(gpu_index)
        if self._amd:
            return self._amd.get_instant_power(gpu_index)
        raise RuntimeError("No GPU backend available")

    def get_total_energy(self, gpu_index: int) -> float:
        """Cumulative GPU energy since driver load (Joules). NVIDIA Volta+ only."""
        if self._nvidia:
            return self._nvidia.get_total_energy(gpu_index)
        raise RuntimeError(
            "Total energy counter only available on NVIDIA Volta+ GPUs"
        )

    def supports_energy_counter(self, gpu_index: int) -> bool:
        """Check if GPU supports hardware energy counter."""
        if self._nvidia:
            return self._nvidia.supports_energy.get(gpu_index, False)
        if self._amd:
            return self._amd.supports_energy.get(gpu_index, False)
        return False

    def get_power_limit(self, gpu_index: int) -> float:
        """GPU power management limit (Watts). NVIDIA only."""
        if self._nvidia:
            return self._nvidia.get_power_limit(gpu_index)
        raise RuntimeError("Power limit query only available on NVIDIA GPUs")

    # ---- Static Utility Functions ----

    @staticmethod
    def get_nvidia_device_count() -> int:
        """Get the number of NVIDIA GPUs."""
        return _NvidiaGpuBackend.get_device_count() if _HAS_PYNVML else 0

    @staticmethod
    def get_amd_device_count() -> int:
        """Get the number of AMD GPUs."""
        return _AmdGpuBackend.get_device_count() if _HAS_AMDSMI else 0

    @staticmethod
    def get_device_count() -> int:
        """Total GPU count (NVIDIA + AMD)."""
        count = 0
        if _HAS_PYNVML:
            try:
                count += _NvidiaGpuBackend.get_device_count()
            except Exception:
                pass
        if _HAS_AMDSMI:
            try:
                count += _AmdGpuBackend.get_device_count()
            except Exception:
                pass
        return count

    @staticmethod
    def get_device_name(gpu_index: int) -> str:
        """Get the name of a GPU by index."""
        if _HAS_PYNVML:
            try:
                return _NvidiaGpuBackend.get_device_name(gpu_index)
            except Exception:
                pass
        if _HAS_AMDSMI:
            try:
                return _AmdGpuBackend.get_device_name(gpu_index)
            except Exception:
                pass
        return f"Unknown GPU {gpu_index}"

    @staticmethod
    def get_architecture_name(gpu_index: int) -> str:
        """Get the architecture name of a GPU (NVIDIA only)."""
        if _HAS_PYNVML:
            try:
                return _NvidiaGpuBackend.get_architecture_name(gpu_index)
            except Exception:
                pass
        return "Unknown"

    # ---- Background Polling (Pre-Volta NVIDIA) ----

    def _start_nvml_polling(self):
        """Start background power polling thread.

        Reference: zeus/monitor/power.py -> _domain_polling_process()
        """
        self._nvml_polling_active = True
        self._nvml_polling_thread = threading.Thread(
            target=self._nvml_polling_loop, daemon=True
        )
        self._nvml_polling_thread.start()

    def _stop_nvml_polling(self):
        if self._nvml_polling_active:
            self._nvml_polling_active = False
            if (
                self._nvml_polling_thread
                and self._nvml_polling_thread.is_alive()
            ):
                self._nvml_polling_thread.join(timeout=2.0)

    def _nvml_polling_loop(self):
        while self._nvml_polling_active:
            ts = time.monotonic()
            if self._nvidia:
                for idx in self._nvidia.gpu_indices:
                    if not self._nvidia.supports_energy[idx]:
                        try:
                            power_w = self._nvidia.get_instant_power(idx)
                            if power_w > 0:
                                with self._nvml_samples_lock:
                                    self._nvml_samples.append(
                                        (ts, idx, power_w)
                                    )
                        except Exception:
                            pass
            elapsed = time.monotonic() - ts
            sleep_t = self._polling_interval - elapsed
            if sleep_t > 0:
                time.sleep(sleep_t)

    def _compute_nvml_poll_energy(
        self, gpu_index: int, start: float, end: float
    ) -> float:
        """Trapezoidal integration of polled power samples -> energy (Joules).

        Reference: zeus/monitor/power.py -> PowerMonitor.get_energy()
        """
        with self._nvml_samples_lock:
            timeline = [
                (ts, pw)
                for ts, idx, pw in self._nvml_samples
                if idx == gpu_index and start <= ts <= end
            ]
        if len(timeline) < 2:
            return 0.0
        timeline.sort()
        energy = 0.0
        for i in range(1, len(timeline)):
            dt = timeline[i][0] - timeline[i - 1][0]
            avg_p = (timeline[i][1] + timeline[i - 1][1]) / 2.0
            energy += avg_p * dt
        return energy


# ============================================================================
# Utility: Print Detected Devices
# ============================================================================

def print_detected_devices():
    """Print all detected energy-measurable devices."""
    print("\n--- Detected Devices ---")

    # NVIDIA GPUs
    if _HAS_PYNVML:
        count = pynvml.nvmlDeviceGetCount()
        print(f"  NVIDIA GPUs: {count}")
        for i in range(count):
            handle = pynvml.nvmlDeviceGetHandleByIndex(i)
            name = pynvml.nvmlDeviceGetName(handle)
            if isinstance(name, bytes):
                name = name.decode()
            arch = pynvml.nvmlDeviceGetArchitecture(handle)
            arch_name = _NVIDIA_ARCH_NAMES.get(arch, f"Unknown({arch})")
            print(f"    [GPU {i}] {name} ({arch_name})")
    else:
        print("  NVIDIA GPUs: not available (pynvml not installed)")

    # AMD GPUs
    if _HAS_AMDSMI:
        handles = amdsmi.amdsmi_get_processor_handles()
        print(f"  AMD GPUs: {len(handles)}")
        for i, h in enumerate(handles):
            try:
                info = amdsmi.amdsmi_get_gpu_asic_info(h)
                name = info.get("market_name", "Unknown")
            except Exception:
                name = "Unknown"
            print(f"    [GPU {i}] {name}")
    else:
        print("  AMD GPUs: not available (amdsmi not installed or no ROCm)")

    # CPU (RAPL)
    if _HAS_RAPL:
        rapl = _RaplCpuBackend()
        print(f"  CPUs (RAPL): {len(rapl.cpu_indices)} socket(s)")
        for idx in rapl.cpu_indices:
            dram = "yes" if rapl.supports_dram.get(idx) else "no"
            print(f"    [CPU {idx}] Package {idx} (DRAM: {dram})")
    else:
        if sys.platform != "linux":
            print(
                f"  CPUs (RAPL): not available "
                f"(requires Linux, current: {sys.platform})"
            )
        else:
            print("  CPUs (RAPL): not available (sysfs path not found)")

    # Apple Silicon
    if _HAS_APPLE_SILICON:
        apple = _AppleSiliconBackend()
        metrics = apple.get_available_metrics()
        print(f"  Apple Silicon SoC: available ({len(metrics)} metrics)")
        for m in sorted(metrics):
            print(f"    - {m}")
    else:
        if sys.platform == "darwin":
            print(
                "  Apple Silicon SoC: not available "
                "(pip install zeus-apple-silicon)"
            )
        else:
            print("  Apple Silicon SoC: not available (not macOS)")

    # Jetson
    if _HAS_JETSON:
        jetson = _JetsonBackend()
        print(
            f"  Jetson SoC: available "
            f"({len(jetson._power_readers)} power rail(s))"
        )
        for rail in jetson._power_readers:
            print(f"    - {rail}")
    else:
        print("  Jetson SoC: not available (not Jetson platform)")

    print("------------------------\n")


# ============================================================================
# Simulated Workload
# ============================================================================

def simulate_workload(duration_s: float):
    """Simulate a workload by sleeping (replace with actual compute work)."""
    time.sleep(duration_s)


# ============================================================================
# Example Functions (mirror main.cpp examples)
# ============================================================================

def example_basic_query():
    """Example 1: Basic device info and power query."""
    print("\n========================================")
    print(" Example 1: Device Info & Power Query")
    print("========================================")

    print_detected_devices()

    if not (_HAS_PYNVML or _HAS_AMDSMI):
        print("  No GPU detected, skipping GPU power query.")
        return

    monitor = PowerMonitor(gpu_indices=[0])
    gpu_type = monitor.gpu_type
    print(f"  GPU backend: {gpu_type}")
    print(f"  Instant power (GPU 0): {monitor.get_instant_power(0):.2f} W")

    if gpu_type == "NVIDIA":
        print(
            f"  Energy counter (Volta+): "
            f"{'Yes' if monitor.supports_energy_counter(0) else 'No'}"
        )
        if monitor.supports_energy_counter(0):
            print(
                f"  Cumulative energy (GPU 0): "
                f"{monitor.get_total_energy(0):.2f} J (since driver load)"
            )
        try:
            print(
                f"  Power limit (GPU 0): {monitor.get_power_limit(0):.1f} W"
            )
        except Exception as e:
            print(f"  Power limit: N/A ({e})")


def example_single_window():
    """Example 2: Single measurement window (all devices)."""
    print("\n==========================================")
    print(" Example 2: Single Measurement Window")
    print("==========================================")

    monitor = PowerMonitor()

    monitor.begin_window("workload")
    simulate_workload(3.0)
    result = monitor.end_window("workload")

    print(f"  Duration: {result.elapsed_time:.2f} s")

    if result.gpu_energy:
        print(f"  GPU energy: {result.total_gpu_energy:.4f} J")
        for idx, e in result.gpu_energy.items():
            print(f"    GPU {idx}: {e:.4f} J")

    if result.cpu_energy:
        print(f"  CPU energy: {result.total_cpu_energy:.4f} J")
        for idx, e in result.cpu_energy.items():
            print(f"    CPU {idx}: {e:.4f} J")

    if result.dram_energy:
        print(f"  DRAM energy: {result.total_dram_energy:.4f} J")
        for idx, e in result.dram_energy.items():
            print(f"    DRAM {idx}: {e:.4f} J")

    if result.soc_energy:
        print(f"  SoC energy: {result.total_soc_energy:.4f} J")
        for name, e in result.soc_energy.items():
            print(f"    {name}: {e:.4f} J")

    print(f"  Total energy (all devices): {result.total_energy:.4f} J")


def example_epoch_steps():
    """Example 3: Nested windows (epoch + steps)."""
    print("\n==========================================")
    print(" Example 3: Epoch + Step Measurement")
    print("==========================================")

    monitor = PowerMonitor()

    num_epochs = 3
    steps_per_epoch = 5
    step_duration = 0.5  # seconds

    for epoch in range(num_epochs):
        monitor.begin_window("epoch")

        step_results = []
        for step in range(steps_per_epoch):
            monitor.begin_window("step")
            simulate_workload(step_duration)
            step_results.append(monitor.end_window("step"))

        epoch_result = monitor.end_window("epoch")

        avg_step = sum(m.total_energy for m in step_results) / len(
            step_results
        )

        parts = [f"Total: {epoch_result.total_energy:.4f} J"]
        if epoch_result.gpu_energy:
            parts.append(f"GPU: {epoch_result.total_gpu_energy:.4f} J")
        if epoch_result.cpu_energy:
            parts.append(f"CPU: {epoch_result.total_cpu_energy:.4f} J")
        if epoch_result.dram_energy:
            parts.append(f"DRAM: {epoch_result.total_dram_energy:.4f} J")
        parts.append(f"Avg step: {avg_step:.4f} J")

        print(f"  Epoch {epoch} | {' | '.join(parts)}")


def example_multi_gpu():
    """Example 4: Multi-GPU monitoring."""
    gpu_count = PowerMonitor.get_device_count()
    if gpu_count < 2:
        print("\n==========================================")
        print(
            f" Example 4: Multi-GPU (skipped, need 2+, found {gpu_count})"
        )
        print("==========================================")
        return

    print("\n==========================================")
    print(" Example 4: Multi-GPU Monitoring")
    print("==========================================")

    monitor = PowerMonitor()  # all GPUs

    monitor.begin_window("multi")
    simulate_workload(2.0)
    result = monitor.end_window("multi")

    print(f"  Total GPU energy: {result.total_gpu_energy:.4f} J")
    for idx, e in result.gpu_energy.items():
        print(f"    GPU {idx}: {e:.4f} J")


def example_cpu_monitoring():
    """Example 5: CPU and DRAM energy monitoring (RAPL)."""
    print("\n==========================================")
    print(" Example 5: CPU/DRAM Energy (RAPL)")
    print("==========================================")

    if not _HAS_RAPL:
        print("  Skipped: RAPL not available")
        if sys.platform != "linux":
            print(f"    Requires Linux (current: {sys.platform})")
        else:
            print("    Intel RAPL sysfs not found")
        return

    monitor = PowerMonitor(monitor_gpu=False, monitor_soc=False)

    print(
        f"  Monitoring {len(monitor.cpu_indices)} CPU socket(s): "
        f"{monitor.cpu_indices}"
    )

    monitor.begin_window("cpu_test")
    simulate_workload(2.0)
    result = monitor.end_window("cpu_test")

    if result.cpu_energy:
        print(f"  CPU energy: {result.total_cpu_energy:.4f} J")
        for idx, e in result.cpu_energy.items():
            print(f"    CPU {idx}: {e:.4f} J")

    if result.dram_energy:
        print(f"  DRAM energy: {result.total_dram_energy:.4f} J")
        for idx, e in result.dram_energy.items():
            print(f"    DRAM {idx}: {e:.4f} J")


def example_soc_monitoring():
    """Example 6: SoC energy monitoring (Apple Silicon / Jetson)."""
    print("\n==========================================")
    print(" Example 6: SoC Energy Monitoring")
    print("==========================================")

    if not (_HAS_APPLE_SILICON or _HAS_JETSON):
        print("  Skipped: No SoC backend available")
        print(
            "    Apple Silicon: requires macOS + arm + zeus_apple_silicon ext"
        )
        print("    Jetson: requires Linux + aarch64 + INA3221 sysfs")
        return

    monitor = PowerMonitor(monitor_gpu=False, monitor_cpu=False)
    print(f"  SoC type: {monitor.soc_type}")

    monitor.begin_window("soc_test")
    simulate_workload(2.0)
    result = monitor.end_window("soc_test")

    if result.soc_energy:
        print(f"  SoC total energy: {result.total_soc_energy:.4f} J")
        for name, e in result.soc_energy.items():
            print(f"    {name}: {e:.4f} J")


def example_continuous_monitoring():
    """Example 7: Continuous power monitoring."""
    print("\n==========================================")
    print(" Example 7: Continuous Power Monitoring")
    print("==========================================")

    if not (_HAS_PYNVML or _HAS_AMDSMI):
        print("  Skipped: No GPU backend for instant power reading")
        return

    monitor = PowerMonitor(gpu_indices=[0])

    print("  Reading power every 500ms for 3 seconds...")
    for i in range(6):
        power = monitor.get_instant_power(0)
        print(f"    [{i * 0.5:.1f}s] GPU 0 power: {power:.2f} W")
        time.sleep(0.5)


# ============================================================================
# Main
# ============================================================================

if __name__ == "__main__":
    print("============================================")
    print(" Zeus Power Monitor - Multi-Device Example")
    print("============================================")
    print(" Based on: github.com/ml-energy/zeus")
    print(" Supports: NVIDIA GPU, AMD GPU, CPU (RAPL),")
    print("           Apple Silicon, NVIDIA Jetson")

    try:
        example_basic_query()
        example_single_window()
        example_epoch_steps()
        example_multi_gpu()
        example_cpu_monitoring()
        example_soc_monitoring()
        example_continuous_monitoring()

        print("\n============================================")
        print(" All examples completed successfully.")
        print("============================================")

    except Exception as e:
        print(f"\n[ERROR] {e}")
        raise
