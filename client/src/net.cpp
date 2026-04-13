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

    double t = now_ms();
    if (t - g_app.last_ping_sent_ms >= PING_INTERVAL_MS) {
        g_app.last_ping_sent_ms = t;
        uint32_t ts32 = static_cast<uint32_t>(static_cast<uint64_t>(t) & 0xFFFFFFFF);
        uint8_t pbuf[pong::PING_BYTES];
        g_app.transport->send({ pbuf, static_cast<size_t>(pong::encode_ping(pbuf, g_app.ping_seq++, ts32)) });
    }
}
