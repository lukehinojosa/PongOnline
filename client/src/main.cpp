#include "pong/codec.h"
#include "pong/messages.h"
#include "pong/role.h"
#include "pong/sim.h"
#include "pong/transport.h"

#include "raylib.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#ifdef PONG_WASM
#  include <emscripten/emscripten.h>
#endif

// Config
static constexpr int SCREEN_W = 800;
static constexpr int SCREEN_H = 660;   // 600px game field + 60px HUD
static constexpr int HUD_H    = 60;
static constexpr int TICK_HZ  = 60;

static constexpr uint32_t INTERP_DELAY_TICKS = 6;
static constexpr int      SNAPSHOT_BUF_MAX   = 32;
static constexpr double   PING_INTERVAL_MS   = 500.0;
static constexpr float    RTT_EWMA_ALPHA     = 0.125f;

// Persistent storage
#ifdef PONG_WASM
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
#else
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
#endif

static std::string storage_get(const char* key, const char* default_val) {
#ifdef PONG_WASM
    char buf[256] = {};
    return js_storage_get(key, buf, sizeof(buf)) ? buf : default_val;
#else
    if (!s_prefs_loaded) load_prefs_file();
    auto it = s_prefs.find(key);
    return it != s_prefs.end() ? it->second : default_val;
#endif
}
static void storage_set(const char* key, const char* value) {
#ifdef PONG_WASM
    js_storage_set(key, value);
#else
    if (!s_prefs_loaded) load_prefs_file();
    s_prefs[key] = value;
    save_prefs_file();
#endif
}

// Clipboard wrappers
// WASM uses an async browser API driven by the paste event listener above.
// Native uses raylib's synchronous clipboard functions.
#ifdef PONG_WASM
static char s_paste_buf[1024] = {};
static const char* get_paste_text() {
    return js_consume_paste(s_paste_buf, sizeof(s_paste_buf)) ? s_paste_buf : nullptr;
}
static void copy_text(const char* t) { js_copy_text(t); }
#else
static const char* get_paste_text() { return GetClipboardText(); }
static void copy_text(const char* t) { SetClipboardText(t); }
#endif

// Utilities
static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Find the character index that corresponds to a pixel x-offset
// using the default raylib font at the given font_size.
static int chars_at_x(const std::string& text, float x_off, int font_size) {
    x_off = std::max(x_off, 0.f);
    for (int i = 0; i < (int)text.size(); ++i) {
        float w0 = (float)MeasureText(text.substr(0, i).c_str(), font_size);
        float w1 = (float)MeasureText(text.substr(0, i + 1).c_str(), font_size);
        if (x_off * 2.f <= w0 + w1)
            return i;
    }
    return (int)text.size();
}

// Same but using a custom Font with DrawTextEx metrics.
static int chars_at_x_ex(const Font& font, float size, float spacing,
                          const std::string& text, float x_off) {
    x_off = std::max(x_off, 0.f);
    for (int i = 0; i < (int)text.size(); ++i) {
        float w0 = MeasureTextEx(font, text.substr(0, i).c_str(),     size, spacing).x;
        float w1 = MeasureTextEx(font, text.substr(0, i + 1).c_str(), size, spacing).x;
        if (x_off * 2.f <= w0 + w1)
            return i;
    }
    return (int)text.size();
}

// TextEdit
// A self-contained editable text field with cursor, selection, and scroll.
struct TextEdit {
    std::string text;
    int cursor = 0;   // insertion point [0, text.size()]
    int anchor = 0;   // other end of selection; cursor==anchor so no selection
    int scroll = 0;   // first visible character index
    bool focused = false;
    bool mouse_held = false;

    bool has_sel() const { return cursor != anchor; }
    int sel_lo() const { return std::min(cursor, anchor); }
    int sel_hi() const { return std::max(cursor, anchor); }

    void select_all() { anchor = 0; cursor = (int)text.size(); }
    std::string selected() const {
        if (!has_sel())
            return "";
        return text.substr(sel_lo(), sel_hi() - sel_lo());
    }
    void delete_sel() {
        if (!has_sel()) return;
        int lo = sel_lo(), hi = sel_hi();
        text.erase(lo, hi - lo);
        cursor = anchor = lo;
    }
    void insert_char(char c) {
        delete_sel();
        text.insert(cursor, 1, c);
        anchor = ++cursor;
    }
    void move_to(int pos, bool extend) {
        cursor = std::clamp(pos, 0, (int)text.size());
        if (!extend)
            anchor = cursor;
    }
};

// Adjust scroll so the cursor remains visible inside box_w (including padding).
static void te_adjust_scroll(TextEdit& te, int box_w, int font_size, int pad) {
    int max_w = box_w - pad * 2;
    te.scroll = std::clamp(te.scroll, 0, (int)te.text.size());
    if (te.cursor < te.scroll)
        te.scroll = te.cursor;
    while (te.cursor > te.scroll) {
        int sub_w = MeasureText(te.text.substr(te.scroll, te.cursor - te.scroll).c_str(), font_size);
        if (sub_w <= max_w)
            break;
        ++te.scroll;
    }
}

