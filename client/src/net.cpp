#include "net.h"
#include "app.h"
#include "config.h"
#include "storage.h"
#include "pong/codec.h"
#include "pong/messages.h"
#include "raylib.h"
#include <algorithm>
#include <mutex>
#include <sstream>
#ifdef PONG_WASM
#  include <emscripten/emscripten.h>
#else
#  include <thread>
#  include <array>
#  include <memory>
#  include <atomic>
#endif

static std::vector<std::unique_ptr<pong::Transport>> g_pending_transports;
static std::atomic<int> g_failed_races{0};

void send_username() {
    if (!g_app.transport) return;
    uint8_t buf[pong::USERNAME_MAX_BYTES];
    int len = pong::encode_username(buf, g_app.username_edit.text);
    g_app.transport->send({ buf, static_cast<size_t>(len) });
}

void wire_host_game_callbacks() {
    g_app.transport->on_open = []() {
        g_app.peer_connected = true;
        g_app.current_seed = GetRandomValue(0, 0x7fffffff);
        pong::reset_sim(g_app.sim, g_app.current_seed);
        g_app.game_over = false;
        g_app.winner = 0;
        g_app.game_started = false;
        g_app.local_tick = 0;
        g_app.accumulator_ms = 0.0;
        g_app.last_frame_ms = now_ms();

        TraceLog(LOG_INFO, "[host] guest connected");
        send_username();
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
                if (g_app.sim.has_schrodinger && g_app.sim.schro_side == side) {
                    pong::resolve_schrodinger(g_app.sim, hit_type, side);
                } else if (tick >= g_app.sim.tick) {
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
}

void wire_guest_game_callbacks() {
    g_app.transport->on_message = [](std::span<const uint8_t> buf) {
        if (buf.empty()) return;
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
                if (g_app.latest_remote_tick > 0 && ps.tick == 0) {
                    TraceLog(LOG_INFO, "[guest] Host restarted game");
                    g_app.latest_remote_tick = 0;
                    g_app.local_tick = 0;
                    g_app.game_over = false;
                    g_app.winner = 0;
                    g_app.accumulator_ms = 0.0;
                    g_app.game_started = false;
                    g_app.guest_ready = false;
                    g_app.remote_ever_sent_paddle = false;
                    g_app.auth_resend = {};
                    g_app.render_remote_paddle_y = static_cast<float>((pong::FIELD_H - pong::PADDLE_H) / 2) / 100.f;
                    g_app.target_remote_paddle_y = static_cast<float>((pong::FIELD_H - pong::PADDLE_H) / 2) / 100.f;
                }
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
                    g_app.valid_pong_count++;
                    if (g_app.valid_pong_count >= 5) g_app.rtt_valid = true;
                }
            }
        } else if (type == pong::MsgType::AuthCollision) {
            uint32_t tick;
            uint8_t hit_type, side;
            if (pong::decode_auth_collision(buf, tick, hit_type, side)) {
                if (g_app.sim.has_schrodinger && g_app.sim.schro_side == side) {
                    pong::resolve_schrodinger(g_app.sim, hit_type, side);
                } else if (tick >= g_app.sim.tick) {
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
        g_app.host_closed = true;
        g_app.peer_connected = false;
    };
}

void start_as_host() {
    g_app.role = pong::Role::Host;

    if (g_app.server_list.empty()) g_app.server_list.push_back(g_app.signaling_edit.text);

    // Clean up any old attempts
    g_pending_transports.clear();
    g_app.transport.reset();
    g_failed_races = 0;

    for (const std::string& url : g_app.server_list) {
        auto t = pong::make_transport();
        pong::Transport* raw_t = t.get(); // Raw pointer to identify the winner

        // For the Host, receiving the lobby code means it successfully hit the signaling server
        t->on_lobby_code = [raw_t, url](const std::string& code) {
            if (g_app.transport) return; // Another server already won the race

            // Promote to winner
            for (auto it = g_pending_transports.begin(); it != g_pending_transports.end(); ++it) {
                if (it->get() == raw_t) {
                    g_app.transport = std::move(*it);
                    g_pending_transports.clear(); // This instantly destroys all losing connections
                    break;
                }
            }
            if (!g_app.transport) return; // Safety check

            // Lock in game callbacks
            wire_host_game_callbacks();

            // Setup UI
            g_app.lobby_code = code;
            g_app.lobby_code_sel = TextSel{};
            TraceLog(LOG_INFO, "[host] Race won. Connected to: %s | Lobby: %s", url.c_str(), code.c_str());
        };

        t->on_close = [url]() {
            if (g_app.transport) return; // Race already won, ignore losers closing
            g_failed_races++;
            TraceLog(LOG_WARNING, "[host] Connection failed for: %s", url.c_str());
            if (g_failed_races >= (int)g_app.server_list.size()) {
                g_app.role = pong::Role::None; // All servers failed
                TraceLog(LOG_ERROR, "[host] All signaling servers failed to connect.");
            }
        };

        t->host(url, g_app.username_edit.text);
        g_pending_transports.push_back(std::move(t));
    }
}

void start_as_guest(const std::string& code) {
    g_app.role = pong::Role::Guest;
    g_app.connecting = true;

    if (g_app.server_list.empty()) g_app.server_list.push_back(g_app.signaling_edit.text);

    g_pending_transports.clear();
    g_app.transport.reset();
    g_failed_races = 0;

    for (const std::string& url : g_app.server_list) {
        auto t = pong::make_transport();
        pong::Transport* raw_t = t.get();

        // For the Guest, on_open means the full WebRTC DataChannel connected to the host
        t->on_open = [raw_t, url]() {
            if (g_app.transport) return; // Another server won

            // Promote to winner
            for (auto it = g_pending_transports.begin(); it != g_pending_transports.end(); ++it) {
                if (it->get() == raw_t) {
                    g_app.transport = std::move(*it);
                    g_pending_transports.clear();
                    break;
                }
            }
            if (!g_app.transport) return;

            // Lock in game callbacks
            wire_guest_game_callbacks();

            // Initialize guest simulation
            g_app.peer_connected = true;
            pong::PingMsg ping;
            ping.seq = ++g_app.ping_seq;
            ping.client_ts = static_cast<uint32_t>(GetTime() * 1000.0);
            g_app.last_ping_sent_ms = GetTime() * 1000.0;
            g_app.transport->send_msg(ping);

            g_app.sim = pong::SimState{};
            g_app.latest_remote_tick = 0;
            g_app.local_tick = 0;
            g_app.game_over = false;
            g_app.winner = 0;
            g_app.accumulator_ms = 0.0;
            g_app.last_frame_ms = now_ms();
            g_app.remote_ever_sent_paddle = false;
            g_app.game_started = false;
            g_app.guest_ready = false;
            g_app.guest_start_timer = 0.0;

            TraceLog(LOG_INFO, "[guest] race won. Established Peer-to-Peer via: %s", url.c_str());
            send_username();
        };

        t->on_close = [url]() {
            if (g_app.transport) return; // Race already won
            g_failed_races++;
            TraceLog(LOG_WARNING, "[guest] Connection failed for: %s", url.c_str());
            if (g_failed_races >= (int)g_app.server_list.size()) {
                g_app.connecting = false;
                g_app.join_code_edit = TextEdit{};
                g_app.role = pong::Role::Guest;
                TraceLog(LOG_ERROR, "[guest] All servers failed to broker connection.");
            }
        };

        t->join(url, code);
        g_pending_transports.push_back(std::move(t));
    }
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

void load_server_list(const std::string& raw_pastebin_text) {
    g_app.server_list.clear();
    std::istringstream stream(raw_pastebin_text);
    std::string line;

    while (std::getline(stream, line)) {
        // Strip whitespace and carriage returns
        line.erase(std::remove_if(line.begin(), line.end(), ::isspace), line.end());

        if (!line.empty()) {
            // If it already has a protocol, add it as-is
            if (line.find("://") != std::string::npos) {
                g_app.server_list.push_back(line);
            } else {
                // Otherwise, format it with the default wss and port 9000
                g_app.server_list.push_back("wss://" + line + ":9000");
            }
        }
    }
}

#ifndef PONG_WASM
// Native state for safely handing data from the background thread to the main thread
static std::string g_pending_server_list;
static std::atomic<bool> g_has_pending_server_list{false};
#endif

#ifdef PONG_WASM
// Export this so the JS fetch promise can call it upon completion
extern "C" void EMSCRIPTEN_KEEPALIVE wasm_on_server_list(const char* text) {
    load_server_list(text);
}

// Inline JS to fire an async HTTP request without blocking the WASM thread
EM_JS(void, js_fetch_servers, (const char* url), {
    fetch(UTF8ToString(url))
        .then(response => response.text())
        .then(text => {
            // Send the raw text back to C++ using ccall
            ccall('wasm_on_server_list', 'void', ['string'], [text]);
        })
        .catch(err => console.error("[net] Failed to fetch server list:", err));
});
#endif

void fetch_and_load_servers(const std::string& url) {
#ifdef PONG_WASM
    js_fetch_servers(url.c_str());
#else
    // Fire a detached background thread so Raylib doesn't freeze while fetching
    std::thread([url]() {
        std::string result;
        std::string cmd = "curl -s " + url; // -s for silent mode

        #ifdef _WIN32
        std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
        #else
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        #endif

        if (pipe) {
            std::array<char, 256> buffer;
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                result += buffer.data();
            }
        }

        // Save the result and flip the atomic flag so the main thread can process it
        g_pending_server_list = result;
        g_has_pending_server_list = true;
    }).detach();
#endif
}

void process_pending_server_list() {
#ifndef PONG_WASM
    // Check if the background thread finished fetching. If so, process it on the safe main thread.
    if (g_has_pending_server_list) {
        load_server_list(g_pending_server_list);
        g_has_pending_server_list = false;
        TraceLog(LOG_INFO, "[net] Server list loaded natively from web.");
    }
#endif
    // (In WASM, ccall executes on the main thread automatically, so no queue check is needed here)
}

std::mutex g_lobby_mutex;

void refresh_lobby_list() {
    if (g_app.server_list.empty()) g_app.server_list.push_back(g_app.signaling_edit.text);

    // Clear the current list immediately so the UI shows it's refreshing
    {
        std::lock_guard<std::mutex> lk(g_lobby_mutex);
        g_app.lobby_list.clear();
    }

    // Ping all servers in your fallback list
    for (const std::string& url : g_app.server_list) {
        pong::fetch_lobbies(url, [](const std::vector<pong::LobbyInfo>& lobbies) {
            // Safely update the global app state
            std::lock_guard<std::mutex> lk(g_lobby_mutex);
            g_app.lobby_list = lobbies;
        });
    }
}