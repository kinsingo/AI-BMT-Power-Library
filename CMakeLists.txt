cmake_minimum_required(VERSION 3.18)
project(zeus_power_monitor LANGUAGES CXX)

# ============================================================================
# C++ Standard
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Find NVIDIA NVML (via CUDA Toolkit)
# ============================================================================
# CUDAToolkit provides the CUDA::nvml imported target.
# Requires CUDA Toolkit to be installed (nvml.h + nvml.lib/libnvml.so).
find_package(CUDAToolkit REQUIRED)

# ============================================================================
# Header-only Library Target
# ============================================================================
# zeus_power_monitor is an INTERFACE (header-only) library.
# Users link against it to get include paths and NVML dependency.
add_library(zeus_power_monitor INTERFACE)

target_include_directories(zeus_power_monitor
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(zeus_power_monitor
    INTERFACE
        CUDA::nvml
)

# On Linux, the polling thread requires pthreads
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(zeus_power_monitor INTERFACE Threads::Threads)
endif()

# ============================================================================
# Example Executable
# ============================================================================
add_executable(power_monitor_example main.cpp)
target_link_libraries(power_monitor_example PRIVATE zeus_power_monitor)

# ============================================================================
# Install (optional)
# ============================================================================
install(FILES power_monitor.h DESTINATION include/zeus)
install(TARGETS power_monitor_example DESTINATION bin)

# ============================================================================
# Summary
# ============================================================================
message(STATUS "=== Zeus Power Monitor ===")
message(STATUS "  CUDA Toolkit: ${CUDAToolkit_VERSION}")
message(STATUS "  NVML include: ${CUDAToolkit_INCLUDE_DIRS}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
