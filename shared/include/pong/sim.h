#pragma once
#include "messages.h"
#include <cstdint>

namespace pong {

// Fixed-point constants
// All positions are in 1/100-pixel units (int16) for determinism
// across native and WASM builds.
static constexpr int FIELD_W = 80000; // 800px * 100
static constexpr int FIELD_H = 60000; // 600px * 100
static constexpr int PADDLE_H = 5000; // 50px * 100
static constexpr int PADDLE_W = 1200; // 12px * 100
static constexpr int BALL_SIZE = 1000; // 10px * 100
static constexpr int BALL_SPEED = 500; // 5px/tick * 100
static constexpr int PADDLE_SPD = 400; // 4px/tick * 100
static constexpr int WIN_SCORE = 7;
static constexpr int BALL_SPEED_DIAG = 353; // 500 * sqrt(2)/2 for 45-degree angles

inline uint8_t get_hit_type(int32_t spawn_y, int32_t paddle_y) {
    int32_t center_y = spawn_y + BALL_SIZE / 2;
    int32_t p_top = paddle_y;
    int32_t p_bot = paddle_y + PADDLE_H;

    // 0 = miss, 1 = up, 2 = mid, 3 = down
    if (spawn_y + BALL_SIZE < p_top || spawn_y > p_bot) return 0;

    int32_t third = PADDLE_H / 3;
    if (center_y < p_top + third) return 1;
    if (center_y > p_bot - third) return 3;
    return 2;
}

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
    uint8_t opt_hit_type = 0;
    int32_t s_x[4] = {0};
    int32_t s_y[4] = {0};
    int32_t s_vx[4] = {0};
    int32_t s_vy[4] = {0};
    uint32_t schro_spawn_tick = 0; // tick at which schro was created
    int32_t schro_spawn_y = 0;    // ball_y at spawn (for hit-detection)
    uint8_t schro_side = 0;       // 0 = Host paddle, 1 = Guest paddle

    bool has_pending_auth = false;
    uint32_t pending_auth_tick = 0;
    uint8_t pending_auth_hit_type = 0;
    uint8_t pending_auth_side = 0;

    // PRNG and Serve State
    uint32_t rng_state = 12345;
    int8_t last_scored_on = 0; // 0 = host, 1 = guest

    // Simple deterministic LCG
    uint32_t next_rand() {
        rng_state = (rng_state * 1103515245 + 12345) & 0x7fffffff;
        return rng_state;
    }
};

// Deterministic ball serve direction and position based on random seed and whoever scored last
    inline void reset_sim(SimState& s, uint32_t seed) {
        s = SimState{}; // Clear state
        s.serve_tick = 180;
        s.rng_state = seed;
        s.last_scored_on = s.next_rand() % 2; // Random first server

        // Randomize initial Y position
        s.ball_x = FIELD_W / 2;
        int32_t min_y = 5000; // 50px padding from the walls
        int32_t max_y = FIELD_H - 5000;
        s.ball_y = min_y + (s.next_rand() % (max_y - min_y));
    }

