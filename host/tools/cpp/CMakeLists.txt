add_executable(fades_benchmark benchmark.cpp)
target_link_libraries(fades_benchmark PRIVATE fades_stream)
target_compile_options(fades_benchmark PRIVATE -Wall -Wextra -Werror)
