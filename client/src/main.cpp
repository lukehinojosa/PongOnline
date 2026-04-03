#include "pong/messages.h"
#include "pong/role.h"
#include "pong/sim.h"
#include "pong/transport.h"

#include "raylib.h"

#include <array>
#include <cstdio>
#include <memory>
#include <string>

#ifdef PONG_WASM
#  include <emscripten/emscripten.h>
#endif

// Config
static constexpr int SCREEN_W = 800;
static constexpr int SCREEN_H = 600;
static constexpr int TICK_HZ = 60;
static constexpr char SIGNALING_URL[] = "ws://localhost:9000";

// App state
struct App {
    pong::Role role = pong::Role::None;
    std::unique_ptr<pong::Transport> transport;
    pong::SimState sim;

    std::string lobby_code;
    std::string join_code_input;
    bool peer_connected = false;

    // Per-tick input ring. Host stores last 64 guest inputs
    std::array<pong::InputMsg, 64> input_buf{};
};

static App g_app;

// Role setup
static void start_as_host() {
    g_app.role = pong::Role::Host;
    g_app.transport = pong::make_transport();

    g_app.transport->on_lobby_code = [](const std::string& code) {
        g_app.lobby_code = code;
        TraceLog(LOG_INFO, "[host] lobby code: %s", code.c_str());
    };
    g_app.transport->on_open = []() {
        g_app.peer_connected = true;
        TraceLog(LOG_INFO, "[host] guest connected");
    };
    g_app.transport->on_message = [](std::span<const uint8_t> buf) {
        if (buf.empty())
            return;
        if (pong::peek_type(buf) == pong::MsgType::Input) {
            const auto* msg = pong::msg_cast<pong::InputMsg>(buf);
            if (msg)
                g_app.input_buf[msg->tick % 64] = *msg;
        }
    };
    g_app.transport->on_close = []() { g_app.peer_connected = false; };

    g_app.transport->host(SIGNALING_URL);
}

static void start_as_guest(const std::string& code) {
    g_app.role = pong::Role::Guest;
    g_app.transport = pong::make_transport();

    g_app.transport->on_open = []() {
        g_app.peer_connected = true;
        TraceLog(LOG_INFO, "[guest] connected to host");
    };
    g_app.transport->on_message = [](std::span<const uint8_t> buf) {
        if (buf.empty())
            return;
        auto type = pong::peek_type(buf);
        if (type == pong::MsgType::GameState) {
            const auto* msg = pong::msg_cast<pong::GameStateMsg>(buf);
            if (msg)
                pong::msg_to_sim(*msg, g_app.sim);
        }
    };
    g_app.transport->on_close = []() { g_app.peer_connected = false; };

    g_app.transport->join(SIGNALING_URL, code);
}

// ── Game tick (host only) ─────────────────────────────────────
static void game_tick() {
    int8_t dir_a = 0;
    if (IsKeyDown(KEY_W))
        dir_a = -1;
    if (IsKeyDown(KEY_S))
        dir_a =  1;

    int8_t dir_b = 0;
    const auto& gi = g_app.input_buf[g_app.sim.tick % 64];
    if (gi.tick == g_app.sim.tick)
        dir_b = gi.dir;

    pong::sim_tick(g_app.sim, dir_a, dir_b);

    if (g_app.sim.tick % 2 == 0) {
        auto msg = pong::sim_to_msg(g_app.sim);
        g_app.transport->send_msg(msg);
    }
}

// Draw
static void draw_game() {
    const auto& s = g_app.sim;
    ClearBackground(BLACK);

    for (int y = 0; y < SCREEN_H; y += 30)
        DrawRectangle(SCREEN_W / 2 - 2, y, 4, 18, GRAY);

    DrawRectangle(
        static_cast<int>(s.ball_x / 100), static_cast<int>(s.ball_y / 100),
        10, 10, WHITE);

    int ph = pong::PADDLE_H / 100;
    int pw = pong::PADDLE_W / 100;
    DrawRectangle(0, static_cast<int>(s.paddle_a_y / 100), pw, ph, WHITE);
    DrawRectangle(SCREEN_W - pw, static_cast<int>(s.paddle_b_y / 100), pw, ph, WHITE);

    DrawText(TextFormat("%d", s.score_a), SCREEN_W / 2 - 60, 20, 40, WHITE);
    DrawText(TextFormat("%d", s.score_b), SCREEN_W / 2 + 30, 20, 40, WHITE);
}

static void draw_lobby() {
    ClearBackground(BLACK);
    if (g_app.role == pong::Role::None) {
        DrawText("ONLINE PONG", SCREEN_W / 2 - 120, 160, 40, GREEN);
        DrawText("[H] Host a game", SCREEN_W / 2 - 100, 260, 24, WHITE);
        DrawText("[J] Join a game", SCREEN_W / 2 - 100, 300, 24, WHITE);

    } else if (g_app.role == pong::Role::Host) {
        DrawText("Waiting for guest...", 240, 200, 28, WHITE);
        if (!g_app.lobby_code.empty()) {
            DrawText("Lobby code:", 280, 270, 24, GRAY);
            DrawText(g_app.lobby_code.c_str(), 280, 306, 40, GREEN);
        }

    } else if (g_app.role == pong::Role::Guest) {
        DrawText("Enter lobby code:", 220, 200, 28, WHITE);
        DrawText(g_app.join_code_input.c_str(), 280, 260, 40, GREEN);
        DrawText("Press ENTER to join", 240, 330, 20, GRAY);
    }
}

// Main loop
static void main_loop() {
    if (g_app.role == pong::Role::None) {
        if (IsKeyPressed(KEY_H))
            start_as_host();
        if (IsKeyPressed(KEY_J))
            g_app.role = pong::Role::Guest;

    } else if (g_app.role == pong::Role::Guest && !g_app.peer_connected) {
        int key = GetCharPressed();
        while (key > 0) {
            if (g_app.join_code_input.size() < 6)
                g_app.join_code_input += static_cast<char>(key);
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !g_app.join_code_input.empty())
            g_app.join_code_input.pop_back();
        if (IsKeyPressed(KEY_ENTER) && g_app.join_code_input.size() == 6)
            start_as_guest(g_app.join_code_input);

    } else if (g_app.peer_connected) {
        if (g_app.role == pong::Role::Guest) {
            int8_t dir = 0;
            if (IsKeyDown(KEY_UP))
                dir = -1;
            if (IsKeyDown(KEY_DOWN))
                dir =  1;
            pong::InputMsg msg{};
            msg.tick = g_app.sim.tick;
            msg.dir = dir;
            msg.checksum = pong::sim_checksum(g_app.sim);
            g_app.transport->send_msg(msg);
        }
        if (g_app.role == pong::Role::Host)
            game_tick();
    }

    BeginDrawing();
    if (g_app.peer_connected)
        draw_game();
    else
        draw_lobby();
    EndDrawing();
}

// Entry point
int main() {
    InitWindow(SCREEN_W, SCREEN_H, "Online Pong");
    SetTargetFPS(TICK_HZ);

#ifdef PONG_WASM
    emscripten_set_main_loop(main_loop, TICK_HZ, 1);
#else
    while (!WindowShouldClose())
        main_loop();
    CloseWindow();
#endif
    return 0;
}
