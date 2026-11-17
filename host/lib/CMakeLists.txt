add_library(acceldoku_stream src/stream_device.cpp)
target_include_directories(acceldoku_stream PUBLIC include ../driver/include)
target_compile_definitions(acceldoku_stream PRIVATE _FILE_OFFSET_BITS=64)
target_compile_options(acceldoku_stream PRIVATE -Wall -Wextra -Werror)
