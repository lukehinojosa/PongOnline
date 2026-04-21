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
#include <cstdio>
#include <cstdarg>

#ifdef PONG_WASM
#  include <emscripten/emscripten.h>
// These are defined by EM_JS in storage.cpp; forward-declared here for main use.
extern "C" {
    int js_viewport_w();
    int js_viewport_h();
    void js_setup_paste_listener();
}
#endif

// Custom logger that filters out Raylib's internal INFO spam
void CustomLogCallback(int logLevel, const char *text, va_list args) {
    // Only print Warnings, Errors, OR custom app logs that start with '['
    if (logLevel >= LOG_WARNING || text[0] == '[') {
        vprintf(text, args);
        printf("\n");
    }
}

// Main loop
static void main_loop() {
    process_pending_server_list();

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

    static double last_signaling_ping_ms = 0.0;
    // The Signaling Heartbeat; Needed when connected to the lobby but waiting for a peer
    if (g_app.role == pong::Role::Host && !g_app.lobby_code.empty() && !g_app.peer_connected) {
        if (now - last_signaling_ping_ms > 2000.0) { // Send every 2 seconds
            last_signaling_ping_ms = now;

            if (g_app.transport) {
                // Send a ping
                g_app.transport->send_signaling_keepalive();
            }
        }
    }

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

        // Only update signaling edit if the dev menu is visible
        if (g_app.show_dev_menu) {
            if (update_text_edit(g_app.signaling_edit, SIGNALING_BOX, 14, 127))
                storage_set("signaling_url", g_app.signaling_edit.text.c_str());
        } else {
            g_app.signaling_edit.focused = false; // Unfocuses when hidden
        }
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
                // Generate a new seed for the rematch and reset the sim
                g_app.current_seed = GetRandomValue(0, 0x7fffffff);
                pong::reset_sim(g_app.sim, g_app.current_seed);
                g_app.game_over = false;
                g_app.winner = 0;

                // Reset sync states to trigger a fresh zero-snap handshake
                g_app.game_started = false;
                g_app.local_tick = 0;
                g_app.last_remote_paddle_ms = now;
                g_app.remote_ever_sent_paddle = false;

                // Broadcast the new seed to the guest before sending the tick-0 paddle
                uint8_t seed_buf[pong::SEED_BYTES];
                int slen = pong::encode_seed(seed_buf, g_app.current_seed);
                g_app.transport->send({ seed_buf, static_cast<size_t>(slen) });

                // Force-broadcast a tick-0 packet to notify the Guest of the restart
                uint8_t pbuf[pong::PADDLE_STATE_MAX_BYTES];
                int plen = pong::encode_paddle_state(pbuf, 0, g_app.sim.paddle_a_y);
                g_app.transport->send({ pbuf, static_cast<size_t>(plen) });
            }
        } else {
            // Symmetrical Timeout Logic
            if (g_app.remote_ever_sent_paddle && (now - g_app.last_remote_paddle_ms) > 3000.0) {
                TraceLog(LOG_INFO, "[net] peer timed out");
                reset_app(); return;
            }
            if (IsKeyPressed(KEY_ESCAPE))
                g_app.show_menu = !g_app.show_menu;

            int8_t guest_dir = 0;
            if (g_app.role == pong::Role::Guest && !g_app.show_menu) {
                if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W)) guest_dir = -1;
                if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S)) guest_dir = 1;
            }

            int current_max_ticks = MAX_TICKS_FRAME;

            // Zero-Snap Synchronized Start Handshake
            if (!g_app.game_started) {
                if (g_app.role == pong::Role::Host) {
                    // Host waits for Guest to send its trigger packet
                    if (g_app.remote_ever_sent_paddle) {
                        g_app.game_started = true;
                        g_app.last_remote_paddle_ms = now;
                        g_app.accumulator_ms = 0.0;
                    }
                } else if (g_app.role == pong::Role::Guest) {
                    // Guest waits for 5 pongs to establish an accurate RTT baseline
                    if (g_app.rtt_valid && !g_app.guest_ready) {
                        g_app.guest_ready = true;

                        // Schedule the start time to match exactly when the Host receives the trigger
                        g_app.guest_start_timer = now + (g_app.rtt_ms / 2.0);

                        // Fire the trigger packet to the Host right now
                        uint8_t pbuf[pong::PADDLE_STATE_MAX_BYTES];
                        int plen = pong::encode_paddle_state(pbuf, 0, g_app.sim.paddle_b_y);
                        g_app.transport->send({ pbuf, static_cast<size_t>(plen) });
                    }

                    // When the scheduled transit time finishes, unlock the game
                    if (g_app.guest_ready && now >= g_app.guest_start_timer) {
                        g_app.game_started = true;

                        // The Host is at tick 0 right now. Their first packet won't arrive until a half-ping from now.
                        g_app.last_remote_paddle_ms = now + (g_app.rtt_ms / 2.0);
                        g_app.accumulator_ms = 0.0;
                    }
                }

                // Keep the simulation clock frozen while waiting
                g_app.accumulator_ms = 0.0;
            }

            // Guest continuous pinging (runs even if waiting for sync)
            if (g_app.role == pong::Role::Guest) {
                double current_ping_interval = g_app.rtt_valid ? PING_INTERVAL_MS : 50.0;

                if (now - g_app.last_ping_sent_ms >= current_ping_interval) {
                    g_app.last_ping_sent_ms = now;
                    uint32_t ts32 = static_cast<uint32_t>(static_cast<uint64_t>(now) & 0xFFFFFFFF);
                    uint8_t pbuf[pong::PING_BYTES];
                    g_app.transport->send({ pbuf, static_cast<size_t>(pong::encode_ping(pbuf, g_app.ping_seq++, ts32)) });
                }
            }

            // PLL tick-pacing (guest only, strictly gated by game_started)
            if (g_app.role == pong::Role::Guest && g_app.game_started) {

                double one_way_ms = g_app.rtt_ms / 2.0;
                double elapsed_ms = now - g_app.last_remote_paddle_ms;
                double total_target_ms = (g_app.latest_remote_tick * TICK_MS) + one_way_ms + elapsed_ms;
                uint32_t target_tick = static_cast<uint32_t>(total_target_ms / TICK_MS);

                double deficit_ms = total_target_ms - (g_app.sim.tick * TICK_MS);
                int hard_deficit = static_cast<int>(deficit_ms / TICK_MS);

                // Check if in the 2-second warmup phase
                bool is_warmup = (g_app.sim.tick < TICK_HZ * 2);

                if (is_warmup) {
                    // Aggressive mode; Hidden behind the match start
                    if (hard_deficit > 0) {
                        // Instantly fast-forward to catch up
                        if (g_app.accumulator_ms < deficit_ms) {
                            g_app.accumulator_ms = deficit_ms;
                        }
                        current_max_ticks = hard_deficit + MAX_TICKS_FRAME;
                    } else if (hard_deficit < 0) {
                        // Instantly freeze the simulation to let the Host catch up
                        g_app.accumulator_ms = 0.0;
                        current_max_ticks = 0;
                    }
                    // No gentle drift during warmup; instant snaps
                    g_app.clock_drift_multiplier = 1.0f;

                } else {
                    // Gantle mode; Normal gameplay
                    if (hard_deficit > 4) {
                        if (g_app.accumulator_ms < deficit_ms) {
                            g_app.accumulator_ms = deficit_ms;
                        }
                        current_max_ticks = hard_deficit + MAX_TICKS_FRAME;
                    }

                    int tick_diff = static_cast<int>(target_tick) - static_cast<int>(g_app.sim.tick);

                    if (tick_diff > 0)
                        g_app.clock_drift_multiplier = 1.01f;
                    else if (tick_diff < 0)
                        g_app.clock_drift_multiplier = 0.99f;
                    else
                        g_app.clock_drift_multiplier = 1.0f;
                }
            }

            g_app.accumulator_ms += delta * g_app.clock_drift_multiplier;
            int ticks_run = 0;

            // Gate the simulation based on valid RTT
            bool can_simulate = g_app.game_started;

            if (can_simulate) {
                while (g_app.accumulator_ms >= TICK_MS && ticks_run < current_max_ticks) {
                    if (g_app.role == pong::Role::Host) {
                        game_tick();
                    } else {
                        bool had_schro = g_app.sim.has_schrodinger;
                        pong::sim_tick(g_app.sim, 0, guest_dir); // host paddle managed by PaddleState

                        // Guest hit detection trigger
                        if (!had_schro && g_app.sim.has_schrodinger && g_app.sim.schro_side == 1) {
                            uint8_t hit_type = pong::get_hit_type(g_app.sim.schro_spawn_y, g_app.sim.paddle_b_y);
                            uint8_t auth_buf[pong::AUTH_COLLISION_BYTES];
                            int alen = pong::encode_auth_collision(auth_buf, g_app.sim.schro_spawn_tick, hit_type, 1);
                            g_app.transport->send({ auth_buf, static_cast<size_t>(alen) });

                            pong::resolve_schrodinger(g_app.sim, hit_type, 1);
                            g_app.auth_resend = { true, g_app.sim.schro_spawn_tick, hit_type, 1, 30 };
                        }

                        if (g_app.sim.score_a >= pong::WIN_SCORE) {
                            g_app.game_over = true;
                            g_app.winner = 1;
                            break;
                        }
                        else if (g_app.sim.score_b >= pong::WIN_SCORE) {
                            g_app.game_over = true;
                            g_app.winner = 2;
                            break;
                        }

                        uint8_t pbuf[pong::PADDLE_STATE_MAX_BYTES];
                        int plen = pong::encode_paddle_state(pbuf, g_app.sim.tick, g_app.sim.paddle_b_y);
                        g_app.transport->send({ pbuf, static_cast<size_t>(plen) });

                        // Broadcast the collision event repeatedly
                        if (g_app.auth_resend.active && g_app.auth_resend.frames_left > 0) {
                            uint8_t auth_buf[pong::AUTH_COLLISION_BYTES];
                            int alen = pong::encode_auth_collision(auth_buf, g_app.auth_resend.spawn_tick, g_app.auth_resend.hit_type, g_app.auth_resend.side);
                            g_app.transport->send({ auth_buf, static_cast<size_t>(alen) });
                            g_app.auth_resend.frames_left--;
                        } else {
                            g_app.auth_resend.active = false;
                        }

                        ++g_app.local_tick;
                    }
                    g_app.accumulator_ms -= TICK_MS;
                    ++ticks_run;
                }
            } else {
                // Throw away accumulated time while waiting for sync
                g_app.accumulator_ms = 0.0;
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
    SetTraceLogCallback(CustomLogCallback);

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

    fetch_and_load_servers("https://gist.githubusercontent.com/lukehinojosa/582d21a38f95f97a87013e901f4943d6/raw/gistfile1.txt");

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