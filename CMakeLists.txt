cmake_minimum_required(VERSION 3.18)
project(zeus_power_monitor LANGUAGES CXX)

# ============================================================================
# C++ Standard
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Options
# ============================================================================
option(ZEUS_USE_NVML     "Enable NVIDIA NVML support (requires CUDA Toolkit)" ON)

# ============================================================================
# Find Dependencies
# ============================================================================

# --- NVIDIA NVML ---
if(ZEUS_USE_NVML)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        message(STATUS "NVML: Found (CUDA Toolkit ${CUDAToolkit_VERSION})")
    else()
        message(WARNING "NVML: CUDAToolkit not found, disabling NVIDIA support")
        set(ZEUS_USE_NVML OFF)
    endif()
endif()

# Check we have at least one backend or CPU-only
if(NOT ZEUS_USE_NVML)
    message(STATUS "No GPU backend enabled. SoC monitoring only.")
endif()

# ============================================================================
# Header-only Library Target
# ============================================================================
add_library(zeus_power_monitor INTERFACE)

target_include_directories(zeus_power_monitor
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# --- NVML ---
if(ZEUS_USE_NVML)
    target_link_libraries(zeus_power_monitor INTERFACE CUDA::nvml)
else()
    target_compile_definitions(zeus_power_monitor INTERFACE ZEUS_NO_NVML)
endif()

# --- Pthreads (Linux) ---
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(zeus_power_monitor INTERFACE Threads::Threads)
endif()

# --- IOKit (macOS, for Apple Silicon SoC backend) ---
if(APPLE)
    target_link_libraries(zeus_power_monitor INTERFACE "-framework IOKit" "-framework CoreFoundation")
endif()

# ============================================================================
# Benchmark Executables (per-device)
# ============================================================================

# NVIDIA GPU benchmark
add_executable(bench_gpu_nvidia main_gpu_nvidia.cpp)
target_link_libraries(bench_gpu_nvidia PRIVATE zeus_power_monitor)

# Jetson SoC benchmark
add_executable(bench_soc_jetson main_soc_jetson.cpp)
target_link_libraries(bench_soc_jetson PRIVATE zeus_power_monitor)

# Apple Silicon benchmark
add_executable(bench_soc_apple main_soc_apple_silicon.cpp)
target_link_libraries(bench_soc_apple PRIVATE zeus_power_monitor)

# ============================================================================
# Install (optional)
# ============================================================================
install(FILES power_monitor.h DESTINATION include/zeus)
install(DIRECTORY device/ DESTINATION include/zeus/device FILES_MATCHING PATTERN "*.h")
install(DIRECTORY monitor/ DESTINATION include/zeus/monitor FILES_MATCHING PATTERN "*.h")
install(TARGETS bench_gpu_nvidia bench_soc_jetson bench_soc_apple
        DESTINATION bin)

# ============================================================================
# Summary
# ============================================================================
message(STATUS "=== Zeus Power Monitor ===")
message(STATUS "  NVIDIA (NVML):       ${ZEUS_USE_NVML}")
if(UNIX)
    message(STATUS "  Jetson SoC (INA3221): auto (Linux sysfs)")
endif()
if(APPLE)
    message(STATUS "  Apple SoC (IOKit):   auto (macOS ARM64)")
endif()
message(STATUS "  C++ standard:        ${CMAKE_CXX_STANDARD}")
