#pragma once
#include "app.h"
#include "config.h"
#include "raylib.h"

// Box rects for main-menu text inputs; shared between draw_lobby and main_loop.
static constexpr Rectangle USERNAME_BOX = { SCREEN_W - 252.f, 38.f, 242.f, 28.f };
static constexpr Rectangle SIGNALING_BOX = { SCREEN_W - 252.f, 98.f, 242.f, 28.f };

RenderState compute_render_state();
void draw_game(const RenderState& rs);
void draw_lobby();
