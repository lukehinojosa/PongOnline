#include "ui.h"
#include "storage.h"
#include <algorithm>

Font* g_code_font_ptr = nullptr;

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
        float w0 = MeasureTextEx(font, text.substr(0, i).c_str(), size, spacing).x;
        float w1 = MeasureTextEx(font, text.substr(0, i + 1).c_str(), size, spacing).x;
        if (x_off * 2.f <= w0 + w1)
            return i;
    }
    return (int)text.size();
}

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

void draw_text_edit(TextEdit& te, Rectangle box, int font_size) {
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

bool update_text_edit(TextEdit& te, Rectangle box, int font_size, int max_len,
                      bool uppercase) {
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

void draw_code_edit(TextEdit& te, Vector2 pos, float size, float spacing, Color color) {
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

bool update_code_edit(TextEdit& te, Vector2 pos, float size, float spacing,
                      int max_len, bool uppercase) {
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

void draw_text_sel(TextSel& sel, const std::string& text, Vector2 pos,
                   const Font& font, float size, float spacing, Color color) {
    if (sel.has_sel()) {
        float x0 = pos.x + MeasureTextEx(font, text.substr(0, sel.sel_lo()).c_str(), size, spacing).x;
        float x1 = pos.x + MeasureTextEx(font, text.substr(0, sel.sel_hi()).c_str(), size, spacing).x;
        DrawRectangle((int)x0, (int)pos.y - 2, (int)(x1 - x0), (int)size + 4, Color{70, 120, 200, 180});
    }
    DrawTextEx(font, text.c_str(), pos, size, spacing, color);
}

void update_text_sel(TextSel& sel, const std::string& text, Vector2 pos,
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

bool draw_button(const char* text, int x, int y, int w, int h) {
    Rectangle rect = { (float)x, (float)y, (float)w, (float)h };
    bool hover = CheckCollisionPointRec(GetMousePosition(), rect);
    DrawRectangleRec(rect, hover ? DARKGRAY : GRAY);
    DrawRectangleLinesEx(rect, 2, hover ? WHITE : LIGHTGRAY);
    int tw = MeasureText(text, 20);
    DrawText(text, x + w / 2 - tw / 2, y + h / 2 - 10, 20, WHITE);
    return hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}
