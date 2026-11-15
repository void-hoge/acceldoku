cmake_minimum_required(VERSION 3.25)
project(fades LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(BUILD_TESTING "Build and register tests" OFF)
option(FADES_BUILD_DRIVER "Build the native Linux kernel module as part of the default build" OFF)
option(FADES_BUILD_CPP "Build the C++ streaming library and benchmark" ON)
option(FADES_BUILD_VERIFY "Build the reference-based FPGA verifier" OFF)
set(FADES_FASTDOKU_ARCHIVE "" CACHE FILEPATH "Prebuilt native fastdoku FFI archive (optional)")
option(FADES_BUILD_SIM "Build RTL simulations and checks" OFF)
option(FADES_BUILD_FPGA "Enable KV260 implementation targets" OFF)
set(FADES_CORES 6 CACHE STRING "Number of solver cores (1..6)")
if(NOT FADES_CORES MATCHES "^[1-6]$")
    message(FATAL_ERROR "FADES_CORES must be an integer between 1 and 6")
endif()
include(CTest)

add_library(sudoku_reference INTERFACE)
target_include_directories(sudoku_reference INTERFACE "${PROJECT_SOURCE_DIR}/reference")
if(FADES_BUILD_SIM OR FADES_BUILD_VERIFY)
    find_package(Threads REQUIRED)
    if(FADES_FASTDOKU_ARCHIVE)
        set(fastdoku_archive "${FADES_FASTDOKU_ARCHIVE}")
    else()
        find_program(CARGO_EXECUTABLE cargo REQUIRED)
        set(fastdoku_manifest "${PROJECT_SOURCE_DIR}/reference/fastdoku-ffi/Cargo.toml")
        if(NOT EXISTS "${PROJECT_SOURCE_DIR}/reference/fastdoku/Cargo.toml")
            message(FATAL_ERROR "fastdoku is missing: run git submodule update --init --recursive")
        endif()
        file(GLOB_RECURSE fastdoku_sources CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/reference/fastdoku/src/*.rs"
            "${PROJECT_SOURCE_DIR}/reference/fastdoku-ffi/src/*.rs")
        set(fastdoku_archive "${PROJECT_BINARY_DIR}/fastdoku/release/libfades_fastdoku_ffi.a")
        add_custom_command(OUTPUT "${fastdoku_archive}"
            COMMAND "${CARGO_EXECUTABLE}" build --release --locked
                --manifest-path "${fastdoku_manifest}" --target-dir "${PROJECT_BINARY_DIR}/fastdoku"
            DEPENDS ${fastdoku_sources} "${fastdoku_manifest}"
                "${PROJECT_SOURCE_DIR}/reference/fastdoku-ffi/Cargo.lock"
                "${PROJECT_SOURCE_DIR}/reference/fastdoku/Cargo.toml"
                "${PROJECT_SOURCE_DIR}/reference/fastdoku/build.rs"
            VERBATIM)
        add_custom_target(fastdoku_build DEPENDS "${fastdoku_archive}")
    endif()
    add_library(fastdoku_ffi STATIC IMPORTED GLOBAL)
    set_target_properties(fastdoku_ffi PROPERTIES IMPORTED_LOCATION "${fastdoku_archive}")
    if(TARGET fastdoku_build)
        add_dependencies(fastdoku_ffi fastdoku_build)
    endif()
    target_link_libraries(sudoku_reference INTERFACE fastdoku_ffi Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
if(FADES_BUILD_VERIFY AND NOT FADES_BUILD_CPP)
    message(FATAL_ERROR "FADES_BUILD_VERIFY requires FADES_BUILD_CPP=ON")
endif()
set(FADES_CORE_RTL
    "${PROJECT_SOURCE_DIR}/rtl/deduction_engine.sv"
    "${PROJECT_SOURCE_DIR}/rtl/branch_generator.sv"
    "${PROJECT_SOURCE_DIR}/rtl/fades_core.sv")
set(FADES_STREAM_RTL ${FADES_CORE_RTL}
    "${PROJECT_SOURCE_DIR}/rtl/problem_preprocessor.sv"
    "${PROJECT_SOURCE_DIR}/rtl/async_queue.sv"
    "${PROJECT_SOURCE_DIR}/rtl/fades.sv")
set(FADES_AXI_RTL ${FADES_STREAM_RTL} "${PROJECT_SOURCE_DIR}/rtl/fades_axi.sv")
if(FADES_BUILD_DRIVER OR FADES_BUILD_CPP)
    enable_language(C)
    add_subdirectory(host)
endif()
if(FADES_BUILD_SIM)
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR "FADES_BUILD_SIM requires BUILD_TESTING=ON")
    endif()
    add_subdirectory(sim)
endif()
if(FADES_BUILD_FPGA)
    add_subdirectory(fpga/kv260)
endif()
