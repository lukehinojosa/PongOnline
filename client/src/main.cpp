#include "pong/codec.h"
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
    pong::QuantState last_tx_state;
    pong::QuantState last_rx_state;

    std::string lobby_code;
    std::string join_code_input;
    bool peer_connected = false;
    bool connecting = false;
    bool game_over = false;
    bool show_menu = false;
    int winner = 0; // 1 = host (A), 2 = guest (B)

    Font code_font;

    // Per-tick input ring. Host stores last 64 guest inputs
    std::array<pong::DecodedInput, 64> input_buf{};
};

static App g_app;

static void reset_app() {
    if (g_app.transport) {
        g_app.transport->close();
        g_app.transport.reset();
    }
    g_app.role = pong::Role::None;
    g_app.sim = pong::SimState{};
    g_app.last_tx_state = pong::QuantState{};
    g_app.last_rx_state = pong::QuantState{};
    g_app.lobby_code.clear();
    g_app.join_code_input.clear();
    g_app.peer_connected = false;
    g_app.connecting = false;
    g_app.game_over = false;
    g_app.show_menu = false;
    g_app.winner = 0;
    g_app.input_buf = {};
}

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
    if (buf.empty()) return;
    auto type = pong::peek_type(buf);

    if (type == pong::MsgType::Input) {
        pong::DecodedInput in;
        // You'll need to implement or call your specific decode_input function here
        if (pong::decode_input(buf, in)) {
            // Store the guest's input in the ring buffer at the correct tick index
            g_app.input_buf[in.tick % 64] = in;
        }
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
            if (pong::decode_game_state(buf, g_app.sim, g_app.last_rx_state)) {
                if (g_app.sim.score_a >= pong::WIN_SCORE) {
                    g_app.game_over = true;
                    g_app.winner = 1;
                } else if (g_app.sim.score_b >= pong::WIN_SCORE) {
                    g_app.game_over = true;
                    g_app.winner = 2;
                }
            }
        }
    };
    g_app.transport->on_close = []() {
        if (!g_app.peer_connected) {
            // Failed before connecting (bad code, network error, etc.); let user retry
            g_app.connecting = false;
            g_app.join_code_input.clear();
            g_app.role = pong::Role::Guest; // stay on guest entry screen
        }
        g_app.peer_connected = false;
    };

    g_app.transport->join(SIGNALING_URL, code);
}

// Game tick (host only)
static void game_tick() {
    if (g_app.game_over)
        return;

    int8_t dir_a = 0;
    if (!g_app.show_menu) {
        if (IsKeyDown(KEY_UP))
            dir_a = -1;
        if (IsKeyDown(KEY_DOWN))
            dir_a =  1;
    }

    int8_t dir_b = 0;
    const auto& gi = g_app.input_buf[g_app.sim.tick % 64];
    if (gi.tick == g_app.sim.tick)
        dir_b = gi.dir;

    pong::sim_tick(g_app.sim, dir_a, dir_b);

    if (g_app.sim.score_a >= pong::WIN_SCORE) {
        g_app.game_over = true;
        g_app.winner = 1;
    } else if (g_app.sim.score_b >= pong::WIN_SCORE) {
        g_app.game_over = true;
        g_app.winner = 2;
    }

    // 60Hz delta-compressed encode and send
    pong::QuantState cur_q = pong::sim_quantize(g_app.sim);
    uint8_t dmask = pong::compute_delta_mask(g_app.last_tx_state, cur_q);

    uint8_t buf[pong::GAMESTATE_MAX_BYTES];
    int len = pong::encode_game_state(buf, g_app.sim, g_app.last_tx_state, dmask);
    g_app.transport->send({ buf, static_cast<size_t>(len) });
    g_app.last_tx_state = cur_q;
}

static bool draw_button(const char* text, int x, int y, int w, int h) {
    Rectangle rect = { static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h) };
    bool hover = CheckCollisionPointRec(GetMousePosition(), rect);
    DrawRectangleRec(rect, hover ? DARKGRAY : GRAY);
    DrawRectangleLinesEx(rect, 2, hover ? WHITE : LIGHTGRAY);
    int tw = MeasureText(text, 20);
    DrawText(text, x + w / 2 - tw / 2, y + h / 2 - 10, 20, WHITE);
    return hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

