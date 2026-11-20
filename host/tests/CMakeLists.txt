find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(host_cpp_smoke cpp_smoke.cpp)
target_link_libraries(host_cpp_smoke PRIVATE acceldoku_stream)
target_compile_options(host_cpp_smoke PRIVATE -Wall -Wextra -Werror -UNDEBUG)

add_test(NAME host-text-input COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_text_input.py")
add_test(NAME host-abi COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_abi.py")
add_test(NAME host-poll-stop COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_poll_stop.py")
add_test(NAME host-dma-ring COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_stream.py" --tx 8 --rx 64)
add_test(NAME host-dma-ring-single COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_stream.py" --tx 1 --rx 1)
add_test(NAME host-dma-ring-wrap COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_stream.py" --tx 65 --rx 65)
add_test(NAME host-cpp-smoke COMMAND host_cpp_smoke)
set_tests_properties(host-abi host-poll-stop host-dma-ring host-dma-ring-single host-dma-ring-wrap host-cpp-smoke
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
set_tests_properties(host-text-input PROPERTIES TIMEOUT 120)
