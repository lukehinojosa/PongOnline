include(FetchContent)

# ── 1. GLOBAL COMPILER DEFINITIONS ───────────────────────────
add_compile_definitions(MBEDTLS_SSL_DTLS_SRTP)

# FORCE EVERYTHING TO BE STATIC
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# ── 2. MbedTLS (Fetched First) ───────────────────────────────
set(ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(mbedtls
        GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
        GIT_TAG        v3.6.2
        GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(mbedtls)

# ── 3. MbedTLS Spoofing ──────────────────────────────────────
set(MbedTLS_FOUND TRUE CACHE BOOL "" FORCE)
set(MbedTLS_VERSION "3.6.2" CACHE STRING "" FORCE)
set(MbedTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(MbedTLS_INCLUDE_DIR  "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)

# MSBuild will read these and look for 'mbedtls.lib', 'mbedx509.lib', etc.
set(MbedTLS_LIBRARY    mbedtls    CACHE STRING "" FORCE)
set(MbedX509_LIBRARY   mbedx509   CACHE STRING "" FORCE)
set(MbedCrypto_LIBRARY mbedcrypto CACHE STRING "" FORCE)
set(MBEDTLS_LIBRARIES  mbedtls mbedx509 mbedcrypto CACHE STRING "" FORCE)

# ── 4. libdatachannel ────────────────────────────────────────
if(NOT EMSCRIPTEN)
    set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
    set(NO_MEDIA    ON CACHE BOOL "" FORCE)

    FetchContent_Declare(libdatachannel
            GIT_REPOSITORY    https://github.com/paullouisageneau/libdatachannel.git
            GIT_TAG           v0.24.1
            GIT_SHALLOW       FALSE
            GIT_SUBMODULES_RECURSIVE YES
    )
    FetchContent_MakeAvailable(libdatachannel)

    # ── MSBuild Linker Fix (LNK1104) ─────────────────────────
    # We tell MSBuild exactly where to look for the compiled .lib files.
    # We apply this to both datachannel and datachannel-static (if built).
    if(TARGET datachannel)
        add_dependencies(datachannel mbedtls mbedcrypto mbedx509)
        target_link_directories(datachannel PUBLIC
                $<TARGET_FILE_DIR:mbedtls>
                $<TARGET_FILE_DIR:mbedx509>
                $<TARGET_FILE_DIR:mbedcrypto>
        )
    endif()
    if(TARGET datachannel-static)
        add_dependencies(datachannel-static mbedtls mbedcrypto mbedx509)
        target_link_directories(datachannel-static PUBLIC
                $<TARGET_FILE_DIR:mbedtls>
                $<TARGET_FILE_DIR:mbedx509>
                $<TARGET_FILE_DIR:mbedcrypto>
        )
    endif()
endif()

# ── 5. nlohmann_json (Collision Guard) ───────────────────────
if(NOT TARGET nlohmann_json)
    FetchContent_Declare(nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG        v3.12.0
            GIT_SHALLOW    FALSE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
endif()

# ── 6. Raylib ────────────────────────────────────────────────
if(BUILD_CLIENT)
    FetchContent_Declare(raylib
            GIT_REPOSITORY https://github.com/raysan5/raylib.git
            GIT_TAG        5.5
            GIT_SHALLOW    FALSE
    )
    FetchContent_MakeAvailable(raylib)
endif()

# ── 7. Standalone Asio ───────────────────────────────────────
FetchContent_Declare(asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        asio-1-30-2
        GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(asio)

if(NOT TARGET asio::asio)
    add_library(asio INTERFACE)
    add_library(asio::asio ALIAS asio)
    target_include_directories(asio INTERFACE "${asio_SOURCE_DIR}/asio/include")
    target_compile_definitions(asio INTERFACE ASIO_STANDALONE)

    if(WIN32)
        target_compile_definitions(asio INTERFACE _WIN32_WINNT=0x0A00)
        target_link_libraries(asio INTERFACE ws2_32 mswsock)
    endif()
endif()