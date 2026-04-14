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
    int32_t ball_vx = 0;
    int32_t ball_vy = 0;
    int32_t paddle_a_y = (FIELD_H - PADDLE_H) / 2; // host paddle
    int32_t paddle_b_y = (FIELD_H - PADDLE_H) / 2; // guest paddle
    uint8_t score_a = 0;
    uint8_t score_b = 0;
    uint32_t tick = 0;
    uint32_t serve_tick = 60;

    // Schrödinger ball: spawned when the ball crosses either paddle face.
    // The real ball optimistically bounces; the schro ball continues in the
    // scoring direction. Cleared by resolve_schrodinger() on AuthCollision.
    bool has_schrodinger = false;
    int32_t s_ball_x = 0;
    int32_t s_ball_y = 0;
    int32_t s_ball_vx = 0;
    int32_t s_ball_vy = 0;
    uint32_t schro_spawn_tick = 0; // tick at which schro was created
    int32_t schro_spawn_y = 0;    // ball_y at spawn (for hit-detection)
    uint8_t schro_side = 0;       // 0 = Host paddle, 1 = Guest paddle

    bool has_pending_auth = false;
    uint32_t pending_auth_tick = 0;
    uint8_t pending_auth_did_hit = 0;
    uint8_t pending_auth_side = 0;
};

// Deterministic ball serve direction based on total goals scored.
// Even total → serve right (toward guest), odd total → serve left (toward host).
inline int32_t serve_vx(const SimState& s) {
    return ((s.score_a + s.score_b) % 2 == 0) ? BALL_SPEED : -BALL_SPEED;
}

    // Called by either client when an AuthCollisionMsg arrives.
    //   side    — which paddle this resolves (0 = Host, 1 = Guest)
    //   did_hit — true if the authoritative side confirms a hit
    //
    // did_hit=true  → real ball already on the optimistic bounce path; just clear schro.
    // did_hit=false → the paddle missed; award the point to the other side and
    //                 reset the ball with a deterministic serve direction.
    inline void resolve_schrodinger(SimState& s, bool did_hit, uint8_t side) {
    if (!s.has_schrodinger) return;

    if (did_hit) {
        // Successful block: Delete the ghost timeline.
        // The real ball is already optimistically bouncing away safely.
        s.has_schrodinger = false;
    } else {
        // Missed!: The ghost timeline is the true reality.
        // Swap the ghost ball into the real ball's place.
        s.ball_x = s.s_ball_x;
        s.ball_y = s.s_ball_y;
        s.ball_vx = s.s_ball_vx;
        s.ball_vy = s.s_ball_vy;

        s.has_schrodinger = false;
    }

    // Clear schro state variables
    s.s_ball_x = 0; s.s_ball_y = 0;
    s.s_ball_vx = 0; s.s_ball_vy = 0;
}

// Advance the simulation by one tick.
// dir_a / dir_b: -1 = up, 0 = none, +1 = down
// In dual-auth mode: pass 0 for the remote paddle; its position is kept
// current via direct assignment from received PaddleState messages.
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

    // --- Check Serve Cooldown ---
    if (s.serve_tick > 0) {
        if (s.tick >= s.serve_tick) {
            // Unfreeze the ball
            s.ball_vx = serve_vx(s);
            s.ball_vy = BALL_SPEED;

            // Catch up missed ticks if the unfreeze happened late due to latency
            uint32_t missed = s.tick - s.serve_tick;
            for (uint32_t i = 0; i < missed; ++i) {
                s.ball_x += s.ball_vx;
                s.ball_y += s.ball_vy;

                // Process wall bounces during catch-up
                if (s.ball_y <= 0) {
                    s.ball_y = 0;
                    s.ball_vy = -s.ball_vy;
                }
                if (s.ball_y >= FIELD_H - BALL_SIZE) {
                    s.ball_y = FIELD_H - BALL_SIZE;
                    s.ball_vy = -s.ball_vy;
                }
            }

            s.serve_tick = 0; // Reset cooldown state
        } else {
            // Keep the ball explicitly frozen while waiting
            s.ball_vx = 0;
            s.ball_vy = 0;
        }
    }

    // --- Real ball (ALWAYS UPDATES — optimistic prediction) ---
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

    // Paddle A (Host) — symmetric Schrödinger spawning.
    // The host is authoritative for this face; it sends AuthCollisionMsg.
    if (prev_ball_x > a_face && s.ball_x <= a_face) {
        if (!s.has_schrodinger) {
            s.has_schrodinger  = true;
            s.schro_side       = 0; // Host's paddle
            s.s_ball_x         = s.ball_x;
            s.s_ball_y         = s.ball_y;
            s.s_ball_vx        = s.ball_vx; // continues left (guest scores)
            s.s_ball_vy        = s.ball_vy;
            s.schro_spawn_tick = s.tick;
            s.schro_spawn_y    = s.ball_y;

            // Real ball optimistically bounces right
            s.ball_x  = a_face;
            s.ball_vx = -s.ball_vx;

            if (s.has_pending_auth && s.pending_auth_tick == s.tick) {
                resolve_schrodinger(s, s.pending_auth_did_hit != 0, s.pending_auth_side);
                s.has_pending_auth = false;
            }
        }
    }

    // Paddle B (Guest) — symmetric Schrödinger spawning.
    // The guest is authoritative for this face; it sends AuthCollisionMsg.
    if (prev_ball_x < b_face && s.ball_x >= b_face) {
        if (!s.has_schrodinger) {
            s.has_schrodinger  = true;
            s.schro_side       = 1; // Guest's paddle
            s.s_ball_x         = s.ball_x;
            s.s_ball_y         = s.ball_y;
            s.s_ball_vx        = s.ball_vx; // continues right (host scores)
            s.s_ball_vy        = s.ball_vy;
            s.schro_spawn_tick = s.tick;
            s.schro_spawn_y    = s.ball_y;

            // Real ball optimistically bounces left
            s.ball_x  = b_face;
            s.ball_vx = -s.ball_vx;

            if (s.has_pending_auth && s.pending_auth_tick == s.tick) {
                resolve_schrodinger(s, s.pending_auth_did_hit != 0, s.pending_auth_side);
                s.has_pending_auth = false;
            }
        }
    }

    // Safety exits act as goal triggers
    if (s.ball_x < 0) {
        s.score_b++;
        s.ball_x  = FIELD_W / 2;
        s.ball_y  = FIELD_H / 2;
        s.ball_vx = 0;
        s.ball_vy = 0;
        s.serve_tick = s.schro_spawn_tick + 60;
    }
    if (s.ball_x > FIELD_W) {
        s.score_a++;
        s.ball_x  = FIELD_W / 2;
        s.ball_y  = FIELD_H / 2;
        s.ball_vx = 0;
        s.ball_vy = 0;
        s.serve_tick = s.schro_spawn_tick + 60;
    }

    // --- Schrödinger ball physics (scoring timeline) ---
    if (s.has_schrodinger) {
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

        // Park once it exits the goal side
        if (s.s_ball_x < -BALL_SIZE) {
            s.s_ball_x  = -BALL_SIZE - 1;
            s.s_ball_vx = 0;
            s.s_ball_vy = 0;
        }
        if (s.s_ball_x > FIELD_W) {
            s.s_ball_x  = FIELD_W + 1;
            s.s_ball_vx = 0;
            s.s_ball_vy = 0;
        }
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