// Draw a text-edit box (default font).
static void draw_text_edit(TextEdit& te, Rectangle box, int font_size) {
    const int pad = 6;
    const int max_w = (int)box.width - pad * 2;
    const int tx = (int)box.x + pad;
    const int ty = (int)(box.y + box.height / 2.f) - font_size / 2;

    DrawRectangleRec(box, Color{20, 20, 20, 255});
    DrawRectangleLinesEx(box, 2, te.focused ? WHITE : Color{100, 100, 100, 255});

    // Build visible string from scroll position.
    std::string vis = te.text.substr(std::min(te.scroll, (int)te.text.size()));
    while (!vis.empty() && MeasureText(vis.c_str(), font_size) > max_w)
        vis.pop_back();
    int vis_len = (int)vis.size();

    // Map cursor / anchor into visible-string coordinates.
    int vis_cur = std::clamp(te.cursor - te.scroll, 0, vis_len);
    int vis_anc = std::clamp(te.anchor - te.scroll, 0, vis_len);
    int vis_lo = std::min(vis_cur, vis_anc);
    int vis_hi = std::max(vis_cur, vis_anc);

    // Selection highlight.
    if (te.focused && te.has_sel()) {
        int sx = tx + MeasureText(vis.substr(0, vis_lo).c_str(), font_size);
        int ex = tx + MeasureText(vis.substr(0, vis_hi).c_str(), font_size);
        DrawRectangle(sx, ty - 2, ex - sx, font_size + 4, Color{70, 120, 200, 180});
    }

    DrawText(vis.c_str(), tx, ty, font_size, WHITE);

    // Blinking cursor (only when no selection).
    if (te.focused && !te.has_sel() && (int)(GetTime() * 2) % 2 == 0) {
        int cx = tx + MeasureText(vis.substr(0, vis_cur).c_str(), font_size);
        DrawRectangle(cx, ty, 2, font_size, WHITE);
    }
}

// Update a text-edit box; handle mouse (click to focus/position, drag to select)
// and keyboard (typing, backspace, arrows, ctrl shortcuts).
// Returns true if `text` was modified.
static bool update_text_edit(TextEdit& te, Rectangle box, int font_size, int max_len,
                              bool uppercase = false) {
    const int pad = 6;
    const int max_w = (int)box.width - pad * 2;

    Vector2 mp = GetMousePosition();
    bool in_box = CheckCollisionPointRec(mp, box);

    // Mouse
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        if (in_box) {
            te.focused = true;
            te.mouse_held = true;
            float x_off = mp.x - box.x - pad;
            std::string vis = te.text.substr(std::min(te.scroll, (int)te.text.size()));
            while (!vis.empty() && MeasureText(vis.c_str(), font_size) > max_w)
                vis.pop_back();
            int pos = te.scroll + chars_at_x(vis, x_off, font_size);
            te.cursor = te.anchor = std::clamp(pos, 0, (int)te.text.size());
            te_adjust_scroll(te, (int)box.width, font_size, pad);
        } else {
            te.focused = false;
            te.mouse_held = false;
        }
    }
    if (te.mouse_held && IsMouseButtonDown(MOUSE_LEFT_BUTTON) && te.focused) {
        float x_off = std::clamp(mp.x - box.x - pad, 0.f, (float)max_w);
        std::string vis = te.text.substr(std::min(te.scroll, (int)te.text.size()));
        while (!vis.empty() && MeasureText(vis.c_str(), font_size) > max_w)
            vis.pop_back();
        int pos = te.scroll + chars_at_x(vis, x_off, font_size);
        te.cursor = std::clamp(pos, 0, (int)te.text.size());
        te_adjust_scroll(te, (int)box.width, font_size, pad);
    }
    if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) te.mouse_held = false;

    if (!te.focused) return false;

    // Keyboard
    bool changed = false;
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (ctrl && IsKeyPressed(KEY_A)) { te.select_all(); return false; }

    if (ctrl && IsKeyPressed(KEY_C)) {
        copy_text(te.has_sel() ? te.selected().c_str() : te.text.c_str());
        return false;
    }
    if (ctrl && IsKeyPressed(KEY_X)) {
        if (te.has_sel()) {
            copy_text(te.selected().c_str());
            te.delete_sel();
            changed = true;
        }
        te_adjust_scroll(te, (int)box.width, font_size, pad);
        return changed;
    }
    if (ctrl && IsKeyPressed(KEY_V)) {
        const char* clip = get_paste_text();
        if (clip) {
            te.delete_sel();
            for (char c : std::string(clip)) {
                if ((int)te.text.size() >= max_len)
                    break;
                if (c < 32 || c > 126)
                    continue;
                te.insert_char((uppercase && c >= 'a' && c <= 'z') ? c - 32 : c);
                changed = true;
            }
        }
        te_adjust_scroll(te, (int)box.width, font_size, pad);
        return changed;
    }

    auto nav = [&](int pos, bool ext) {
        te.move_to(pos, ext);
        te_adjust_scroll(te, (int)box.width, font_size, pad);
    };
    if (IsKeyPressed(KEY_LEFT)  || IsKeyPressedRepeat(KEY_LEFT))
        nav(te.has_sel() && !shift ? te.sel_lo() : te.cursor - 1, shift);
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT))
        nav(te.has_sel() && !shift ? te.sel_hi() : te.cursor + 1, shift);
    if (IsKeyPressed(KEY_HOME))
        nav(0, shift);
    if (IsKeyPressed(KEY_END))
        nav((int)te.text.size(), shift);

    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (te.has_sel()) { te.delete_sel(); changed = true; }
        else if (te.cursor > 0) {
            te.text.erase(--te.cursor, 1);
            te.anchor = te.cursor;
            changed = true;
        }
        te_adjust_scroll(te, (int)box.width, font_size, pad);
    }
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) {
        if (te.has_sel()) { te.delete_sel(); changed = true; }
        else if (te.cursor < (int)te.text.size()) { te.text.erase(te.cursor, 1); changed = true; }
        te_adjust_scroll(te, (int)box.width, font_size, pad);
    }

    int key = GetCharPressed();
    while (key > 0) {
        if ((int)te.text.size() < max_len && key >= 32 && key <= 126) {
            te.insert_char((uppercase && key >= 'a' && key <= 'z') ? key - 32 : (char)key);
            changed = true;
        }
        key = GetCharPressed();
    }
    if (changed)
        te_adjust_scroll(te, (int)box.width, font_size, pad);
    return changed;
}

// Code-font text edit (large, for join code entry)
// This variant uses a custom Font (g_app.code_font) and draws the text free-floating
// (no scroll; the field is short enough). A visible box appears when focused.
static Font* g_code_font_ptr = nullptr; // set in main() before first use