// Draw
static void draw_game() {
    const auto& s = g_app.sim;
    ClearBackground(BLACK);

    // Center dashed line
    for (int y = 0; y < SCREEN_H; y += 30)
        DrawRectangle(SCREEN_W / 2 - 2, y, 4, 18, GRAY);

    // Ball
    DrawRectangle(
        static_cast<int>(s.ball_x / 100), static_cast<int>(s.ball_y / 100),
        10, 10, WHITE);

    // Paddles
    int ph = pong::PADDLE_H / 100;
    int pw = pong::PADDLE_W / 100;
    DrawRectangle(0, static_cast<int>(s.paddle_a_y / 100), pw, ph, WHITE);
    DrawRectangle(SCREEN_W - pw, static_cast<int>(s.paddle_b_y / 100), pw, ph, WHITE);

    // Scores centered at top
    static constexpr int SCORE_FONT = 48;
    static constexpr int SCORE_GAP = 24; // gap from center line on each side
    static constexpr int SCORE_Y = 16;
    const char* score_a_str = TextFormat("%d", s.score_a);
    const char* score_b_str = TextFormat("%d", s.score_b);
    int wa = MeasureText(score_a_str, SCORE_FONT);
    int wb = MeasureText(score_b_str, SCORE_FONT);
    DrawText(score_a_str, SCREEN_W / 2 - SCORE_GAP - wa, SCORE_Y, SCORE_FONT, WHITE);
    DrawText(score_b_str, SCREEN_W / 2 + SCORE_GAP,      SCORE_Y, SCORE_FONT, WHITE);

    // Win overlay & menus
    if (g_app.game_over) {
        DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Color{0, 0, 0, 160});
        const char* msg = (g_app.winner == 1) ? "HOST WINS!" : "GUEST WINS!";
        int mw = MeasureText(msg, 60);
        DrawText(msg, SCREEN_W / 2 - mw / 2, SCREEN_H / 2 - 100, 60, YELLOW);

        if (g_app.role == pong::Role::Host) {
            const char* sub = "Press SPACE to play again";
            int sw = MeasureText(sub, 24);
            DrawText(sub, SCREEN_W / 2 - sw / 2, SCREEN_H / 2 - 20, 24, LIGHTGRAY);
        }

        if (draw_button("Main Menu", SCREEN_W / 2 - 100, SCREEN_H / 2 + 40, 200, 40)) {
            reset_app();
        }
    } else if (g_app.show_menu) {
        DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Color{0, 0, 0, 160});
        const char* msg = "PAUSED";
        int mw = MeasureText(msg, 40);
        DrawText(msg, SCREEN_W / 2 - mw / 2, SCREEN_H / 2 - 100, 40, WHITE);

        if (draw_button("Resume", SCREEN_W / 2 - 100, SCREEN_H / 2 - 20, 200, 40)) {
            g_app.show_menu = false;
        }
        if (draw_button("Main Menu", SCREEN_W / 2 - 100, SCREEN_H / 2 + 40, 200, 40)) {
            reset_app();
        }
    }
}

static void draw_lobby() {
    ClearBackground(BLACK);
    if (g_app.role == pong::Role::None) {
        DrawText("ONLINE PONG", SCREEN_W / 2 - 120, 160, 40, GREEN);

        if (draw_button("Host a Game", SCREEN_W / 2 - 100, 250, 200, 40)) {
            start_as_host();
        }
        if (draw_button("Join a Game", SCREEN_W / 2 - 100, 310, 200, 40)) {
            g_app.role = pong::Role::Guest;
        }

    } else {
        // Universal back button for lobby screens
        if (draw_button("< Back", 20, 20, 100, 40)) {
            reset_app();
            return; // State reset; exit drawing immediately
        }

        if (g_app.role == pong::Role::Host) {
            DrawText("Waiting for guest...", 240, 200, 28, WHITE);
            if (!g_app.lobby_code.empty()) {
                DrawText("Lobby code:", 280, 270, 24, GRAY);
                DrawTextEx(g_app.code_font, g_app.lobby_code.c_str(), {280, 306}, 40, 2, GREEN);
            }

        } else if (g_app.role == pong::Role::Guest) {
            if (g_app.connecting) {
                DrawText("Connecting...", 270, 260, 32, YELLOW);
            } else {
                DrawText("Enter lobby code:", 220, 200, 28, WHITE);
                DrawTextEx(g_app.code_font, g_app.join_code_input.c_str(), {280, 260}, 40, 2, GREEN);
                DrawText("Press ENTER to join", 240, 330, 20, GRAY);
            }
        }
    }
}

// Main loop
static void main_loop() {
    if (g_app.role != pong::Role::None && !g_app.peer_connected) {
        // Allow backing out to the main menu
        if (IsKeyPressed(KEY_ESCAPE)) {
            reset_app();
            return;
        }

        if (g_app.role == pong::Role::Guest && !g_app.connecting) {
            int key = GetCharPressed();
            while (key > 0) {
                if (g_app.join_code_input.size() < 6) {
                    // Normalize to uppercase to match server-generated codes
                    if (key >= 'a' && key <= 'z')
                        key -= 32;
                    g_app.join_code_input += static_cast<char>(key);
                }
                key = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && !g_app.join_code_input.empty())
                g_app.join_code_input.pop_back();
            if (IsKeyPressed(KEY_ENTER) && g_app.join_code_input.size() == 6) {
                g_app.connecting = true;
                start_as_guest(g_app.join_code_input);
            }
        }

    } else if (g_app.peer_connected) {
        if (g_app.game_over) {
            if (IsKeyPressed(KEY_SPACE) && g_app.role == pong::Role::Host) {
                g_app.sim = pong::SimState{};
                g_app.last_tx_state = pong::QuantState{};
                g_app.game_over = false;
                g_app.winner = 0;
            }
        } else {
            if (IsKeyPressed(KEY_ESCAPE))
                g_app.show_menu = !g_app.show_menu;

            if (g_app.role == pong::Role::Guest) {
                int8_t dir = 0;
                if (!g_app.show_menu) {
                    if (IsKeyDown(KEY_UP))
                        dir = -1;
                    if (IsKeyDown(KEY_DOWN))
                        dir =  1;
                }
                uint8_t buf[pong::INPUT_MAX_BYTES];
                int len = pong::encode_input(buf, g_app.sim.tick, dir, pong::sim_checksum(g_app.sim));
                g_app.transport->send({ buf, static_cast<size_t>(len) });
            }
            if (g_app.role == pong::Role::Host)
                game_tick();
        }
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
    SetExitKey(0);
    SetTargetFPS(TICK_HZ);
    g_app.code_font = LoadFontEx("Assets/Roboto-Regular.ttf", 40, 0, 250);

#ifdef PONG_WASM
    emscripten_set_main_loop(main_loop, TICK_HZ, 1);
#else
    while (!WindowShouldClose())
        main_loop();
    CloseWindow();
#endif
    UnloadFont(g_app.code_font);
    return 0;
}