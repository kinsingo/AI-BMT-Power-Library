"""
Zeus C++ Power Monitor - Python Equivalent

Python implementation of the same GPU power monitoring API as power_monitor.h.
Uses pynvml to directly call NVML APIs (same as the C++ version).

This mirrors the C++ zeus::PowerMonitor class for easy comparison and testing.

Reference Python source files from the Zeus project:
  - zeus/monitor/energy.py   : ZeusMonitor (begin_window / end_window)
  - zeus/device/gpu/nvidia.py: NVML API wrappers
  - zeus/monitor/power.py    : Background power polling for pre-Volta GPUs

Usage:
    pip install -r requirements.txt
    python main.py
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional

import pynvml


# ============================================================================
# Measurement Result
# ============================================================================

@dataclass
class Measurement:
    """Result of a power measurement window.

    Corresponds to zeus.monitor.energy.Measurement and zeus::Measurement (C++).

    Attributes:
        gpu_energy: GPU index -> energy consumed in Joules.
    """

    gpu_energy: Dict[int, float] = field(default_factory=dict)

    @property
    def total_energy(self) -> float:
        """Total energy consumed across all monitored GPUs (Joules)."""
        return sum(self.gpu_energy.values())


# ============================================================================
# Architecture Names
# ============================================================================

_ARCH_NAMES = {
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

# Volta architecture value (used for energy counter support check)
_ARCH_VOLTA = 5


# ============================================================================
# PowerMonitor
# ============================================================================

class PowerMonitor:
    """GPU Power Monitor using NVIDIA NVML.

    Python equivalent of zeus::PowerMonitor (C++).
    Implements the same measurement strategy as zeus.monitor.energy.ZeusMonitor:

    - Volta+ GPUs: Hardware energy counter (nvmlDeviceGetTotalEnergyConsumption)
    - Pre-Volta GPUs: Background thread polling + trapezoidal integration

    Example:
        >>> monitor = PowerMonitor(gpu_indices=[0])
        >>> monitor.begin_window("train")
        >>> # ... GPU workload ...
        >>> result = monitor.end_window("train")
        >>> print(f"Energy: {result.total_energy} J")
    """

    def __init__(
        self,
        gpu_indices: Optional[List[int]] = None,
        polling_interval: float = 0.1,
    ):
        """Initialize the power monitor.

        Args:
            gpu_indices: GPU indices to monitor. None = all GPUs.
            polling_interval: Polling interval in seconds for pre-Volta GPUs.
        """
        pynvml.nvmlInit()

        device_count = pynvml.nvmlDeviceGetCount()

        # Set GPU indices
        if gpu_indices is None:
            self.gpu_indices = list(range(device_count))
        else:
            for idx in gpu_indices:
                if idx < 0 or idx >= device_count:
                    raise ValueError(
                        f"GPU index {idx} out of range [0, {device_count})"
                    )
            self.gpu_indices = list(gpu_indices)

        # Get handles and check capabilities
        self._handles: Dict[int, object] = {}
        self._supports_energy_counter: Dict[int, bool] = {}
        need_polling = False

        for idx in self.gpu_indices:
            handle = pynvml.nvmlDeviceGetHandleByIndex(idx)
            self._handles[idx] = handle

            # Check Volta+ (architecture >= 5)
            try:
                arch = pynvml.nvmlDeviceGetArchitecture(handle)
                supports = arch >= _ARCH_VOLTA
            except pynvml.NVMLError:
                supports = False

            self._supports_energy_counter[idx] = supports
            if not supports:
                need_polling = True

        # Measurement window state
        self._windows: Dict[str, dict] = {}

        # Background polling for pre-Volta GPUs
        self._polling_interval = polling_interval
        self._polling_active = False
        self._samples: list = []
        self._samples_lock = threading.Lock()
        self._polling_thread: Optional[threading.Thread] = None

        if need_polling:
            self._start_polling()

    def __del__(self):
        self._stop_polling()

    # ---- Main Measurement API ----

    def begin_window(self, key: str) -> None:
        """Mark the beginning of a measurement window.

        Corresponds to ZeusMonitor.begin_window() in Python Zeus
        and zeus::PowerMonitor::begin_window() in C++.

        Args:
            key: Unique name for this measurement window.
        """
        if key in self._windows:
            raise RuntimeError(
                f"Window '{key}' already active. Call end_window('{key}') first."
            )

        state = {
            "start_time": time.monotonic(),
            "start_energy": {},
        }

        # Snapshot energy counters for Volta+ GPUs
        for idx in self.gpu_indices:
            if self._supports_energy_counter[idx]:
                # nvmlDeviceGetTotalEnergyConsumption returns millijoules
                energy_mj = pynvml.nvmlDeviceGetTotalEnergyConsumption(
                    self._handles[idx]
                )
                state["start_energy"][idx] = energy_mj / 1000.0  # mJ -> J

        self._windows[key] = state

    def end_window(self, key: str) -> Measurement:
        """Mark the end of a measurement window and return the result.

        Corresponds to ZeusMonitor.end_window() in Python Zeus
        and zeus::PowerMonitor::end_window() in C++.

        Args:
            key: Name of the window started with begin_window().

        Returns:
            Measurement result with per-GPU energy in Joules.
        """
        if key not in self._windows:
            raise RuntimeError(
                f"Window '{key}' not found. Call begin_window('{key}') first."
            )

        end_time = time.monotonic()
        state = self._windows.pop(key)
        result = Measurement()

        for idx in self.gpu_indices:
            if self._supports_energy_counter[idx]:
                # PATH A: Volta+ hardware energy counter
                end_energy_mj = pynvml.nvmlDeviceGetTotalEnergyConsumption(
                    self._handles[idx]
                )
                end_energy = end_energy_mj / 1000.0  # mJ -> J
                result.gpu_energy[idx] = end_energy - state["start_energy"][idx]
            else:
                # PATH B: Pre-Volta trapezoidal integration
                energy = self._compute_energy_from_samples(
                    idx, state["start_time"], end_time
                )
                # Fallback: instant power × time
                if energy <= 0.0:
                    power_w = self.get_instant_power(idx)
                    elapsed = end_time - state["start_time"]
                    energy = power_w * elapsed
                result.gpu_energy[idx] = energy

        return result

    # ---- Power / Energy Query Functions ----

    def get_instant_power(self, gpu_index: int) -> float:
        """Get instantaneous power draw (Watts).

        Uses nvmlDeviceGetPowerUsage.
        """
        power_mw = pynvml.nvmlDeviceGetPowerUsage(self._handles[gpu_index])
        return power_mw / 1000.0  # mW -> W

    def get_total_energy(self, gpu_index: int) -> float:
        """Get cumulative total energy consumption since driver load (Joules).

        Uses nvmlDeviceGetTotalEnergyConsumption (Volta+ only).
        """
        energy_mj = pynvml.nvmlDeviceGetTotalEnergyConsumption(
            self._handles[gpu_index]
        )
        return energy_mj / 1000.0  # mJ -> J

    def supports_energy_counter(self, gpu_index: int) -> bool:
        """Check if GPU supports hardware energy counter (Volta+)."""
        return self._supports_energy_counter.get(gpu_index, False)

    def get_power_limit(self, gpu_index: int) -> float:
        """Get power management limit (Watts)."""
        limit_mw = pynvml.nvmlDeviceGetPowerManagementLimit(
            self._handles[gpu_index]
        )
        return limit_mw / 1000.0  # mW -> W

    # ---- Static Utility Functions ----

    @staticmethod
    def get_device_count() -> int:
        """Get the number of NVIDIA GPUs in the system."""
        pynvml.nvmlInit()
        return pynvml.nvmlDeviceGetCount()

    @staticmethod
    def get_device_name(gpu_index: int) -> str:
        """Get the name of a GPU by index."""
        pynvml.nvmlInit()
        handle = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
        name = pynvml.nvmlDeviceGetName(handle)
        if isinstance(name, bytes):
            name = name.decode("utf-8")
        return name

    @staticmethod
    def get_architecture_name(gpu_index: int) -> str:
        """Get the architecture name of a GPU."""
        pynvml.nvmlInit()
        handle = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
        arch = pynvml.nvmlDeviceGetArchitecture(handle)
        return _ARCH_NAMES.get(arch, f"Unknown (arch={arch})")

    # ---- Background Polling (Pre-Volta) ----

    def _start_polling(self):
        """Start background power polling thread.

        Reference: zeus/monitor/power.py -> _domain_polling_process()
        """
        self._polling_active = True
        self._polling_thread = threading.Thread(
            target=self._polling_loop, daemon=True
        )
        self._polling_thread.start()

    def _stop_polling(self):
        if self._polling_active:
            self._polling_active = False
            if self._polling_thread and self._polling_thread.is_alive():
                self._polling_thread.join(timeout=2.0)

    def _polling_loop(self):
        while self._polling_active:
            ts = time.monotonic()
            for idx in self.gpu_indices:
                if not self._supports_energy_counter[idx]:
                    try:
                        power_mw = pynvml.nvmlDeviceGetPowerUsage(
                            self._handles[idx]
                        )
                        power_w = power_mw / 1000.0
                        if power_w > 0.0:
                            with self._samples_lock:
                                self._samples.append((ts, idx, power_w))
                    except pynvml.NVMLError:
                        pass
            elapsed = time.monotonic() - ts
            sleep_time = self._polling_interval - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

    def _compute_energy_from_samples(
        self, gpu_index: int, start_time: float, end_time: float
    ) -> float:
        """Trapezoidal integration of power samples -> energy (Joules).

        Reference: zeus/monitor/power.py -> PowerMonitor.get_energy()
                   which uses sklearn.metrics.auc() on (timestamp, power) pairs.
        """
        with self._samples_lock:
            timeline = [
                (ts, pw)
                for ts, idx, pw in self._samples
                if idx == gpu_index and start_time <= ts <= end_time
            ]

        if len(timeline) < 2:
            return 0.0

        timeline.sort()

        # Trapezoidal integration
        energy = 0.0
        for i in range(1, len(timeline)):
            dt = timeline[i][0] - timeline[i - 1][0]
            avg_power = (timeline[i][1] + timeline[i - 1][1]) / 2.0
            energy += avg_power * dt

        return energy


# ============================================================================
# Simulated Workload
# ============================================================================

def simulate_workload(duration_s: float):
    """Simulate a GPU workload by sleeping (replace with actual CUDA work)."""
    time.sleep(duration_s)


# ============================================================================
# Example Functions (mirror main.cpp examples)
# ============================================================================

def example_basic_query():
    """Example 1: Basic GPU info and power query."""
    print("\n========================================")
    print(" Example 1: Basic GPU Info & Power Query")
    print("========================================")

    count = PowerMonitor.get_device_count()
    print(f"Detected {count} GPU(s):")

    for i in range(count):
        name = PowerMonitor.get_device_name(i)
        arch = PowerMonitor.get_architecture_name(i)
        print(f"  [GPU {i}] {name}  (Architecture: {arch})")

    monitor = PowerMonitor(gpu_indices=[0])
    print(f"\n  Instant power (GPU 0): {monitor.get_instant_power(0):.2f} W")
    print(
        f"  Energy counter supported (Volta+): "
        f"{'Yes' if monitor.supports_energy_counter(0) else 'No'}"
    )

    if monitor.supports_energy_counter(0):
        print(
            f"  Cumulative energy (GPU 0): "
            f"{monitor.get_total_energy(0):.2f} J (since driver load)"
        )

    try:
        print(f"  Power limit (GPU 0): {monitor.get_power_limit(0):.1f} W")
    except pynvml.NVMLError as e:
        print(f"  Power limit: N/A ({e})")


def example_single_window():
    """Example 2: Single measurement window."""
    print("\n==========================================")
    print(" Example 2: Single Measurement Window")
    print("==========================================")

    monitor = PowerMonitor(gpu_indices=[0])

    monitor.begin_window("workload")
    simulate_workload(3.0)
    result = monitor.end_window("workload")

    print(f"  Total energy consumed: {result.total_energy:.4f} J")
    for gpu_idx, energy in result.gpu_energy.items():
        print(f"    GPU {gpu_idx}: {energy:.4f} J")


def example_epoch_steps():
    """Example 3: Nested windows (epoch + steps)."""
    print("\n==========================================")
    print(" Example 3: Epoch + Step Measurement")
    print("==========================================")

    monitor = PowerMonitor(gpu_indices=[0])

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

        avg_step_energy = sum(m.total_energy for m in step_results) / len(
            step_results
        )

        print(
            f"  Epoch {epoch}"
            f" | Total: {epoch_result.total_energy:.4f} J"
            f" | Avg step: {avg_step_energy:.4f} J"
        )


def example_multi_gpu():
    """Example 4: Multi-GPU monitoring."""
    count = PowerMonitor.get_device_count()
    if count < 2:
        print("\n==========================================")
        print(" Example 4: Multi-GPU (skipped, need 2+ GPUs)")
        print("==========================================")
        return

    print("\n==========================================")
    print(" Example 4: Multi-GPU Monitoring")
    print("==========================================")

    monitor = PowerMonitor()  # all GPUs

    monitor.begin_window("multi")
    simulate_workload(2.0)
    result = monitor.end_window("multi")

    print(f"  Total energy (all GPUs): {result.total_energy:.4f} J")
    for gpu_idx, energy in result.gpu_energy.items():
        print(f"    GPU {gpu_idx}: {energy:.4f} J")


def example_continuous_monitoring():
    """Example 5: Continuous power monitoring."""
    print("\n==========================================")
    print(" Example 5: Continuous Power Monitoring")
    print("==========================================")

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
    print(" Zeus Python Power Monitor - Example Program")
    print("============================================")
    print(" Based on: github.com/ml-energy/zeus")
    print(" Library:  PowerMonitor class (pynvml)")

    try:
        example_basic_query()
        example_single_window()
        example_epoch_steps()
        example_multi_gpu()
        example_continuous_monitoring()

        print("\n============================================")
        print(" All examples completed successfully.")
        print("============================================")

    except Exception as e:
        print(f"\n[ERROR] {e}")
        raise