static void draw_code_edit(TextEdit& te, Vector2 pos, float size, float spacing, Color color) {
    const Font& font = *g_code_font_ptr;

    // Background box; appears when focused or always, for consistent layout.
    Vector2 tsz = te.text.empty()
                ? Vector2{size * 0.6f, size}
                : MeasureTextEx(font, te.text.c_str(), size, spacing);
    float bx = pos.x - 6, by = pos.y - 4;
    float bw = std::max(tsz.x, size * 6 * 0.6f) + 12, bh = size + 8;
    DrawRectangle((int)bx, (int)by, (int)bw, (int)bh, Color{20, 20, 20, 180});
    DrawRectangleLinesEx({bx, by, bw, bh}, 2, te.focused ? WHITE : Color{80, 80, 80, 255});

    // Selection highlight.
    if (te.focused && te.has_sel()) {
        float x0 = pos.x + MeasureTextEx(font, te.text.substr(0, te.sel_lo()).c_str(), size, spacing).x;
        float x1 = pos.x + MeasureTextEx(font, te.text.substr(0, te.sel_hi()).c_str(), size, spacing).x;
        DrawRectangle((int)x0, (int)pos.y - 2, (int)(x1 - x0), (int)size + 4, Color{70, 120, 200, 180});
    }

    DrawTextEx(font, te.text.c_str(), pos, size, spacing, color);

    // Blinking cursor.
    if (te.focused && !te.has_sel() && (int)(GetTime() * 2) % 2 == 0) {
        float cx = pos.x + MeasureTextEx(font, te.text.substr(0, te.cursor).c_str(), size, spacing).x;
        DrawRectangle((int)cx, (int)pos.y, 2, (int)size, color);
    }
}

static bool update_code_edit(TextEdit& te, Vector2 pos, float size, float spacing,
                              int max_len, bool uppercase = false) {
    const Font& font = *g_code_font_ptr;

    Vector2 tsz = MeasureTextEx(font, te.text.empty() ? " " : te.text.c_str(), size, spacing);
    float bw = std::max(tsz.x, size * (float)max_len * 0.6f) + 12;
    Rectangle box = { pos.x - 6, pos.y - 4, bw, size + 8 };

    Vector2 mp = GetMousePosition();
    bool in_box = CheckCollisionPointRec(mp, box);

    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        if (in_box) {
            te.focused = true;
            te.mouse_held = true;
            float x_off = mp.x - pos.x;
            int p = chars_at_x_ex(font, size, spacing, te.text, x_off);
            te.cursor = te.anchor = std::clamp(p, 0, (int)te.text.size());
        } else {
            te.focused = false;
            te.mouse_held = false;
        }
    }
    if (te.mouse_held && IsMouseButtonDown(MOUSE_LEFT_BUTTON) && te.focused) {
        int p = chars_at_x_ex(font, size, spacing, te.text, std::max(mp.x - pos.x, 0.f));
        te.cursor = std::clamp(p, 0, (int)te.text.size());
    }
    if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON))
        te.mouse_held = false;

    if (!te.focused) return false;

    bool changed = false;
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (ctrl && IsKeyPressed(KEY_A)) { te.select_all(); return false; }
    if (ctrl && IsKeyPressed(KEY_C)) {
        copy_text(te.has_sel() ? te.selected().c_str() : te.text.c_str());
        return false;
    }
    if (ctrl && IsKeyPressed(KEY_V)) {
        const char* clip = get_paste_text();
        if (clip) {
            te.delete_sel();
            for (char c : std::string(clip)) {
                if ((int)te.text.size() >= max_len)
                    break;
                if (uppercase && c >= 'a' && c <= 'z')
                    c -= 32;
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                    te.insert_char(c);
                    changed = true;
                }
            }
        }
        return changed;
    }

    auto nav = [&](int p, bool ext) { te.move_to(p, ext); };
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT))
        nav(te.has_sel() && !shift ? te.sel_lo() : te.cursor - 1, shift);
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT))
        nav(te.has_sel() && !shift ? te.sel_hi() : te.cursor + 1, shift);
    if (IsKeyPressed(KEY_HOME))
        nav(0, shift);
    if (IsKeyPressed(KEY_END))
        nav((int)te.text.size(), shift);

    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (te.has_sel()) { te.delete_sel(); changed = true; }
        else if (te.cursor > 0) { te.text.erase(--te.cursor, 1); te.anchor = te.cursor; changed = true; }
    }
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) {
        if (te.has_sel()) { te.delete_sel(); changed = true; }
        else if (te.cursor < (int)te.text.size()) { te.text.erase(te.cursor, 1); changed = true; }
    }

    int key = GetCharPressed();
    while (key > 0) {
        if ((int)te.text.size() < max_len) {
            if (uppercase && key >= 'a' && key <= 'z')
                key -= 32;
            if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
                te.insert_char((char)key); changed = true;
            }
        }
        key = GetCharPressed();
    }
    return changed;
}

// TextSel
// Read-only selectable text; for displaying the lobby code.
struct TextSel {
    int anchor = 0;
    int cursor = 0;
    bool mouse_held = false;

    bool has_sel() const { return cursor != anchor; }
    int sel_lo() const { return std::min(cursor, anchor); }
    int sel_hi() const { return std::max(cursor, anchor); }
};

static void draw_text_sel(TextSel& sel, const std::string& text, Vector2 pos,
                           const Font& font, float size, float spacing, Color color) {
    if (sel.has_sel()) {
        float x0 = pos.x + MeasureTextEx(font, text.substr(0, sel.sel_lo()).c_str(), size, spacing).x;
        float x1 = pos.x + MeasureTextEx(font, text.substr(0, sel.sel_hi()).c_str(), size, spacing).x;
        DrawRectangle((int)x0, (int)pos.y - 2, (int)(x1 - x0), (int)size + 4, Color{70, 120, 200, 180});
    }
    DrawTextEx(font, text.c_str(), pos, size, spacing, color);
}

