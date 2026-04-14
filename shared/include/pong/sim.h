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

    // Schrödinger ball: spawned when ball crosses guest's paddle face without a
    // confirmed hit; lives until AuthCollisionMsg consensus or it exits the field.
    bool has_schrodinger = false;
    int32_t s_ball_x = 0;
    int32_t s_ball_y = 0;
    int32_t s_ball_vx = 0;
    int32_t s_ball_vy = 0;
    uint32_t schro_spawn_tick = 0; // tick at which schro was created
    int32_t schro_spawn_y = 0;    // ball_y at spawn (for guest hit-detection)
};

// Advance the simulation by one tick.
// dir_a / dir_b: -1 = up, 0 = none, +1 = down
inline void sim_tick(SimState& s, int8_t dir_a, int8_t dir_b) {
    // Move paddles
    s.paddle_a_y += dir_a * PADDLE_SPD;
    s.paddle_b_y += dir_b * PADDLE_SPD;

    // Clamp paddles
    if (s.paddle_a_y < 0) s.paddle_a_y = 0;
    if (s.paddle_a_y > FIELD_H - PADDLE_H) s.paddle_a_y = FIELD_H - PADDLE_H;
    if (s.paddle_b_y < 0) s.paddle_b_y = 0;
    if (s.paddle_b_y > FIELD_H - PADDLE_H) s.paddle_b_y = FIELD_H - PADDLE_H;

    static constexpr int32_t a_face = PADDLE_W;
    static constexpr int32_t b_face = FIELD_W - PADDLE_W - BALL_SIZE;

    // --- Real ball (frozen off-screen while a Schrödinger ball is active) ---
    if (!s.has_schrodinger) {
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

        // Paddle A (Host) — authoritative, no uncertainty
        if (prev_ball_x > a_face && s.ball_x <= a_face) {
            if (s.ball_y + BALL_SIZE >= s.paddle_a_y && s.ball_y <= s.paddle_a_y + PADDLE_H) {
                s.ball_x = a_face;
                s.ball_vx = -s.ball_vx;
            }
        }

        // Paddle B (Guest) — on a miss, spawn the Schrödinger ball instead of
        // immediately scoring; consensus is reached via AuthCollisionMsg.
        if (prev_ball_x < b_face && s.ball_x >= b_face) {
            if (s.ball_y + BALL_SIZE >= s.paddle_b_y && s.ball_y <= s.paddle_b_y + PADDLE_H) {
                s.ball_x = b_face;
                s.ball_vx = -s.ball_vx;
            } else {
                // Host sees a miss — birth the Schrödinger ball at the exact
                // crossing point (as if the guest HAD bounced it back).
                s.has_schrodinger = true;
                s.s_ball_x   = b_face;
                s.s_ball_y   = s.ball_y;
                s.s_ball_vx  = -s.ball_vx; // going left (bounced)
                s.s_ball_vy  = s.ball_vy;
                s.schro_spawn_tick = s.tick;
                s.schro_spawn_y    = s.ball_y;
                // Freeze real ball off-screen; it will be reset on resolution.
                s.ball_vx = 0;
                s.ball_vy = 0;
            }
        }

        // Host side exit — immediate authoritative score (no ambiguity)
        if (s.ball_x < 0) {
            s.score_b++;
            s.ball_x  = FIELD_W / 2;
            s.ball_y  = FIELD_H / 2;
            s.ball_vx = BALL_SPEED;
            s.ball_vy = BALL_SPEED;
        }
    }

    // --- Schrödinger ball physics (mirrors real ball; paddles can hit it) ---
    if (s.has_schrodinger) {
        int32_t prev_s_x = s.s_ball_x;
        s.s_ball_x += s.s_ball_vx;
        s.s_ball_y += s.s_ball_vy;

        // Wall bounces
        if (s.s_ball_y <= 0) {
            s.s_ball_y = 0;
            s.s_ball_vy = -s.s_ball_vy;
        }
        if (s.s_ball_y >= FIELD_H - BALL_SIZE) {
            s.s_ball_y = FIELD_H - BALL_SIZE;
            s.s_ball_vy = -s.s_ball_vy;
        }

        // Paddle A
        if (prev_s_x > a_face && s.s_ball_x <= a_face) {
            if (s.s_ball_y + BALL_SIZE >= s.paddle_a_y && s.s_ball_y <= s.paddle_a_y + PADDLE_H) {
                s.s_ball_x  = a_face;
                s.s_ball_vx = -s.s_ball_vx;
            }
        }

        // Paddle B
        if (prev_s_x < b_face && s.s_ball_x >= b_face) {
            if (s.s_ball_y + BALL_SIZE >= s.paddle_b_y && s.s_ball_y <= s.paddle_b_y + PADDLE_H) {
                s.s_ball_x  = b_face;
                s.s_ball_vx = -s.s_ball_vx;
            }
        }

        // Schrödinger ball exits — definitive goal; round resolves immediately
        if (s.s_ball_x < 0) {
            s.score_b++;
            s.has_schrodinger = false;
            s.ball_x  = FIELD_W / 2; s.ball_y  = FIELD_H / 2;
            s.ball_vx = BALL_SPEED;   s.ball_vy = BALL_SPEED;
        } else if (s.s_ball_x > FIELD_W) {
            s.score_a++;
            s.has_schrodinger = false;
            s.ball_x  = FIELD_W / 2; s.ball_y  = FIELD_H / 2;
            s.ball_vx = -BALL_SPEED;  s.ball_vy = BALL_SPEED;
        }
    }

    s.tick++;
}

// Called by the host when an AuthCollisionMsg arrives from the guest.
// did_hit=true  → guest actually hit; promote schro ball to real ball (no score).
// did_hit=false → guest confirms miss; award host the point and serve.
inline void resolve_schrodinger(SimState& s, bool did_hit) {
    if (!s.has_schrodinger) return;
    s.has_schrodinger = false;
    if (did_hit) {
        s.ball_x  = s.s_ball_x;
        s.ball_y  = s.s_ball_y;
        s.ball_vx = s.s_ball_vx;
        s.ball_vy = s.s_ball_vy;
    } else {
        s.score_a++;
        s.ball_x  = FIELD_W / 2; s.ball_y  = FIELD_H / 2;
        s.ball_vx = -BALL_SPEED;  s.ball_vy = BALL_SPEED;
    }
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