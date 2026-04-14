#include "render.h"
#include "net.h"
#include "ui.h"
#include "config.h"
#include "pong/sim.h"
#include <algorithm>
#include <mutex>

static constexpr int BT = 3;  // border thickness

RenderState compute_render_state() {
    RenderState rs;
    rs.score_a = g_app.sim.score_a;
    rs.score_b = g_app.sim.score_b;
    rs.ball_x = (float)g_app.sim.ball_x / 100.f;
    rs.ball_y = (float)g_app.sim.ball_y / 100.f;

    // Lerp the remote paddle towards the latest absolute target
    g_app.render_remote_paddle_y += (g_app.target_remote_paddle_y - g_app.render_remote_paddle_y) * 0.4f;

    if (g_app.role == pong::Role::Host) {
        rs.paddle_a_y = (float)g_app.sim.paddle_a_y / 100.f;
        rs.paddle_b_y = g_app.render_remote_paddle_y;
    } else {
        rs.paddle_b_y = (float)g_app.sim.paddle_b_y / 100.f;
        rs.paddle_a_y = g_app.render_remote_paddle_y;
    }

    rs.is_split = g_app.sim.has_schrodinger;
    if (rs.is_split) {
        rs.ghost_ball_x = (float)g_app.sim.s_ball_x / 100.f;
        rs.ghost_ball_y = (float)g_app.sim.s_ball_y / 100.f;
    }
    return rs;
}

void draw_game(const RenderState& rs) {
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

    // Yellow ghost = Schrödinger ball (the disputed "scored" timeline).
    // Always draw the real ball; it bounces optimistically regardless.
    if (rs.is_split) {
        DrawRectangle((int)rs.ghost_ball_x, (int)rs.ghost_ball_y + HUD_H, 10, 10, YELLOW);
    }
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

void draw_lobby() {
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