static void update_text_sel(TextSel& sel, const std::string& text, Vector2 pos,
                              const Font& font, float size, float spacing) {
    if (text.empty())
        return;
    Vector2 tsz = MeasureTextEx(font, text.c_str(), size, spacing);
    Rectangle box = { pos.x, pos.y, tsz.x, size };
    Vector2 mp = GetMousePosition();
    bool in_box = CheckCollisionPointRec(mp, box);

    auto get_idx = [&](float x_off) -> int {
        return chars_at_x_ex(font, size, spacing, text, x_off);
    };

    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && in_box) {
        sel.mouse_held = true;
        int p = get_idx(mp.x - pos.x);
        sel.anchor = sel.cursor = p;
    }
    if (sel.mouse_held && IsMouseButtonDown(MOUSE_LEFT_BUTTON))
        sel.cursor = get_idx(std::max(mp.x - pos.x, 0.f));
    if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) sel.mouse_held = false;

    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_A)) { sel.anchor = 0; sel.cursor = (int)text.size(); }
    if (ctrl && IsKeyPressed(KEY_C) && sel.has_sel())
        copy_text(text.substr(sel.sel_lo(), sel.sel_hi() - sel.sel_lo()).c_str());
}

// App types
struct Snapshot {
    pong::QuantState state;
    double recv_time_ms;
};
struct RenderState {
    float ball_x = 0.f, ball_y = 0.f;
    float paddle_a_y = 0.f, paddle_b_y = 0.f;
    uint8_t score_a = 0, score_b = 0;
};

// App state
struct App {
    pong::Role role = pong::Role::None;
    std::unique_ptr<pong::Transport> transport;
    pong::SimState sim;
    pong::QuantState last_tx_state;
    pong::QuantState last_rx_state;

    std::string lobby_code;
    bool peer_connected = false;
    bool connecting = false;
    bool game_over = false;
    bool show_menu = false;
    bool host_closed = false;
    int winner = 0;

    // Persistent text inputs
    TextEdit username_edit;
    TextEdit signaling_edit;
    TextEdit join_code_edit;
    TextSel  lobby_code_sel;
    std::string opponent_username;

    Font code_font;

    std::array<pong::DecodedInput, 64> input_buf{};
    uint32_t ping_seq = 0;
    double last_ping_sent_ms = -1000.0;
    float rtt_ms = 60.f;
    int8_t last_guest_dir = 0;

    std::deque<Snapshot> snap_buf;
    std::mutex snap_mutex;

    uint32_t local_tick = 0;
    int8_t input_history[64] = {};

    double accumulator_ms = 0.0;
    double last_frame_ms = 0.0;

    float  render_paddle_b_y = static_cast<float>(pong::SimState{}.paddle_b_y) / 100.f;
    double last_guest_input_ms = 0.0;
    bool guest_ever_sent_input = false;

    RenderTexture2D render_target = {};
};

static App g_app;

static void reset_app() {
    if (g_app.transport) { g_app.transport->close(); g_app.transport.reset(); }
    g_app.role = pong::Role::None;
    g_app.sim = pong::SimState{};
    g_app.last_tx_state = pong::QuantState{};
    g_app.last_rx_state = pong::QuantState{};
    g_app.lobby_code.clear();
    g_app.peer_connected = false;
    g_app.connecting = false;
    g_app.game_over = false;
    g_app.show_menu = false;
    g_app.host_closed = false;
    g_app.winner = 0;
    g_app.opponent_username.clear();
    g_app.join_code_edit = TextEdit{};
    g_app.lobby_code_sel = TextSel{};
    g_app.input_buf = {};

    g_app.ping_seq = 0;
    g_app.last_ping_sent_ms = -1000.0;
    g_app.rtt_ms = 60.f;
    g_app.last_guest_dir = 0;
    { std::lock_guard<std::mutex> lk(g_app.snap_mutex); g_app.snap_buf.clear(); }
    g_app.local_tick = 0;
    std::fill(std::begin(g_app.input_history), std::end(g_app.input_history), int8_t{0});
    g_app.accumulator_ms = 0.0;
    g_app.last_frame_ms = 0.0;
    g_app.render_paddle_b_y = static_cast<float>(pong::SimState{}.paddle_b_y) / 100.f;
    g_app.last_guest_input_ms = 0.0;
    g_app.guest_ever_sent_input = false;
    // username_edit and signaling_edit are intentionally not reset
    g_app.username_edit.focused = false;
    g_app.signaling_edit.focused = false;
}

// Username exchange
static void send_username() {
    if (!g_app.transport) return;
    uint8_t buf[pong::USERNAME_MAX_BYTES];
    int len = pong::encode_username(buf, g_app.username_edit.text);
    g_app.transport->send({ buf, static_cast<size_t>(len) });
}

// Role setup
static void start_as_host() {
    g_app.role = pong::Role::Host;
    g_app.transport = pong::make_transport();

    g_app.transport->on_lobby_code = [](const std::string& code) {
        g_app.lobby_code = code;
        g_app.lobby_code_sel = TextSel{};
        TraceLog(LOG_INFO, "[host] lobby code: %s", code.c_str());
    };
    g_app.transport->on_open = []() {
        g_app.peer_connected = true;
        TraceLog(LOG_INFO, "[host] guest connected");
        send_username();
    };
    g_app.transport->on_message = [](std::span<const uint8_t> buf) {
        if (buf.empty()) return;
        auto type = pong::peek_type(buf);
        if (type == pong::MsgType::Input) {
            pong::DecodedInput in;
            if (pong::decode_input(buf, in)) {
                g_app.input_buf[in.tick % 64] = in;
                g_app.last_guest_dir = in.dir;
                g_app.last_guest_input_ms = now_ms();
                g_app.guest_ever_sent_input = true;
            }
        } else if (type == pong::MsgType::Pong) {
            uint32_t seq, client_ts;
            if (pong::decode_pong(buf, seq, client_ts) && seq == g_app.ping_seq - 1) {
                uint32_t now32 = static_cast<uint32_t>(static_cast<uint64_t>(now_ms()) & 0xFFFFFFFF);
                double rtt = static_cast<double>((now32 - client_ts) & 0xFFFFFFFF);
                g_app.rtt_ms = RTT_EWMA_ALPHA * static_cast<float>(rtt)
                             + (1.f - RTT_EWMA_ALPHA) * g_app.rtt_ms;
            }
        } else if (type == pong::MsgType::Username) {
            std::string name;
            if (pong::decode_username(buf, name))
                g_app.opponent_username = name;
        }
    };
    g_app.transport->on_close = []() { g_app.peer_connected = false; };
    g_app.transport->host(g_app.signaling_edit.text);
}

