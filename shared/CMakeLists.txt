add_library(pong_shared INTERFACE)
add_library(pong::shared ALIAS pong_shared)

target_include_directories(pong_shared INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(pong_shared INTERFACE cxx_std_20)
