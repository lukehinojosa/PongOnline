#pragma once

// Screen / layout
static constexpr int SCREEN_W = 800;
static constexpr int SCREEN_H = 660;  // 600px game field + 60px HUD
static constexpr int HUD_H = 60;

// Simulation
static constexpr int TICK_HZ = 60;

// Interpolation / snapshot buffer
static constexpr uint32_t INTERP_DELAY_TICKS = 6;
static constexpr int SNAPSHOT_BUF_MAX = 32;

// Networking
static constexpr double PING_INTERVAL_MS = 500.0;
static constexpr float RTT_EWMA_ALPHA = 0.125f;
