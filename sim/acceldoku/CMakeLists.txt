add_executable(stream_test stream_test.cpp)
target_link_libraries(stream_test PRIVATE sudoku_reference)
target_compile_definitions(stream_test PRIVATE ACCELDOKU_CORES=${ACCELDOKU_CORES})
verilate(stream_test SOURCES ${ACCELDOKU_STREAM_RTL} TOP_MODULE acceldoku
    PREFIX Vacceldoku VERILATOR_ARGS --assert --language 1800-2017 -GCORES=${ACCELDOKU_CORES}
    --output-split 20000 --output-split-cfuncs 200)
add_test(NAME acceldoku COMMAND stream_test "${ACCELDOKU_DATASET}" 0)
set_tests_properties(acceldoku PROPERTIES LABELS acceldoku TIMEOUT 3600)
add_test(NAME acceldoku-hard COMMAND stream_test
    "${PROJECT_SOURCE_DIR}/sim/data/HardestDatabase110626.txt" 0)
set_tests_properties(acceldoku-hard PROPERTIES LABELS acceldoku TIMEOUT 3600)
acceldoku_iverilog(problem-preprocessor tb_problem_preprocessor
    SOURCES "${PROJECT_SOURCE_DIR}/rtl/deduction_engine.sv"
    "${PROJECT_SOURCE_DIR}/rtl/problem_preprocessor.sv"
    "${CMAKE_CURRENT_SOURCE_DIR}/tb_problem_preprocessor.sv")
