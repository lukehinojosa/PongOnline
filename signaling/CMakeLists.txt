add_executable(pong_signaling
    src/main.cpp
)

target_link_libraries(pong_signaling PRIVATE
    pong::shared
    datachannel-static
    mbedtls
    mbedx509
    mbedcrypto
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
    # Detect if targeting Windows Arm64
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_CF_URL "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-arm64.exe")
    else()
        set(_CF_URL "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe")
    endif()
    set(_CF_BIN "cloudflared.exe")
elseif(APPLE)
    # Detect if on Apple Silicon (M1+) or Intel
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_CF_URL "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-darwin-arm64")
    else()
        set(_CF_URL "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-darwin-amd64")
    endif()
    set(_CF_BIN "cloudflared")
else()
    # Detect the architecture on Linux
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_CF_URL "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
        set(_CF_URL "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-arm")
    else()
        set(_CF_URL "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64")
    endif()

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
