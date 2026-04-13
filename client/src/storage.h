#pragma once
#include <chrono>
#include <string>

// Persistent key/value storage (localStorage on WASM, pong_prefs.txt on native).
std::string storage_get(const char* key, const char* default_val);
void storage_set(const char* key, const char* value);

// Clipboard; WASM uses the async paste-event buffer, native uses raylib.
const char* get_paste_text();
void copy_text(const char* t);

// Monotonic millisecond timestamp.
inline double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
