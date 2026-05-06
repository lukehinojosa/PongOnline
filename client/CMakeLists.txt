add_executable(pong_client
        src/main.cpp
        src/storage.cpp
        src/ui.cpp
        src/app.cpp
        src/net.cpp
        src/render.cpp
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
            mbedtls
            mbedx509
            mbedcrypto
            asio::asio
    )
    target_compile_definitions(pong_client PRIVATE PONG_NATIVE)

    # Copy the Assets folder next to the native executable after building
    add_custom_command(TARGET pong_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:pong_client>/Assets"
        COMMENT "Copying Assets directory to build output..."
    )

    # Install the executable to /app/bin
    install(TARGETS pong_client DESTINATION bin)
    # Install the Assets directory next to the executable
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/Assets" DESTINATION bin)
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

            # Package the Assets folder into the WASM virtual file system
            "SHELL:--preload-file \"${CMAKE_SOURCE_DIR}/Assets@/Assets\""
    )
endif()

# ── Compiler warnings ────────────────────────────────────────
if(MSVC)
    target_compile_options(pong_client PRIVATE /W4 /wd4100 /utf-8)
else()
    target_compile_options(pong_client PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
