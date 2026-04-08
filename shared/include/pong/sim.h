#pragma once
#include "messages.h"
#include <cstdint>

namespace pong {

// Fixed-point constants
// All positions are in 1/100-pixel units (int16) for determinism
// across native and WASM builds.
static constexpr int FIELD_W = 80000; // 800px * 100
static constexpr int FIELD_H = 60000; // 600px * 100
static constexpr int PADDLE_H = 8000; // 80px * 100
static constexpr int PADDLE_W = 1200; // 12px * 100
static constexpr int BALL_SIZE = 1000; // 10px * 100
static constexpr int BALL_SPEED = 500; // 5px/tick * 100
static constexpr int PADDLE_SPD = 400; // 4px/tick * 100
static constexpr int WIN_SCORE = 7;

struct SimState {
    int32_t ball_x = FIELD_W / 2;
    int32_t ball_y = FIELD_H / 2;
    int32_t ball_vx = BALL_SPEED;
    int32_t ball_vy = BALL_SPEED;
    int32_t paddle_a_y = (FIELD_H - PADDLE_H) / 2; // host paddle
    int32_t paddle_b_y = (FIELD_H - PADDLE_H) / 2; // guest paddle
    uint8_t score_a = 0;
    uint8_t score_b = 0;
    uint32_t tick = 0;
};

// Advance the simulation by one tick.
// dir_a / dir_b: -1 = up, 0 = none, +1 = down
inline void sim_tick(SimState& s, int8_t dir_a, int8_t dir_b) {
    // Move paddles
    s.paddle_a_y += dir_a * PADDLE_SPD;
    s.paddle_b_y += dir_b * PADDLE_SPD;

    // Clamp paddles
    if (s.paddle_a_y < 0)
        s.paddle_a_y = 0;

    if (s.paddle_a_y > FIELD_H - PADDLE_H)
        s.paddle_a_y = FIELD_H - PADDLE_H;

    if (s.paddle_b_y < 0)
        s.paddle_b_y = 0;

    if (s.paddle_b_y > FIELD_H - PADDLE_H)
        s.paddle_b_y = FIELD_H - PADDLE_H;

    // Move ball
    s.ball_x += s.ball_vx;
    s.ball_y += s.ball_vy;

    // Top / bottom wall bounce
    if (s.ball_y <= 0) {
        s.ball_y = 0;
        s.ball_vy = -s.ball_vy;
    }
    if (s.ball_y >= FIELD_H - BALL_SIZE) {
        s.ball_y = FIELD_H - BALL_SIZE;
        s.ball_vy = -s.ball_vy;
    }

    // Paddle A (left) collision
    if (s.ball_x <= PADDLE_W &&
        s.ball_y + BALL_SIZE >= s.paddle_a_y &&
        s.ball_y <= s.paddle_a_y + PADDLE_H)
    {
        s.ball_x = PADDLE_W;
        s.ball_vx = -s.ball_vx;
    }

    // Paddle B (right) collision
    if (s.ball_x + BALL_SIZE >= FIELD_W - PADDLE_W &&
        s.ball_y + BALL_SIZE >= s.paddle_b_y &&
        s.ball_y <= s.paddle_b_y + PADDLE_H)
    {
        s.ball_x = FIELD_W - PADDLE_W - BALL_SIZE;
        s.ball_vx = -s.ball_vx;
    }

    // Scoring
    if (s.ball_x < 0) {
        s.score_b++;
        s.ball_x = FIELD_W / 2;
        s.ball_y = FIELD_H / 2;
        s.ball_vx = BALL_SPEED;
        s.ball_vy = BALL_SPEED;
    }
    if (s.ball_x > FIELD_W) {
        s.score_a++;
        s.ball_x = FIELD_W / 2;
        s.ball_y = FIELD_H / 2;
        s.ball_vx = -BALL_SPEED;
        s.ball_vy = BALL_SPEED;
    }

    s.tick++;
}

// Pack SimState into a GameStateMsg (delta_mask = DELTA_ALL for now)
inline GameStateMsg sim_to_msg(const SimState& s) {
    GameStateMsg msg{};
    msg.tick = s.tick;
    msg.ball_x = static_cast<int16_t>(s.ball_x / 100);
    msg.ball_y = static_cast<int16_t>(s.ball_y / 100);
    msg.ball_vx = static_cast<int16_t>(s.ball_vx / 100);
    msg.ball_vy = static_cast<int16_t>(s.ball_vy / 100);
    msg.paddle_a_y = static_cast<int16_t>(s.paddle_a_y / 100);
    msg.paddle_b_y = static_cast<int16_t>(s.paddle_b_y / 100);
    msg.score_a = s.score_a;
    msg.score_b = s.score_b;
    msg.delta_mask = DELTA_ALL;
    return msg;
}

// Unpack a GameStateMsg back into SimState
inline void msg_to_sim(const GameStateMsg& msg, SimState& s) {
    s.tick = msg.tick;
    s.ball_x = msg.ball_x * 100;
    s.ball_y = msg.ball_y * 100;
    s.ball_vx = msg.ball_vx * 100;
    s.ball_vy = msg.ball_vy * 100;
    s.paddle_a_y = msg.paddle_a_y * 100;
    s.paddle_b_y = msg.paddle_b_y * 100;
    s.score_a = msg.score_a;
    s.score_b = msg.score_b;
}

// Checksum over the fields that matter for desync detection
inline uint16_t sim_checksum(const SimState& s) {
    uint32_t h = s.tick;
    h ^= static_cast<uint32_t>(s.ball_x) << 1;
    h ^= static_cast<uint32_t>(s.ball_y) << 2;
    h ^= static_cast<uint32_t>(s.ball_vx) << 3;
    h ^= static_cast<uint32_t>(s.ball_vy) << 4;
    h ^= static_cast<uint32_t>(s.paddle_a_y) << 5;
    h ^= static_cast<uint32_t>(s.paddle_b_y) << 6;
    return static_cast<uint16_t>((h ^ (h >> 16)) & 0xFFFF);
}

} // namespace pong
