#include "app.h"
#include <algorithm>
#include <mutex>

App g_app;

void reset_app() {
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