static void start_as_guest(const std::string& code) {
    g_app.role = pong::Role::Guest;
    g_app.transport = pong::make_transport();

    g_app.transport->on_open = []() {
        g_app.peer_connected = true;
        TraceLog(LOG_INFO, "[guest] connected to host");
        send_username();
    };
    g_app.transport->on_message = [](std::span<const uint8_t> buf) {
        if (buf.empty())
            return;
        auto type = pong::peek_type(buf);

        if (type == pong::MsgType::GameState) {
            if (g_app.game_over && buf.size() >= 2) {
                const uint8_t* pp  = buf.data() + 1;
                const uint8_t* end = buf.data() + buf.size();
                if (pong::vlq_read(pp, end) < g_app.sim.tick) {
                    g_app.game_over = false;
                    g_app.winner = 0;
                    g_app.last_rx_state = pong::QuantState{};
                    g_app.local_tick = 0;
                    g_app.sim.paddle_b_y = pong::SimState{}.paddle_b_y;
                    std::fill(std::begin(g_app.input_history), std::end(g_app.input_history), int8_t{0});
                    std::lock_guard<std::mutex> lk(g_app.snap_mutex);
                    g_app.snap_buf.clear();
                }
            }
            pong::SimState tmp{};
            if (pong::decode_game_state(buf, tmp, g_app.last_rx_state)) {
                Snapshot snap{ g_app.last_rx_state, now_ms() };
                {
                    std::lock_guard<std::mutex> lk(g_app.snap_mutex);
                    if (g_app.snap_buf.empty() || snap.state.tick > g_app.snap_buf.back().state.tick) {
                        g_app.snap_buf.push_back(snap);
                        while (g_app.snap_buf.size() > SNAPSHOT_BUF_MAX)
                            g_app.snap_buf.pop_front();
                    }
                }
                g_app.sim.score_a = tmp.score_a;
                g_app.sim.score_b = tmp.score_b;
                g_app.sim.tick = tmp.tick;
                {
                    int32_t pos = tmp.paddle_b_y;
                    uint32_t from = tmp.tick + 1;
                    uint32_t to = g_app.local_tick;
                    uint32_t max_replay = 64;
                    if (to > from && to - from > max_replay) from = to - max_replay;
                    for (uint32_t t = from; t < to; ++t) {
                        pos += g_app.input_history[t % 64] * pong::PADDLE_SPD;
                        pos = std::clamp(pos, 0, pong::FIELD_H - pong::PADDLE_H);
                    }
                    g_app.sim.paddle_b_y = pos;
                }
                if (g_app.sim.score_a >= pong::WIN_SCORE) { g_app.game_over = true; g_app.winner = 1; }
                else if (g_app.sim.score_b >= pong::WIN_SCORE) { g_app.game_over = true; g_app.winner = 2; }
            }
        } else if (type == pong::MsgType::Ping) {
            uint32_t seq, client_ts;
            if (pong::decode_ping(buf, seq, client_ts)) {
                uint8_t pbuf[pong::PONG_BYTES];
                int len = pong::encode_pong(pbuf, seq, client_ts);
                g_app.transport->send({ pbuf, static_cast<size_t>(len) });
            }
        } else if (type == pong::MsgType::Username) {
            std::string name;
            if (pong::decode_username(buf, name))
                g_app.opponent_username = name;
        }
    };
    g_app.transport->on_close = []() {
        if (!g_app.peer_connected) {
            g_app.connecting = false;
            g_app.join_code_edit = TextEdit{};
            g_app.role = pong::Role::Guest;
        } else {
            g_app.host_closed = true; // handled next frame then reset_app()
        }
        g_app.peer_connected = false;
    };
    g_app.transport->join(g_app.signaling_edit.text, code);
}

// Game tick (host)
static void game_tick() {
    if (g_app.game_over)
        return;

    int8_t dir_a = 0;
    if (!g_app.show_menu) {
        if (IsKeyDown(KEY_UP)   || IsKeyDown(KEY_W))
            dir_a = -1;
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
            dir_a =  1;
    }
    const auto& gi = g_app.input_buf[g_app.sim.tick % 64];
    int8_t dir_b = (gi.tick == g_app.sim.tick) ? gi.dir : g_app.last_guest_dir;
    if (gi.tick == g_app.sim.tick)
        g_app.last_guest_dir = gi.dir;

    pong::sim_tick(g_app.sim, dir_a, dir_b);
    if (g_app.sim.score_a >= pong::WIN_SCORE) { g_app.game_over = true; g_app.winner = 1; }
    else if (g_app.sim.score_b >= pong::WIN_SCORE) { g_app.game_over = true; g_app.winner = 2; }

    pong::QuantState cur_q = pong::sim_quantize(g_app.sim);
    uint8_t dmask = pong::compute_delta_mask(g_app.last_tx_state, cur_q);
    uint8_t sbuf[pong::GAMESTATE_MAX_BYTES];
    int slen = pong::encode_game_state(sbuf, g_app.sim, g_app.last_tx_state, dmask);
    g_app.transport->send({ sbuf, static_cast<size_t>(slen) });
    g_app.last_tx_state = cur_q;

    double t = now_ms();
    if (t - g_app.last_ping_sent_ms >= PING_INTERVAL_MS) {
        g_app.last_ping_sent_ms = t;
        uint32_t ts32 = static_cast<uint32_t>(static_cast<uint64_t>(t) & 0xFFFFFFFF);
        uint8_t  pbuf[pong::PING_BYTES];
        g_app.transport->send({ pbuf, static_cast<size_t>(pong::encode_ping(pbuf, g_app.ping_seq++, ts32)) });
    }
}

