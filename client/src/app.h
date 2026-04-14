#pragma once
#include "ui.h"
#include "pong/codec.h"
#include "pong/messages.h"
#include "pong/role.h"
#include "pong/sim.h"
#include "pong/transport.h"
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

struct Snapshot {
    pong::QuantState state;
    double recv_time_ms;
};

struct RenderState {
    float ball_x = 0.f, ball_y = 0.f;
    float paddle_a_y = 0.f, paddle_b_y = 0.f;
    uint8_t score_a = 0, score_b = 0;

    // Speculative State
    bool is_split = false;
    float ghost_ball_x = 0.f, ghost_ball_y = 0.f;
};

struct App {
    pong::Role role = pong::Role::None;
    std::unique_ptr<pong::Transport> transport;
    pong::SimState sim;
    pong::QuantState last_tx_state;
    pong::QuantState last_rx_state;

    std::string lobby_code;
    bool peer_connected = false;
    bool connecting = false;
    bool game_over = false;
    bool show_menu = false;
    bool host_closed = false;
    int winner = 0;

    // Persistent text inputs
    TextEdit username_edit;
    TextEdit signaling_edit;
    TextEdit join_code_edit;
    TextSel lobby_code_sel;
    std::string opponent_username;

    Font code_font;

    std::array<pong::DecodedInput, 64> input_buf{};
    uint32_t ping_seq = 0;
    double last_ping_sent_ms = -1000.0;
    float rtt_ms = 60.f;
    int8_t last_guest_dir = 0;

    std::deque<Snapshot> snap_buf;
    std::mutex snap_mutex;

    uint32_t local_tick = 0;
    int8_t input_history[64] = {};

    double accumulator_ms = 0.0;
    double last_frame_ms = 0.0;

    float render_paddle_b_y = static_cast<float>(pong::SimState{}.paddle_b_y) / 100.f;
    double last_guest_input_ms = 0.0;
    bool guest_ever_sent_input = false;

    RenderTexture2D render_target = {};
};

extern App g_app;

void reset_app();