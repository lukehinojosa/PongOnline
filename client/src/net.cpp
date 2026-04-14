#include "net.h"
#include "app.h"
#include "config.h"
#include "storage.h"
#include "pong/codec.h"
#include "pong/messages.h"
#include "raylib.h"
#include <algorithm>
#include <mutex>

void send_username() {
    if (!g_app.transport) return;
    uint8_t buf[pong::USERNAME_MAX_BYTES];
    int len = pong::encode_username(buf, g_app.username_edit.text);
    g_app.transport->send({ buf, static_cast<size_t>(len) });
}

void start_as_host() {
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
        } else if (type == pong::MsgType::Ping) {
            // Guest is the time-client; host echoes the ping back with its own timestamp.
            uint32_t seq, client_ts;
            if (pong::decode_ping(buf, seq, client_ts)) {
                uint32_t server_ts = static_cast<uint32_t>(static_cast<uint64_t>(now_ms()) & 0xFFFFFFFF);
                uint8_t pbuf[pong::PONG_BYTES];
                int len = pong::encode_pong(pbuf, seq, client_ts, server_ts);
                g_app.transport->send({ pbuf, static_cast<size_t>(len) });
            }
        } else if (type == pong::MsgType::AuthCollision) {
            // Guest reports whether they hit the ball that just crossed b_face.
            uint32_t tick; uint8_t did_hit;
            if (pong::decode_auth_collision(buf, tick, did_hit) &&
                g_app.sim.has_schrodinger && tick == g_app.sim.schro_spawn_tick) {
                pong::resolve_schrodinger(g_app.sim, did_hit != 0);
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

void start_as_guest(const std::string& code) {
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
                const uint8_t* pp = buf.data() + 1;
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

                // Detect a new Schrödinger ball event and respond with our verdict.
                // We use the paddle position at schro_spawn_tick by replaying input
                // history forward from the host's last known paddle_b_y.
                if (tmp.has_schrodinger && tmp.schro_spawn_tick != g_app.last_schro_spawn_tick) {
                    g_app.last_schro_spawn_tick = tmp.schro_spawn_tick;

                    int32_t paddle = tmp.paddle_b_y; // base: host's latest known position
                    const uint32_t schro_tick = tmp.schro_spawn_tick;
                    const uint32_t from = tmp.tick + 1;
                    if (schro_tick >= from) {
                        const uint32_t replay_to = schro_tick < g_app.local_tick
                            ? schro_tick : g_app.local_tick;
                        for (uint32_t t = from; t <= replay_to; ++t) {
                            paddle += static_cast<int32_t>(g_app.input_history[t % 64]) * pong::PADDLE_SPD;
                            if (paddle < 0) paddle = 0;
                            if (paddle > pong::FIELD_H - pong::PADDLE_H)
                                paddle = pong::FIELD_H - pong::PADDLE_H;
                        }
                    }

                    const int32_t spawn_y = tmp.schro_spawn_y; // 1/100-px units
                    const bool did_hit =
                        (spawn_y + pong::BALL_SIZE >= paddle) &&
                        (spawn_y <= paddle + pong::PADDLE_H);

                    uint8_t auth_buf[pong::AUTH_COLLISION_BYTES];
                    const int alen = pong::encode_auth_collision(
                        auth_buf, schro_tick, did_hit ? 1u : 0u);
                    g_app.transport->send({ auth_buf, static_cast<size_t>(alen) });
                }
            }
        } else if (type == pong::MsgType::Pong) {
            // NTP-style clock offset + RTT measurement.
            // offset = ServerTime - (T0 + RTT/2)  →  HostTime ≈ LocalTime + offset
            uint32_t seq, client_ts, server_ts;
            if (pong::decode_pong(buf, seq, client_ts, server_ts) && seq == g_app.ping_seq - 1) {
                uint32_t now32 = static_cast<uint32_t>(static_cast<uint64_t>(now_ms()) & 0xFFFFFFFF);
                double rtt = static_cast<double>((now32 - client_ts) & 0xFFFFFFFF);

                g_app.rtt_ms = RTT_EWMA_ALPHA * static_cast<float>(rtt)
                    + (1.f - RTT_EWMA_ALPHA) * g_app.rtt_ms;

                double one_way = rtt / 2.0;
                double estimated_server_now = static_cast<double>(server_ts) + one_way;
                double raw_offset = estimated_server_now - static_cast<double>(now32);

                // Smooth the offset to reject jitter; first sample sets it directly.
                g_app.time_offset_ms = (g_app.time_offset_ms == 0.0)
                    ? raw_offset
                    : (0.8 * g_app.time_offset_ms + 0.2 * raw_offset);
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
            g_app.host_closed = true;  // handled next frame then reset_app()
        }
        g_app.peer_connected = false;
    };
    g_app.transport->join(g_app.signaling_edit.text, code);
}

void game_tick() {
    if (g_app.game_over)
        return;

    int8_t dir_a = 0;
    if (!g_app.show_menu) {
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))
            dir_a = -1;
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
            dir_a = 1;
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
}