// Generic button
static bool draw_button(const char* text, int x, int y, int w, int h) {
    Rectangle rect = { (float)x, (float)y, (float)w, (float)h };
    bool hover = CheckCollisionPointRec(GetMousePosition(), rect);
    DrawRectangleRec(rect, hover ? DARKGRAY : GRAY);
    DrawRectangleLinesEx(rect, 2, hover ? WHITE : LIGHTGRAY);
    int tw = MeasureText(text, 20);
    DrawText(text, x + w / 2 - tw / 2, y + h / 2 - 10, 20, WHITE);
    return hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

// Render-state computation
static RenderState compute_render_state() {
    RenderState rs;
    rs.score_a = g_app.sim.score_a;
    rs.score_b = g_app.sim.score_b;

    if (g_app.role == pong::Role::Guest) {
        const uint32_t latest_tick = g_app.sim.tick;
        const uint32_t render_tick = (latest_tick >= INTERP_DELAY_TICKS)
                                   ? latest_tick - INTERP_DELAY_TICKS : 0;
        std::lock_guard<std::mutex> lk(g_app.snap_mutex);
        const auto& snaps = g_app.snap_buf;
        if (snaps.empty())
            return rs;

        rs.paddle_b_y = g_app.render_paddle_b_y;

        if (snaps.size() == 1 || render_tick <= snaps.front().state.tick) {
            rs.ball_x = (float)snaps.front().state.ball_x;
            rs.ball_y = (float)snaps.front().state.ball_y;
            rs.paddle_a_y = (float)snaps.front().state.paddle_a_y;
            return rs;
        }

        const Snapshot *prev = nullptr, *next = nullptr;
        for (int i = (int)snaps.size() - 2; i >= 0; --i) {
            if (snaps[i].state.tick <= render_tick) { prev = &snaps[i]; next = &snaps[i+1]; break; }
        }
        auto lerpf = [](float a, float b, float t){ return a + (b - a) * t; };
        if (prev && next && next->state.tick >= render_tick) {
            uint32_t span = next->state.tick - prev->state.tick;
            float t = span > 0 ? std::clamp(
                (float)(render_tick - prev->state.tick) / (float)span, 0.f, 1.f) : 0.f;
            rs.ball_x = lerpf((float)prev->state.ball_x, (float)next->state.ball_x, t);
            rs.ball_y = lerpf((float)prev->state.ball_y, (float)next->state.ball_y, t);
            rs.paddle_a_y = lerpf((float)prev->state.paddle_a_y, (float)next->state.paddle_a_y, t);
        } else {
            const Snapshot& lat = snaps.back();
            uint32_t dt = render_tick - lat.state.tick;
            static constexpr float MY = (float)((pong::FIELD_H - pong::BALL_SIZE) / 100);
            static constexpr float MX = (float)((pong::FIELD_W - pong::BALL_SIZE) / 100);
            float bx = (float)lat.state.ball_x, by = (float)lat.state.ball_y;
            float vx = (float)lat.state.ball_vx, vy = (float)lat.state.ball_vy;
            for (uint32_t i = 0; i < dt; ++i) {
                bx += vx; by += vy;
                if (by <= 0.f) { by = 0.f; vy = -vy; }
                if (by >= MY) { by = MY; vy = -vy; }
                if (bx < 0.f) bx = 0.f;
                if (bx > MX) bx = MX;
            }
            rs.ball_x = bx; rs.ball_y = by;
            rs.paddle_a_y = (float)lat.state.paddle_a_y;
        }
        return rs;
    }
    rs.ball_x = (float)g_app.sim.ball_x / 100.f;
    rs.ball_y = (float)g_app.sim.ball_y / 100.f;
    rs.paddle_a_y = (float)g_app.sim.paddle_a_y / 100.f;
    rs.paddle_b_y = (float)g_app.sim.paddle_b_y / 100.f;
    return rs;
}

// Draw
static constexpr int BT = 3; // border thickness

static void draw_game(const RenderState& rs) {
    ClearBackground(BLACK);

    // Outer frame
    DrawRectangle(0, 0, SCREEN_W, BT, WHITE);
    DrawRectangle(0, SCREEN_H - BT, SCREEN_W, BT, WHITE);
    DrawRectangle(0, 0, BT, SCREEN_H, WHITE);
    DrawRectangle(SCREEN_W - BT, 0, BT, SCREEN_H, WHITE);
    // HUD / game divider
    DrawRectangle(0, HUD_H - BT, SCREEN_W, BT, WHITE);

    // HUD content
    const char* name_a = (g_app.role == pong::Role::Host)
        ? g_app.username_edit.text.c_str() : g_app.opponent_username.c_str();
    const char* name_b = (g_app.role == pong::Role::Guest)
        ? g_app.username_edit.text.c_str() : g_app.opponent_username.c_str();

    static constexpr int SF = 36, NF = 16;
    int mid = SCREEN_W / 2, vy = HUD_H / 2;
    DrawRectangle(mid - 1, BT, 2, HUD_H - BT * 2, GRAY);

    const char* sa = TextFormat("%d", rs.score_a);
    DrawText(sa, mid - 18 - MeasureText(sa, SF), vy - SF / 2, SF, WHITE);
    DrawText(TextFormat("%d", rs.score_b), mid + 18, vy - SF / 2, SF, WHITE);
    DrawText(name_a, BT + 8, vy - NF / 2, NF, LIGHTGRAY);
    DrawText(name_b, SCREEN_W - BT - 8 - MeasureText(name_b, NF), vy - NF / 2, NF, LIGHTGRAY);

    // Game area
    for (int y = HUD_H; y < SCREEN_H - BT; y += 30)
        DrawRectangle(mid - 2, y, 4, 18, GRAY);

    DrawRectangle((int)rs.ball_x, (int)rs.ball_y + HUD_H, 10, 10, WHITE);
    int ph = pong::PADDLE_H / 100, pw = pong::PADDLE_W / 100;
    DrawRectangle(BT, (int)rs.paddle_a_y + HUD_H, pw, ph, WHITE);
    DrawRectangle(SCREEN_W - pw - BT, (int)rs.paddle_b_y + HUD_H, pw, ph, WHITE);

    // Overlays
    int gt = HUD_H;
    int gmy = gt + (SCREEN_H - gt) / 2;

    if (g_app.game_over) {
        DrawRectangle(0, gt, SCREEN_W, SCREEN_H - gt, Color{0, 0, 0, 160});
        const char* msg = (g_app.winner == 1) ? "HOST WINS!" : "GUEST WINS!";
        DrawText(msg, SCREEN_W / 2 - MeasureText(msg, 60) / 2, gmy - 100, 60, YELLOW);
        if (g_app.role == pong::Role::Host) {
            const char* sub = "Press SPACE to play again";
            DrawText(sub, SCREEN_W / 2 - MeasureText(sub, 24) / 2, gmy - 20, 24, LIGHTGRAY);
        }
        if (draw_button("Main Menu", SCREEN_W / 2 - 100, gmy + 40, 200, 40))
            reset_app();
    } else if (g_app.show_menu) {
        DrawRectangle(0, gt, SCREEN_W, SCREEN_H - gt, Color{0, 0, 0, 160});
        const char* msg = "PAUSED";
        DrawText(msg, SCREEN_W / 2 - MeasureText(msg, 40) / 2, gmy - 100, 40, WHITE);
        if (draw_button("Resume", SCREEN_W / 2 - 100, gmy - 20, 200, 40))
            g_app.show_menu = false;
        if (draw_button("Main Menu", SCREEN_W / 2 - 100, gmy + 40, 200, 40))
            reset_app();
    }
}

// Box rects for main-menu text inputs; used in both draw and update.
static constexpr Rectangle USERNAME_BOX  = { SCREEN_W - 252.f,  38.f, 242.f, 28.f };
static constexpr Rectangle SIGNALING_BOX = { SCREEN_W - 252.f,  98.f, 242.f, 28.f };

static void draw_lobby() {
    ClearBackground(BLACK);

    if (g_app.role == pong::Role::None) {
        DrawText("ONLINE PONG", SCREEN_W / 2 - 120, 180, 40, GREEN);
        if (draw_button("Host a Game", SCREEN_W / 2 - 100, 270, 200, 40)) start_as_host();
        if (draw_button("Join a Game", SCREEN_W / 2 - 100, 330, 200, 40))
            g_app.role = pong::Role::Guest;

        // Username
        DrawText("Username:", (int)USERNAME_BOX.x, 16, 18, GRAY);
        draw_text_edit(g_app.username_edit, USERNAME_BOX, 16);

        // Signaling server
        DrawText("Server:", (int)SIGNALING_BOX.x, 76, 18, GRAY);
        draw_text_edit(g_app.signaling_edit, SIGNALING_BOX, 14);

    } else {
        if (draw_button("< Back", 20, 20, 100, 40)) { reset_app(); return; }

        if (g_app.role == pong::Role::Host) {
            DrawText("Waiting for guest...", 240, 200, 28, WHITE);
            if (!g_app.lobby_code.empty()) {
                DrawText("Lobby code:", 280, 270, 24, GRAY);
                DrawText("(click & drag to select, Ctrl+C to copy)", 195, 360, 16, DARKGRAY);
                draw_text_sel(g_app.lobby_code_sel, g_app.lobby_code,
                              {280, 306}, g_app.code_font, 40, 2, GREEN);
            }
        } else if (g_app.role == pong::Role::Guest) {
            if (g_app.connecting) {
                DrawText("Connecting...", 270, 260, 32, YELLOW);
            } else {
                DrawText("Enter lobby code:", 220, 200, 28, WHITE);
                draw_code_edit(g_app.join_code_edit, {280, 260}, 40, 2, GREEN);
                DrawText("Press ENTER to join", 240, 330, 20, GRAY);
            }
        }
    }
}

// Main loop
static void main_loop() {
    static constexpr double TICK_MS = 1000.0 / TICK_HZ;
    static constexpr double MAX_DELTA_MS = 200.0;
    static constexpr int MAX_TICKS_FRAME = 4;

#ifdef PONG_WASM
    // Keep the canvas sized to the browser viewport every frame.
    {
        int vw = js_viewport_w(), vh = js_viewport_h();
        if (vw != GetScreenWidth() || vh != GetScreenHeight())
            SetWindowSize(vw, vh);
    }
#endif

    // Handle async host-disconnect flag.
    if (g_app.host_closed) { reset_app(); return; }

    double now = now_ms();
    if (g_app.last_frame_ms == 0.0)
        g_app.last_frame_ms = now;
    double delta = std::min(now - g_app.last_frame_ms, MAX_DELTA_MS);
    g_app.last_frame_ms = now;

#ifndef PONG_WASM
    if (IsKeyPressed(KEY_F11)) {
        if (!IsWindowFullscreen()) {
            int mon = GetCurrentMonitor();
            SetWindowSize(GetMonitorWidth(mon), GetMonitorHeight(mon));
            ToggleFullscreen();
        } else { ToggleFullscreen(); SetWindowSize(SCREEN_W, SCREEN_H); }
    }
#endif

    // Text input updates
    if (g_app.role == pong::Role::None) {
        // Main menu; username and server fields.
        if (update_text_edit(g_app.username_edit, USERNAME_BOX, 16, pong::USERNAME_MAX_LEN))
            storage_set("username", g_app.username_edit.text.c_str());
        if (update_text_edit(g_app.signaling_edit, SIGNALING_BOX, 14, 127))
            storage_set("signaling_url", g_app.signaling_edit.text.c_str());
        if (IsKeyPressed(KEY_ESCAPE)) {
            g_app.username_edit.focused  = false;
            g_app.signaling_edit.focused = false;
        }
    } else if (!g_app.peer_connected) {
        // Lobby screens.
        if (g_app.role == pong::Role::Guest && !g_app.connecting) {
            update_code_edit(g_app.join_code_edit, {280, 260}, 40, 2, 6, /*uppercase=*/true);
        } else if (g_app.role == pong::Role::Host && !g_app.lobby_code.empty()) {
            update_text_sel(g_app.lobby_code_sel, g_app.lobby_code,
                            {280, 306}, g_app.code_font, 40, 2);
        }
    }

    // Lobby; not yet connected
    if (g_app.role != pong::Role::None && !g_app.peer_connected) {
        if (IsKeyPressed(KEY_ESCAPE)) { reset_app(); return; }
        if (g_app.role == pong::Role::Guest && !g_app.connecting) {
            if (IsKeyPressed(KEY_ENTER) && (int)g_app.join_code_edit.text.size() == 6) {
                g_app.connecting = true;
                start_as_guest(g_app.join_code_edit.text);
            }
        }

    // In-game
    } else if (g_app.peer_connected) {
        if (g_app.game_over) {
            g_app.accumulator_ms = 0.0;
            if (IsKeyPressed(KEY_SPACE) && g_app.role == pong::Role::Host) {
                g_app.sim = pong::SimState{};
                g_app.last_tx_state = pong::QuantState{};
                g_app.last_guest_dir = 0;
                g_app.input_buf = {};
                g_app.game_over = false;
                g_app.winner = 0;
                // Reset the disconnect-timer then the new game gets a fresh 3-second window.
                g_app.last_guest_input_ms = now_ms();
                g_app.guest_ever_sent_input = false;
            }
        } else {
            if (g_app.role == pong::Role::Host &&
                g_app.guest_ever_sent_input &&
                (now - g_app.last_guest_input_ms) > 3000.0) {
                TraceLog(LOG_INFO, "[host] guest timed out");
                reset_app(); return;
            }
            if (IsKeyPressed(KEY_ESCAPE))
                g_app.show_menu = !g_app.show_menu;

            int8_t guest_dir = 0;
            if (g_app.role == pong::Role::Guest && !g_app.show_menu) {
                if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))
                    guest_dir = -1;
                if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
                    guest_dir =  1;
            }

            g_app.accumulator_ms += delta;
            int ticks_run = 0;
            while (g_app.accumulator_ms >= TICK_MS && ticks_run < MAX_TICKS_FRAME) {
                if (g_app.role == pong::Role::Host) {
                    game_tick();
                } else {
                    g_app.input_history[g_app.local_tick % 64] = guest_dir;
                    g_app.sim.paddle_b_y = std::clamp(
                        g_app.sim.paddle_b_y + guest_dir * pong::PADDLE_SPD,
                        0, pong::FIELD_H - pong::PADDLE_H);

                    uint8_t ibuf[pong::INPUT_MAX_BYTES];
                    int ilen = pong::encode_input(ibuf, g_app.local_tick, guest_dir, 0);
                    g_app.transport->send({ ibuf, static_cast<size_t>(ilen) });
                    ++g_app.local_tick;

                    float target = (float)g_app.sim.paddle_b_y / 100.f;
                    g_app.render_paddle_b_y += (target - g_app.render_paddle_b_y) * 0.8f;
                }
                g_app.accumulator_ms -= TICK_MS;
                ++ticks_run;
            }
        }
    }

    // Render
    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    float scale = std::min(sw / SCREEN_W, sh / SCREEN_H);
    float ox = (sw - SCREEN_W * scale) / 2.f;
    float oy = (sh - SCREEN_H * scale) / 2.f;
    SetMouseOffset((int)-ox, (int)-oy);
    SetMouseScale(1.f / scale, 1.f / scale);

    BeginTextureMode(g_app.render_target);
    if (g_app.peer_connected)
        draw_game(compute_render_state());
    else
        draw_lobby();
    EndTextureMode();

    BeginDrawing();
    ClearBackground(BLACK);
    DrawTexturePro(
        g_app.render_target.texture,
        { 0.f, 0.f, (float)SCREEN_W, -(float)SCREEN_H },
        { ox, oy, SCREEN_W * scale, SCREEN_H * scale },
        { 0.f, 0.f }, 0.f, WHITE);
    EndDrawing();
}

// Entry point
int main() {
    // Load persisted settings before opening the window.
    std::string saved_username = storage_get("username", "");
    std::string saved_url = storage_get("signaling_url", "ws://localhost:9000");

#ifndef PONG_WASM
    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "Online Pong");
#else
    // Start the WASM canvas at the current browser viewport size.
    InitWindow(js_viewport_w(), js_viewport_h(), "Online Pong");
    js_setup_paste_listener();
#endif
    SetExitKey(0);

    g_app.code_font = LoadFontEx("Assets/Roboto-Regular.ttf", 40, 0, 250);
    g_code_font_ptr = &g_app.code_font;
    g_app.render_target = LoadRenderTexture(SCREEN_W, SCREEN_H);
    SetTextureFilter(g_app.render_target.texture, TEXTURE_FILTER_BILINEAR);

    g_app.username_edit.text = saved_username;
    g_app.signaling_edit.text = saved_url;

#ifdef PONG_WASM
    emscripten_set_main_loop(main_loop, 0, 1);
#else
    while (!WindowShouldClose())
        main_loop();
    UnloadRenderTexture(g_app.render_target);
    UnloadFont(g_app.code_font);
    CloseWindow();
#endif
    return 0;
}