// Called by either client when an AuthCollisionMsg arrives.
inline void resolve_schrodinger(SimState& s, uint8_t actual_hit_type, uint8_t side) {
    if (!s.has_schrodinger) return;

    // Always snap the real ball to the authoritative timeline.
    s.ball_x = s.s_x[actual_hit_type];
    s.ball_y = s.s_y[actual_hit_type];
    s.ball_vx = s.s_vx[actual_hit_type];
    s.ball_vy = s.s_vy[actual_hit_type];

    s.has_schrodinger = false;
    for(int i = 0; i < 4; i++) {
        s.s_x[i] = 0; s.s_y[i] = 0;
        s.s_vx[i] = 0; s.s_vy[i] = 0;
    }
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
            // Serve diagonally to the player who was scored on
            s.ball_vx = (s.last_scored_on == 0) ? -BALL_SPEED_DIAG : BALL_SPEED_DIAG;
            s.ball_vy = (s.next_rand() % 2 == 0) ? BALL_SPEED_DIAG : -BALL_SPEED_DIAG;

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

    // Real ball always updates; optimistic prediction
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
            s.schro_side       = 0;
            s.schro_spawn_tick = s.tick;
            s.schro_spawn_y    = s.ball_y;

            // 0: Miss Timeline
            s.s_x[0] = s.ball_x;   s.s_y[0] = s.ball_y;
            s.s_vx[0] = s.ball_vx; s.s_vy[0] = s.ball_vy;

            // 1-3: Bounce Timelines
            for(int i = 1; i <= 3; i++) { s.s_x[i] = a_face; s.s_y[i] = s.ball_y; }
            s.s_vx[1] = BALL_SPEED_DIAG; s.s_vy[1] = -BALL_SPEED_DIAG; // Up
            s.s_vx[2] = BALL_SPEED;      s.s_vy[2] = 0;                // Mid
            s.s_vx[3] = BALL_SPEED_DIAG; s.s_vy[3] = BALL_SPEED_DIAG;  // Down

            // Optimistically lock the main ball to the locally predicted timeline
            s.opt_hit_type = 2;
            s.ball_x = s.s_x[s.opt_hit_type];   s.ball_y = s.s_y[s.opt_hit_type];
            s.ball_vx = s.s_vx[s.opt_hit_type]; s.ball_vy = s.s_vy[s.opt_hit_type];

            if (s.has_pending_auth && s.pending_auth_side == s.schro_side) {
                resolve_schrodinger(s, s.pending_auth_hit_type, s.pending_auth_side);
                s.has_pending_auth = false;
            }
        }
    }

    // Paddle B (Guest) — symmetric Schrödinger spawning.
    // The guest is authoritative for this face; it sends AuthCollisionMsg.
    if (prev_ball_x < b_face && s.ball_x >= b_face) {
        if (!s.has_schrodinger) {
            s.has_schrodinger  = true;
            s.schro_side       = 1;
            s.schro_spawn_tick = s.tick;
            s.schro_spawn_y    = s.ball_y;

            // 0: Miss Timeline
            s.s_x[0] = s.ball_x;   s.s_y[0] = s.ball_y;
            s.s_vx[0] = s.ball_vx; s.s_vy[0] = s.ball_vy;

            // 1-3: Bounce Timelines
            for(int i = 1; i <= 3; i++) { s.s_x[i] = b_face; s.s_y[i] = s.ball_y; }
            s.s_vx[1] = -BALL_SPEED_DIAG; s.s_vy[1] = -BALL_SPEED_DIAG; // Up
            s.s_vx[2] = -BALL_SPEED;      s.s_vy[2] = 0;                // Mid
            s.s_vx[3] = -BALL_SPEED_DIAG; s.s_vy[3] = BALL_SPEED_DIAG;  // Down

            s.opt_hit_type = get_hit_type(s.schro_spawn_y, s.paddle_b_y);
            s.ball_x = s.s_x[s.opt_hit_type];   s.ball_y = s.s_y[s.opt_hit_type];
            s.ball_vx = s.s_vx[s.opt_hit_type]; s.ball_vy = s.s_vy[s.opt_hit_type];

            if (s.has_pending_auth && s.pending_auth_side == s.schro_side) {
                resolve_schrodinger(s, s.pending_auth_hit_type, s.pending_auth_side);
                s.has_pending_auth = false;
            }
        }
    }

    // --- Schrödinger ball physics (scoring timeline) ---
    if (s.has_schrodinger) {
        for(int i = 0; i < 4; i++) {
            s.s_x[i] += s.s_vx[i];
            s.s_y[i] += s.s_vy[i];

            if (s.s_y[i] <= 0) {s.s_y[i] = 0; s.s_vy[i] = -s.s_vy[i]; }
            if (s.s_y[i] >= FIELD_H - BALL_SIZE) { s.s_y[i] = FIELD_H - BALL_SIZE; s.s_vy[i] = -s.s_vy[i]; }

            // No horizontal clamping and zeroing.
            // The ghost ball needs to retain its velocity and true overshoot distance
            // to accurately calculate retrospective goal timers.
        }
    }

    // Wrap the safety exits so goals cannot be scored while the timeline is disputed
    if (!s.has_schrodinger) {
        // Safety exits act as goal triggers
        if (s.ball_x < 0) {
            s.score_b++;
            s.last_scored_on = 0; // Host got scored on

            // Calculate how many ticks ago the ball actually crossed the line
            int32_t overdue_ticks = (0 - s.ball_x) / (-s.ball_vx);

            s.ball_x = FIELD_W / 2;
            int32_t min_y = 5000;
            int32_t max_y = FIELD_H - 5000;
            s.ball_y = min_y + (s.next_rand() % (max_y - min_y));
            s.ball_vx = 0; s.ball_vy = 0;
            s.serve_tick = s.tick + 60 - overdue_ticks;
        }
        if (s.ball_x > FIELD_W) {
            s.score_a++;
            s.last_scored_on = 1; // Guest got scored on

            // Calculate how many ticks ago the ball actually crossed the line
            int32_t overdue_ticks = (s.ball_x - FIELD_W) / s.ball_vx;

            s.ball_x = FIELD_W / 2;
            int32_t min_y = 5000;
            int32_t max_y = FIELD_H - 5000;
            s.ball_y = min_y + (s.next_rand() % (max_y - min_y));
            s.ball_vx = 0; s.ball_vy = 0;
            s.serve_tick = s.tick + 60 - overdue_ticks;
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
