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

        // Generate and apply the initial seed
        g_app.current_seed = GetRandomValue(0, 0x7fffffff);
        pong::reset_sim(g_app.sim, g_app.current_seed);

        // Reset state so new guests don't inherit a massive tick deficit
        g_app.game_over = false;
        g_app.winner = 0;
        g_app.game_started = false;
        g_app.local_tick = 0;
        g_app.accumulator_ms = 0.0;
        g_app.last_frame_ms = now_ms();

        TraceLog(LOG_INFO, "[host] guest connected");
        send_username();

        // Send the initial seed to the guest
        uint8_t seed_buf[pong::SEED_BYTES];
        int slen = pong::encode_seed(seed_buf, g_app.current_seed);
        g_app.transport->send({ seed_buf, static_cast<size_t>(slen) });
    };
    g_app.transport->on_message = [](std::span<const uint8_t> buf) {
        if (buf.empty()) return;
        auto type = pong::peek_type(buf);

        if (type == pong::MsgType::PaddleState) {
            pong::DecodedPaddleState ps;
            if (pong::decode_paddle_state(buf, ps)) {
                g_app.sim.paddle_b_y = ps.paddle_y;
                g_app.target_remote_paddle_y = static_cast<float>(ps.paddle_y) / 100.f;
                g_app.last_remote_paddle_ms = now_ms();
                g_app.remote_ever_sent_paddle = true;
            }
        } else if (type == pong::MsgType::Ping) {
            uint32_t seq, client_ts;
            if (pong::decode_ping(buf, seq, client_ts)) {
                uint32_t server_ts = static_cast<uint32_t>(static_cast<uint64_t>(now_ms()) & 0xFFFFFFFF);
                uint8_t pbuf[pong::PONG_BYTES];
                int len = pong::encode_pong(pbuf, seq, client_ts, server_ts);
                g_app.transport->send({ pbuf, static_cast<size_t>(len) });
            }
        } else if (type == pong::MsgType::AuthCollision) {
            uint32_t tick;
            uint8_t hit_type, side;
            if (pong::decode_auth_collision(buf, tick, hit_type, side)) {
            // Match by side, not by exact tick, for minor desyncs
                if (g_app.sim.has_schrodinger && g_app.sim.schro_side == side) {
                    pong::resolve_schrodinger(g_app.sim, hit_type, side);
                } else if (tick >= g_app.sim.tick) {
                    // Arrived before the ball reached the paddle locally
                    g_app.sim.has_pending_auth = true;
                    g_app.sim.pending_auth_tick = tick;
                    g_app.sim.pending_auth_hit_type = hit_type;
                    g_app.sim.pending_auth_side = side;
                }
            }
        } else if (type == pong::MsgType::Username) {
            std::string name;
            if (pong::decode_username(buf, name)) g_app.opponent_username = name;
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

        // Force an immediate Ping to establish true RTT before starting
        pong::PingMsg ping;
        ping.seq = ++g_app.ping_seq;
        ping.client_ts = static_cast<uint32_t>(GetTime() * 1000.0);
        g_app.last_ping_sent_ms = GetTime() * 1000.0;
        g_app.transport->send_msg(ping);

        // Clean state upon joining
        g_app.sim = pong::SimState{};
        g_app.latest_remote_tick = 0;
        g_app.local_tick = 0;
        g_app.game_over = false;
        g_app.winner = 0;
        g_app.accumulator_ms = 0.0;
        g_app.last_frame_ms = now_ms();
        g_app.remote_ever_sent_paddle = false;

        // Zero-Snap Handshake states
        g_app.game_started = false;
        g_app.guest_ready = false;
        g_app.guest_start_timer = 0.0;

        TraceLog(LOG_INFO, "[guest] connected to host");
        send_username();
    };

    g_app.transport->on_message = [](std::span<const uint8_t> buf) {
        if (buf.empty())
            return;
        auto type = pong::peek_type(buf);

        if (type == pong::MsgType::Seed) {
            uint32_t seed;
            if (pong::decode_seed(buf, seed)) {
                g_app.current_seed = seed;
                pong::reset_sim(g_app.sim, g_app.current_seed);
            }
        } else if (type == pong::MsgType::PaddleState) {
            pong::DecodedPaddleState ps;
            if (pong::decode_paddle_state(buf, ps)) {

                // Detect Restart; If the host suddenly sends tick 0, they restarted the game
                if (g_app.latest_remote_tick > 0 && ps.tick == 0) {
                    TraceLog(LOG_INFO, "[guest] Host restarted game");
                    g_app.latest_remote_tick = 0;
                    g_app.local_tick = 0;
                    g_app.game_over = false;
                    g_app.winner = 0;
                    g_app.accumulator_ms = 0.0;

                    // Reset handshake states
                    g_app.game_started = false;
                    g_app.guest_ready = false;
                    g_app.remote_ever_sent_paddle = false;

                    // Clear any lingering collision resends
                    g_app.auth_resend = {};

                    // Reset rendering targets
                    g_app.render_remote_paddle_y = static_cast<float>((pong::FIELD_H - pong::PADDLE_H) / 2) / 100.f;
                    g_app.target_remote_paddle_y = static_cast<float>((pong::FIELD_H - pong::PADDLE_H) / 2) / 100.f;
                }

                // Track the highest tick seen from the host
                g_app.latest_remote_tick = std::max(g_app.latest_remote_tick, ps.tick);

                g_app.sim.paddle_a_y = ps.paddle_y;
                g_app.target_remote_paddle_y = static_cast<float>(ps.paddle_y) / 100.f;
                g_app.last_remote_paddle_ms = now_ms();
                g_app.remote_ever_sent_paddle = true;
            }
        } else if (type == pong::MsgType::Pong) {
            uint32_t seq, client_ts, server_ts;

            if (pong::decode_pong(buf, seq, client_ts, server_ts)) {
                uint32_t now32 = static_cast<uint32_t>(static_cast<uint64_t>(now_ms()) & 0xFFFFFFFF);

                // Prevent processing ancient wrapped packets
                int32_t rtt_diff = static_cast<int32_t>(now32 - client_ts);
                if (rtt_diff >= 0 && rtt_diff < 10000) {
                    double rtt = static_cast<double>(rtt_diff);

                    if (g_app.time_offset_ms == 0.0) {
                        g_app.rtt_ms = static_cast<float>(rtt);
                    } else {
                        g_app.rtt_ms = RTT_EWMA_ALPHA * static_cast<float>(rtt) + (1.f - RTT_EWMA_ALPHA) * g_app.rtt_ms;
                    }

                    double one_way = rtt / 2.0;
                    double estimated_server_now = static_cast<double>(server_ts) + one_way;
                    double raw_offset = estimated_server_now - static_cast<double>(now32);
                    g_app.time_offset_ms = (g_app.time_offset_ms == 0.0) ? raw_offset : (0.8 * g_app.time_offset_ms + 0.2 * raw_offset);

                    // Wait for 5 pongs to stabilize the EWMA before unlocking
                    g_app.valid_pong_count++;
                    if (g_app.valid_pong_count >= 5) {
                        g_app.rtt_valid = true;
                    }
                }
            }
        } else if (type == pong::MsgType::AuthCollision) {
            uint32_t tick;
            uint8_t hit_type, side;
            if (pong::decode_auth_collision(buf, tick, hit_type, side)) {
                // Match by side, not by exact tick, for minor desyncs
                if (g_app.sim.has_schrodinger && g_app.sim.schro_side == side) {
                    pong::resolve_schrodinger(g_app.sim, hit_type, side);
                } else if (tick >= g_app.sim.tick) {
                    // Arrived before the ball reached the paddle locally
                    g_app.sim.has_pending_auth = true;
                    g_app.sim.pending_auth_tick = tick;
                    g_app.sim.pending_auth_hit_type = hit_type;
                    g_app.sim.pending_auth_side = side;
                }
            }
        } else if (type == pong::MsgType::Username) {
            std::string name;
            if (pong::decode_username(buf, name)) g_app.opponent_username = name;
        }
    };

    g_app.transport->on_close = []() {
        if (!g_app.peer_connected) {
            g_app.connecting = false;
            g_app.join_code_edit = TextEdit{};
            g_app.role = pong::Role::Guest;
        } else {
            g_app.host_closed = true;
        }
        g_app.peer_connected = false;
    };

    g_app.transport->join(g_app.signaling_edit.text, code);
}

