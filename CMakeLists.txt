cmake_minimum_required(VERSION 3.10...4.0)
project(OnlinePong VERSION 0.1.0 LANGUAGES C CXX)

option(BUILD_CLIENT "Build the desktop/WASM client" ON)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POLICY_DEFAULT_CMP0127 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Platform detection ────────────────────────────────────────
if(EMSCRIPTEN)
    message(STATUS "[OnlinePong] Target: WebAssembly (Emscripten)")
else()
    message(STATUS "[OnlinePong] Target: Native (${CMAKE_SYSTEM_NAME})")
endif()

include(cmake/deps.cmake)

add_subdirectory(shared)

if(BUILD_CLIENT)
    add_subdirectory(client)
endif()

# Signaling server is native-only — just brokers WebRTC handshakes
if(NOT EMSCRIPTEN)
    add_subdirectory(signaling)
endif()
