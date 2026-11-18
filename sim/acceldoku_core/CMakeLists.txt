add_executable(core_test core_test.cpp)
target_link_libraries(core_test PRIVATE sudoku_reference)
verilate(core_test SOURCES ${ACCELDOKU_CORE_RTL} TOP_MODULE acceldoku_core
    PREFIX Vacceldoku_core VERILATOR_ARGS --assert --language 1800-2017
    --output-split 20000 --output-split-cfuncs 200)
add_test(NAME acceldoku-core COMMAND core_test "${ACCELDOKU_DATASET}"
    "${CMAKE_CURRENT_BINARY_DIR}/results.csv" 0)
set_tests_properties(acceldoku-core PROPERTIES LABELS acceldoku_core TIMEOUT 1800)
add_test(NAME acceldoku-core-hard COMMAND core_test
    "${PROJECT_SOURCE_DIR}/sim/data/HardestDatabase110626.txt"
    "${CMAKE_CURRENT_BINARY_DIR}/hard-results.csv" 0)
set_tests_properties(acceldoku-core-hard PROPERTIES LABELS acceldoku_core TIMEOUT 1800)
add_executable(core_preprocessed_test core_test.cpp)
target_link_libraries(core_preprocessed_test PRIVATE sudoku_reference)
verilate(core_preprocessed_test SOURCES ${ACCELDOKU_CORE_RTL}
    "${CMAKE_CURRENT_SOURCE_DIR}/preprocessed.sv" TOP_MODULE acceldoku_core_preprocessed
    PREFIX Vacceldoku_core VERILATOR_ARGS --assert --language 1800-2017
    --output-split 20000 --output-split-cfuncs 200)
add_test(NAME acceldoku-core-preprocessed COMMAND core_preprocessed_test "${ACCELDOKU_DATASET}"
    "${CMAKE_CURRENT_BINARY_DIR}/preprocessed-results.csv" 0)
add_test(NAME acceldoku-core-preprocessed-hard COMMAND core_preprocessed_test
    "${PROJECT_SOURCE_DIR}/sim/data/HardestDatabase110626.txt"
    "${CMAKE_CURRENT_BINARY_DIR}/preprocessed-hard-results.csv" 0)
set_tests_properties(acceldoku-core-preprocessed acceldoku-core-preprocessed-hard
    PROPERTIES LABELS acceldoku_core TIMEOUT 1800)
add_custom_target(core-summary COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core_results.py"
    summary "${CMAKE_CURRENT_BINARY_DIR}/results.csv" "${CMAKE_CURRENT_BINARY_DIR}/summary.json"
    VERBATIM)
if(ACCELDOKU_BASELINE)
    add_custom_target(regression COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core_results.py"
        compare "${ACCELDOKU_BASELINE}" "${CMAKE_CURRENT_BINARY_DIR}/results.csv" VERBATIM)
endif()
set(synth_sources ${ACCELDOKU_CORE_RTL})
list(TRANSFORM synth_sources PREPEND "\"")
list(TRANSFORM synth_sources APPEND "\"")
string(JOIN " " rtl_sources ${synth_sources})
add_test(NAME synth-core COMMAND "${YOSYS_EXECUTABLE}" -Q -q -l "${CMAKE_CURRENT_BINARY_DIR}/synth.log"
    -p "read_verilog -sv ${rtl_sources}; hierarchy -check -top acceldoku_core; proc; opt; memory_dff; memory_collect; opt_clean; check -assert; select -assert-none t:$dlatch t:$_DLATCH_*; stat")
set_tests_properties(synth-core PROPERTIES LABELS acceldoku_core TIMEOUT 120)
add_custom_target(synth COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^synth-core$"
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}" USES_TERMINAL VERBATIM)