void game_tick() {
    if (g_app.game_over) return;

    int8_t dir_a = 0;
    if (!g_app.show_menu) {
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W)) dir_a = -1;
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S)) dir_a = 1;
    }

    bool had_schro = g_app.sim.has_schrodinger;
    pong::sim_tick(g_app.sim, dir_a, 0); // guest paddle managed by PaddleState directly

    // Host hit detection trigger
    if (!had_schro && g_app.sim.has_schrodinger && g_app.sim.schro_side == 0) {
        uint8_t hit_type = pong::get_hit_type(g_app.sim.schro_spawn_y, g_app.sim.paddle_a_y);
        uint8_t auth_buf[pong::AUTH_COLLISION_BYTES];
        int alen = pong::encode_auth_collision(auth_buf, g_app.sim.schro_spawn_tick, hit_type, 0);
        g_app.transport->send({ auth_buf, static_cast<size_t>(alen) });

        pong::resolve_schrodinger(g_app.sim, hit_type, 0);
        g_app.auth_resend = { true, g_app.sim.schro_spawn_tick, hit_type, 0, 30 };
    }

    if (g_app.sim.score_a >= pong::WIN_SCORE) { g_app.game_over = true; g_app.winner = 1; }
    else if (g_app.sim.score_b >= pong::WIN_SCORE) { g_app.game_over = true; g_app.winner = 2; }

    uint8_t sbuf[pong::PADDLE_STATE_MAX_BYTES];
    int slen = pong::encode_paddle_state(sbuf, g_app.sim.tick, g_app.sim.paddle_a_y);
    g_app.transport->send({ sbuf, static_cast<size_t>(slen) });

    if (g_app.auth_resend.active && g_app.auth_resend.frames_left > 0) {
        uint8_t auth_buf[pong::AUTH_COLLISION_BYTES];
        int alen = pong::encode_auth_collision(auth_buf, g_app.auth_resend.spawn_tick, g_app.auth_resend.hit_type, g_app.auth_resend.side);
        g_app.transport->send({ auth_buf, static_cast<size_t>(alen) });
        g_app.auth_resend.frames_left--;
    } else {
        g_app.auth_resend.active = false;
    }
}
