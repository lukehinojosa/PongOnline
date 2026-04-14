#include "app.h"
#include "config.h"
#include "net.h"
#include "render.h"
#include "storage.h"
#include "ui.h"

#include "pong/codec.h"
#include "pong/messages.h"

#include "raylib.h"

#include <algorithm>

#ifdef PONG_WASM
#  include <emscripten/emscripten.h>
// These are defined by EM_JS in storage.cpp; forward-declared here for main use.
extern "C" {
    int js_viewport_w();
    int js_viewport_h();
    void js_setup_paste_listener();
}
#endif

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
            g_app.username_edit.focused = false;
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
                // Reset the disconnect-timer so the new game gets a fresh 3-second window.
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
                    guest_dir = 1;
            }

            // PLL tick-pacing (guest only): keep local_tick running at ~RTT/TICK_MS ticks
            // ahead of the last known host tick so inputs arrive on time.
            // The host always uses multiplier 1.0 (set to default in App).
            if (g_app.role == pong::Role::Guest) {
                uint32_t rtt_ticks = static_cast<uint32_t>(g_app.rtt_ms / TICK_MS);
                uint32_t target_tick = g_app.sim.tick + rtt_ticks + 1;
                int tick_diff = static_cast<int>(target_tick) - static_cast<int>(g_app.local_tick);

                if (tick_diff > 1) {
                    g_app.clock_drift_multiplier = 1.1f;  // behind target — speed up 10 %
                } else if (tick_diff < -1) {
                    g_app.clock_drift_multiplier = 0.9f;  // ahead of target — slow down 10 %
                } else {
                    g_app.clock_drift_multiplier = 1.0f;  // within ±1 tick deadband — coast
                }

                // Guest is the heartbeat source; send a ping on the configured interval.
                if (now - g_app.last_ping_sent_ms >= PING_INTERVAL_MS) {
                    g_app.last_ping_sent_ms = now;
                    uint32_t ts32 = static_cast<uint32_t>(static_cast<uint64_t>(now) & 0xFFFFFFFF);
                    uint8_t pbuf[pong::PING_BYTES];
                    g_app.transport->send({ pbuf, static_cast<size_t>(pong::encode_ping(pbuf, g_app.ping_seq++, ts32)) });
                }
            }

            g_app.accumulator_ms += delta * g_app.clock_drift_multiplier;

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