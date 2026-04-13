add_executable(pong_signaling
    src/main.cpp
)

target_link_libraries(pong_signaling PRIVATE
    pong::shared
    datachannel-static
    asio::asio
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(pong_signaling PRIVATE /W4 /wd4100 /utf-8)
else()
    target_compile_options(pong_signaling PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ── Download cloudflared for WSS tunneling ────────────────────────────────────
# Fetches the cloudflared binary and places it next to pong_signaling so the
# server can spawn a trycloudflare.com quick tunnel on startup (no account needed).
if(WIN32)
    set(_CF_URL "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe")
    set(_CF_BIN "cloudflared.exe")
elseif(APPLE)
    set(_CF_URL "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-darwin-amd64")
    set(_CF_BIN "cloudflared")
else()
    set(_CF_URL "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64")
    set(_CF_BIN "cloudflared")
endif()

add_custom_command(TARGET pong_signaling POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        "-DURL=${_CF_URL}"
        "-DOUTPUT=$<TARGET_FILE_DIR:pong_signaling>/${_CF_BIN}"
        -P "${CMAKE_SOURCE_DIR}/cmake/download_if_missing.cmake"
    COMMENT "Ensuring cloudflared is present for WSS tunneling..."
    VERBATIM
)
