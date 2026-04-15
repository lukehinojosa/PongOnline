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

    uint32_t ping_seq = 0;
    double last_ping_sent_ms = -1000.0;
    float rtt_ms = 60.f;

    // PLL clock synchronisation (guest only)
    double time_offset_ms = 0.0;
    float clock_drift_multiplier = 1.0f;

    uint32_t local_tick = 0;
    double accumulator_ms = 0.0;
    double last_frame_ms = 0.0;

    // Symmetric remote tracking for timeouts and rendering
    float render_remote_paddle_y = static_cast<float>((pong::FIELD_H - pong::PADDLE_H) / 2) / 100.f;
    float target_remote_paddle_y = static_cast<float>((pong::FIELD_H - pong::PADDLE_H) / 2) / 100.f;
    double last_remote_paddle_ms = 0.0;
    bool remote_ever_sent_paddle = false;
    uint32_t latest_remote_tick = 0;

    RenderTexture2D render_target = {};

    // Packet loss resilience for one-shot events
    struct AuthResend {
        bool active = false;
        uint32_t spawn_tick = 0;
        uint8_t did_hit = 0;
        uint8_t side = 0;
        int frames_left = 0;
    } auth_resend;
};

extern App g_app;

void reset_app();