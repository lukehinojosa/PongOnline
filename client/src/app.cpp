#include "app.h"
#include <algorithm>
#include <mutex>

App g_app;

void reset_app() {
    if (g_app.transport) { g_app.transport->close(); g_app.transport.reset(); }
    g_app.role = pong::Role::None;
    g_app.sim = pong::SimState{};
    g_app.lobby_code.clear();
    g_app.peer_connected = false;
    g_app.connecting = false;
    g_app.game_over = false;
    g_app.show_menu = false;
    g_app.host_closed = false;
    g_app.game_started = false;
    g_app.guest_ready = false;
    g_app.guest_start_timer = 0.0;
    g_app.winner = 0;
    g_app.opponent_username.clear();
    g_app.join_code_edit = TextEdit{};
    g_app.lobby_code_sel = TextSel{};

    g_app.ping_seq = 0;
    g_app.last_ping_sent_ms = -1000.0;
    g_app.rtt_ms = 60.f;
    g_app.rtt_valid = false;
    g_app.valid_pong_count = 0;
    g_app.time_offset_ms = 0.0;
    g_app.clock_drift_multiplier = 1.0f;

    g_app.local_tick = 0;
    g_app.latest_remote_tick = 0;
    g_app.accumulator_ms = 0.0;
    g_app.last_frame_ms = 0.0;

    float start_y = static_cast<float>((pong::FIELD_H - pong::PADDLE_H) / 2) / 100.f;
    g_app.render_remote_paddle_y = start_y;
    g_app.target_remote_paddle_y = start_y;
    g_app.last_remote_paddle_ms = 0.0;
    g_app.remote_ever_sent_paddle = false;
    g_app.render_ball_x = static_cast<float>(pong::FIELD_W / 2) / 100.f;
    g_app.render_ball_y = static_cast<float>(pong::FIELD_H / 2) / 100.f;

    // username_edit and signaling_edit are intentionally not reset
    g_app.username_edit.focused = false;
    g_app.signaling_edit.focused = false;

    g_app.auth_resend = {};
}
