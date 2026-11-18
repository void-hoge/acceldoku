cmake_minimum_required(VERSION 3.25)
project(acceldoku LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(BUILD_TESTING "Build and register tests" ON)
set(ACCELDOKU_PROFILE "dev" CACHE STRING "Build profile: dev, kv260-ps, or kv260-pl")
set_property(CACHE ACCELDOKU_PROFILE PROPERTY STRINGS dev kv260-ps kv260-pl)
if(NOT ACCELDOKU_PROFILE MATCHES "^(dev|kv260-ps|kv260-pl)$")
    message(FATAL_ERROR "Unknown ACCELDOKU_PROFILE: ${ACCELDOKU_PROFILE}")
endif()
set(ACCELDOKU_FASTDOKU_ARCHIVE "" CACHE FILEPATH "Prebuilt native fastdoku FFI archive (optional)")
set(ACCELDOKU_CORES 6 CACHE STRING "Number of solver cores (1..6)")
if(NOT ACCELDOKU_CORES MATCHES "^[1-6]$")
    message(FATAL_ERROR "ACCELDOKU_CORES must be an integer between 1 and 6")
endif()
include(CTest)

add_library(sudoku_reference INTERFACE)
target_include_directories(sudoku_reference INTERFACE "${PROJECT_SOURCE_DIR}/reference")
if(ACCELDOKU_PROFILE STREQUAL "dev" OR ACCELDOKU_PROFILE STREQUAL "kv260-ps")
    find_package(Threads REQUIRED)
    if(ACCELDOKU_FASTDOKU_ARCHIVE)
        set(fastdoku_archive "${ACCELDOKU_FASTDOKU_ARCHIVE}")
    else()
        find_program(CARGO_EXECUTABLE cargo REQUIRED)
        set(fastdoku_manifest "${PROJECT_SOURCE_DIR}/reference/fastdoku-ffi/Cargo.toml")
        if(NOT EXISTS "${PROJECT_SOURCE_DIR}/reference/fastdoku/Cargo.toml")
            message(FATAL_ERROR "fastdoku is missing: run git submodule update --init --recursive")
        endif()
        file(GLOB_RECURSE fastdoku_sources CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/reference/fastdoku/src/*.rs"
            "${PROJECT_SOURCE_DIR}/reference/fastdoku-ffi/src/*.rs")
        set(fastdoku_archive "${PROJECT_BINARY_DIR}/fastdoku/release/libacceldoku_fastdoku_ffi.a")
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
set(ACCELDOKU_CORE_RTL
    "${PROJECT_SOURCE_DIR}/rtl/deduction_engine.sv"
    "${PROJECT_SOURCE_DIR}/rtl/branch_generator.sv"
    "${PROJECT_SOURCE_DIR}/rtl/acceldoku_core.sv")
set(ACCELDOKU_STREAM_RTL ${ACCELDOKU_CORE_RTL}
    "${PROJECT_SOURCE_DIR}/rtl/problem_preprocessor.sv"
    "${PROJECT_SOURCE_DIR}/rtl/async_queue.sv"
    "${PROJECT_SOURCE_DIR}/rtl/acceldoku.sv")
set(ACCELDOKU_AXI_RTL ${ACCELDOKU_STREAM_RTL} "${PROJECT_SOURCE_DIR}/rtl/acceldoku_axi.sv")
if(ACCELDOKU_PROFILE STREQUAL "kv260-ps")
    enable_language(C)
    add_subdirectory(host)
endif()
if(ACCELDOKU_PROFILE STREQUAL "dev")
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR "The dev profile requires BUILD_TESTING=ON")
    endif()
    add_subdirectory(sim)
endif()
if(ACCELDOKU_PROFILE STREQUAL "kv260-pl")
    add_subdirectory(fpga/kv260)
endif()
