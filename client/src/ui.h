#pragma once
#include "raylib.h"
#include <algorithm>
#include <string>

// Set before calling any draw_code_edit / update_code_edit.
extern Font* g_code_font_ptr;

// TextEdit
// A self-contained editable text field with cursor, selection, and scroll.
struct TextEdit {
    std::string text;
    int cursor = 0;  // insertion point [0, text.size()]
    int anchor = 0;  // other end of selection; cursor==anchor means no selection
    int scroll = 0;  // first visible character index
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

// Default-font text edit box.
void draw_text_edit(TextEdit& te, Rectangle box, int font_size);
// Returns true if text was modified.
bool update_text_edit(TextEdit& te, Rectangle box, int font_size, int max_len,
                      bool uppercase = false);

// Large code-font text edit (no scroll; used for the join-code field).
void draw_code_edit(TextEdit& te, Vector2 pos, float size, float spacing, Color color);
// Returns true if text was modified.
bool update_code_edit(TextEdit& te, Vector2 pos, float size, float spacing,
                      int max_len, bool uppercase = false);

// Read-only selectable text rendered with a custom font.
void draw_text_sel(TextSel& sel, const std::string& text, Vector2 pos,
                   const Font& font, float size, float spacing, Color color);
void update_text_sel(TextSel& sel, const std::string& text, Vector2 pos,
                     const Font& font, float size, float spacing);

// Generic button; returns true on click.
bool draw_button(const char* text, int x, int y, int w, int h);
