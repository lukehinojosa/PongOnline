#include "storage.h"

#ifdef PONG_WASM
#  include <emscripten/emscripten.h>

EM_JS(void, js_storage_set, (const char* key, const char* val), {
    localStorage.setItem(UTF8ToString(key), UTF8ToString(val));
});
EM_JS(int, js_storage_get, (const char* key, char* buf, int len), {
    var val = localStorage.getItem(UTF8ToString(key));
    if (!val) return 0;
    var bytes = lengthBytesUTF8(val) + 1;
    if (bytes > len) bytes = len;
    stringToUTF8(val, buf, bytes);
    return 1;
});

// Paste; listen for the browser paste event and buffer the text for polling.
EM_JS(void, js_setup_paste_listener, (), {
    window._pongPaste = '';
    window._pongPastePending = false;
    document.addEventListener('paste', function(e) {
        window._pongPaste = (e.clipboardData || window.clipboardData).getData('text');
        window._pongPastePending = true;
        e.preventDefault();
    });
});
EM_JS(int, js_consume_paste, (char* buf, int len), {
    if (!window._pongPastePending) return 0;
    window._pongPastePending = false;
    var t = window._pongPaste || '';
    var bytes = Math.min(lengthBytesUTF8(t) + 1, len);
    stringToUTF8(t, buf, bytes);
    return 1;
});

// Copy; try the modern async API, fall back to the textarea trick.
EM_JS(void, js_copy_text, (const char* text), {
    var t = UTF8ToString(text);
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(t).catch(function() {
            var ta = document.createElement('textarea');
            ta.value = t;
            ta.style.cssText = 'position:fixed;opacity:0;top:0;left:0';
            document.body.appendChild(ta);
            ta.focus(); ta.select();
            try { document.execCommand('copy'); } catch(e) {}
            document.body.removeChild(ta);
        });
    }
});

// Viewport dimensions (for fullscreen canvas sizing).
EM_JS(int, js_viewport_w, (), { return window.innerWidth;  });
EM_JS(int, js_viewport_h, (), { return window.innerHeight; });

static char s_paste_buf[1024] = {};

std::string storage_get(const char* key, const char* default_val) {
    char buf[256] = {};
    return js_storage_get(key, buf, sizeof(buf)) ? buf : default_val;
}
void storage_set(const char* key, const char* value) {
    js_storage_set(key, value);
}
const char* get_paste_text() {
    return js_consume_paste(s_paste_buf, sizeof(s_paste_buf)) ? s_paste_buf : nullptr;
}
void copy_text(const char* t) { js_copy_text(t); }

#else
#  include <fstream>
#  include <map>
#  include "raylib.h"

static std::map<std::string, std::string> s_prefs;
static bool s_prefs_loaded = false;

static void load_prefs_file() {
    std::ifstream f("pong_prefs.txt");
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos)
            s_prefs[line.substr(0, eq)] = line.substr(eq + 1);
    }
    s_prefs_loaded = true;
}
static void save_prefs_file() {
    std::ofstream f("pong_prefs.txt");
    for (auto& [k, v] : s_prefs) f << k << "=" << v << "\n";
}

std::string storage_get(const char* key, const char* default_val) {
    if (!s_prefs_loaded) load_prefs_file();
    auto it = s_prefs.find(key);
    return it != s_prefs.end() ? it->second : default_val;
}
void storage_set(const char* key, const char* value) {
    if (!s_prefs_loaded) load_prefs_file();
    s_prefs[key] = value;
    save_prefs_file();
}
const char* get_paste_text() { return GetClipboardText(); }
void copy_text(const char* t) { SetClipboardText(t); }

#endif
