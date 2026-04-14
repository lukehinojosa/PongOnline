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
    int32_t prev_ball_x = s.ball_x;
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

    // --- STRICT FACE COLLISIONS ---
    int32_t a_face = PADDLE_W;
    int32_t b_face = FIELD_W - PADDLE_W - BALL_SIZE;

    // Paddle A (Host) Collision Check
    if (prev_ball_x > a_face && s.ball_x <= a_face) {
        // Ball crossed the threshold this tick. Did it hit the paddle?
        if (s.ball_y + BALL_SIZE >= s.paddle_a_y && s.ball_y <= s.paddle_a_y + PADDLE_H) {
            s.ball_x = a_face; // Snap to the exact bounce coordinate
            s.ball_vx = -s.ball_vx;
        }
    }

    // Paddle B (Guest) Collision Check
    if (prev_ball_x < b_face && s.ball_x >= b_face) {
        if (s.ball_y + BALL_SIZE >= s.paddle_b_y && s.ball_y <= s.paddle_b_y + PADDLE_H) {
            s.ball_x = b_face; // Snap to exact bounce coordinate
            s.ball_vx = -s.ball_vx;
        }
    }

    // Goal detection: ball exits the field, score and reset.
    // Serve toward the player who just scored so they see it approaching.
    if (s.ball_x < 0) {
        // Host missed — guest scores
        s.score_b++;
        s.ball_x  = FIELD_W / 2;
        s.ball_y  = FIELD_H / 2;
        s.ball_vx = BALL_SPEED;   // serves rightward, toward guest
        s.ball_vy = BALL_SPEED;
    } else if (s.ball_x > FIELD_W) {
        // Guest missed — host scores
        s.score_a++;
        s.ball_x  = FIELD_W / 2;
        s.ball_y  = FIELD_H / 2;
        s.ball_vx = -BALL_SPEED;  // serves leftward, toward host
        s.ball_vy = BALL_SPEED;
    }

    s.tick++;
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