# Called as: cmake -DURL=<url> -DOUTPUT=<path> -P download_if_missing.cmake
# Downloads the file at URL to OUTPUT only if OUTPUT doesn't already exist.

if(EXISTS "${OUTPUT}")
    message(STATUS "[cloudflared] already present, skipping download.")
    return()
endif()

message(STATUS "[cloudflared] downloading from ${URL} ...")
file(DOWNLOAD "${URL}" "${OUTPUT}"
    SHOW_PROGRESS
    STATUS dl_status
    TLS_VERIFY ON
)
list(GET dl_status 0 dl_code)
if(NOT dl_code EQUAL 0)
    list(GET dl_status 1 dl_error)
    message(WARNING "[cloudflared] download failed (${dl_error}). WSS tunneling will be unavailable.")
    file(REMOVE "${OUTPUT}")
else()
    if(NOT WIN32)
        execute_process(COMMAND chmod +x "${OUTPUT}")
    endif()
    message(STATUS "[cloudflared] downloaded successfully.")
endif()
