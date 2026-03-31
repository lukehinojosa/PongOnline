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
