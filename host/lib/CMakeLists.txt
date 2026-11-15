add_library(fades_stream src/stream_device.cpp)
target_include_directories(fades_stream PUBLIC include ../driver/include)
target_compile_definitions(fades_stream PRIVATE _FILE_OFFSET_BITS=64)
target_compile_options(fades_stream PRIVATE -Wall -Wextra -Werror)
