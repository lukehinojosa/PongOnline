add_executable(pong_client
        src/main.cpp
        src/webrtc/transport_native.cpp   # compiled on all platforms; #ifdef guards WASM out
        src/webrtc/transport_wasm.cpp     # compiled on all platforms; #ifdef guards native out
)

target_link_libraries(pong_client PRIVATE
        pong::shared
        raylib
        nlohmann_json::nlohmann_json
)

# ── Native build ─────────────────────────────────────────────
if(NOT EMSCRIPTEN)
    target_link_libraries(pong_client PRIVATE
            datachannel-static
            asio::asio
    )
    target_compile_definitions(pong_client PRIVATE PONG_NATIVE)
endif()

# ── WASM build ───────────────────────────────────────────────
if(EMSCRIPTEN)
    target_compile_definitions(pong_client PRIVATE PONG_WASM)

    set_target_properties(pong_client PROPERTIES
            OUTPUT_NAME "index"
            SUFFIX      ".html"
    )

    target_link_options(pong_client PRIVATE
            "SHELL:--shell-file \"${CMAKE_SOURCE_DIR}/template.html\""
            "SHELL:-s USE_GLFW=3"
            "SHELL:-s ASYNCIFY"
            "SHELL:-s ALLOW_MEMORY_GROWTH=1"
            "SHELL:-s EXPORTED_RUNTIME_METHODS=[\"ccall\",\"cwrap\",\"stringToNewUTF8\"]"
            "SHELL:-s EXPORTED_FUNCTIONS=[\"_main\",\"_wasm_on_open\",\"_wasm_on_close\",\"_wasm_on_message\",\"_wasm_on_lobby_code\"]"
            "SHELL:-s WEBSOCKET_SUBPROTOCOL=\"binary\""
    )
endif()

# ── Compiler warnings ────────────────────────────────────────
if(MSVC)
    target_compile_options(pong_client PRIVATE /W4 /wd4100 /utf-8)
else()
    target_compile_options(pong_client PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
