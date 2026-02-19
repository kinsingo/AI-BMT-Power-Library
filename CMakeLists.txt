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
option(ZEUS_USE_ROCM_SMI "Enable AMD ROCm SMI support (requires ROCm)"        OFF)

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

# --- AMD ROCm SMI ---
if(ZEUS_USE_ROCM_SMI)
    find_library(ROCM_SMI_LIB rocm_smi64
        HINTS /opt/rocm/lib ENV ROCM_PATH)
    find_path(ROCM_SMI_INCLUDE rocm_smi/rocm_smi.h
        HINTS /opt/rocm/include ENV ROCM_PATH)
    if(ROCM_SMI_LIB AND ROCM_SMI_INCLUDE)
        message(STATUS "ROCm SMI: Found (${ROCM_SMI_LIB})")
    else()
        message(WARNING "ROCm SMI: Not found, disabling AMD support")
        set(ZEUS_USE_ROCM_SMI OFF)
    endif()
endif()

# Check we have at least one backend or CPU-only
if(NOT ZEUS_USE_NVML AND NOT ZEUS_USE_ROCM_SMI)
    message(STATUS "No GPU backend enabled. CPU (RAPL) monitoring only on Linux.")
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

# --- ROCm SMI ---
if(ZEUS_USE_ROCM_SMI)
    target_compile_definitions(zeus_power_monitor INTERFACE ZEUS_USE_ROCM_SMI)
    target_include_directories(zeus_power_monitor INTERFACE ${ROCM_SMI_INCLUDE})
    target_link_libraries(zeus_power_monitor INTERFACE ${ROCM_SMI_LIB})
endif()

# --- Pthreads (Linux) ---
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
message(STATUS "  NVIDIA (NVML):    ${ZEUS_USE_NVML}")
message(STATUS "  AMD (ROCm SMI):   ${ZEUS_USE_ROCM_SMI}")
if(UNIX)
    message(STATUS "  CPU (RAPL):       auto (Linux sysfs)")
endif()
message(STATUS "  C++ standard:     ${CMAKE_CXX_STANDARD}")
