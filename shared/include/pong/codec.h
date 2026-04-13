#pragma once
#include "messages.h"
#include "sim.h"
#include <cstdint>
#include <span>
#include <string>

namespace pong {

// Zig-zag encode: maps signed ints to unsigned so small |n| → small value.
// Avoids the sign-extension overhead that would make VLQ inefficient for negatives.
inline uint32_t zz_enc(int32_t n) {
    return static_cast<uint32_t>((n << 1) ^ (n >> 31));
}
inline int32_t zz_dec(uint32_t n) {
    return static_cast<int32_t>((n >> 1) ^ (0u - (n & 1)));
}

// Variable-length quantity: 7 data bits per byte, MSB = more bytes follow.
inline int vlq_write(uint8_t* buf, uint32_t v) {
    int n = 0;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        buf[n++] = b | (v ? 0x80u : 0u);
    } while (v);
    return n;
}
inline uint32_t vlq_read(const uint8_t*& p, const uint8_t* end) {
    uint32_t v = 0;
    int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        v |= static_cast<uint32_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}

// Pixel-quantized state (1 unit = 1 pixel) used for delta encoding.
// The sim works in 1/100-pixel units; we quantize down to pixel precision for the wire.
struct QuantState {
    int32_t ball_x = 0;
    int32_t ball_y = 0;
    int32_t ball_vx = 0;
    int32_t ball_vy = 0;
    int32_t paddle_a_y = 0;
    int32_t paddle_b_y = 0;
    uint8_t score_a = 0;
    uint8_t score_b = 0;
    uint32_t tick = 0;
};

inline QuantState sim_quantize(const SimState& s) {
    QuantState q;
    q.ball_x = s.ball_x / 100;
    q.ball_y = s.ball_y / 100;
    q.ball_vx = s.ball_vx / 100;
    q.ball_vy = s.ball_vy / 100;
    q.paddle_a_y = s.paddle_a_y / 100;
    q.paddle_b_y = s.paddle_b_y / 100;
    q.score_a = s.score_a;
    q.score_b = s.score_b;
    q.tick = s.tick;
    return q;
}

// Returns a delta_mask of which fields changed between two quantized states.
inline uint8_t compute_delta_mask(const QuantState& prev, const QuantState& cur) {
    uint8_t m = 0;
    if (cur.ball_x != prev.ball_x) m |= DELTA_BALL_X;
    if (cur.ball_y != prev.ball_y) m |= DELTA_BALL_Y;
    if (cur.ball_vx != prev.ball_vx) m |= DELTA_BALL_VX;
    if (cur.ball_vy != prev.ball_vy) m |= DELTA_BALL_VY;
    if (cur.paddle_a_y != prev.paddle_a_y) m |= DELTA_PADDLE_A;
    if (cur.paddle_b_y != prev.paddle_b_y) m |= DELTA_PADDLE_B;
    if (cur.score_a != prev.score_a || cur.score_b != prev.score_b) m |= DELTA_SCORE;
    return m;
}

// GameState wire format (variable length, max ~24 bytes):
//   msg_id   (1)     — MsgType::GameState = 0x20
//   tick     (VLQ)   — absolute tick
//   dmask    (1)     — DeltaBit flags; only present fields follow
//   per set bit, ascending order:
//     BALL_X    — zz-VLQ delta from prev.ball_x
//     BALL_Y    — zz-VLQ delta from prev.ball_y
//     BALL_VX   — zz-VLQ absolute (velocity flips on bounce; delta adds no gain)
//     BALL_VY   — zz-VLQ absolute
//     PADDLE_A  — zz-VLQ delta from prev.paddle_a_y
//     PADDLE_B  — zz-VLQ delta from prev.paddle_b_y
//     SCORE     — 1 byte packed: (score_a << 4) | score_b  (max 7 each → fits 4 bits)
static constexpr int GAMESTATE_MAX_BYTES = 24;

inline int encode_game_state(uint8_t* buf, const SimState& cur,
                              const QuantState& prev, uint8_t dmask) {
    QuantState q = sim_quantize(cur);
    uint8_t* p = buf;
    *p++ = static_cast<uint8_t>(MsgType::GameState);
    p += vlq_write(p, q.tick);
    *p++ = dmask;
    if (dmask & DELTA_BALL_X)
        p += vlq_write(p, zz_enc(q.ball_x - prev.ball_x));
    if (dmask & DELTA_BALL_Y)
        p += vlq_write(p, zz_enc(q.ball_y - prev.ball_y));
    if (dmask & DELTA_BALL_VX)
        p += vlq_write(p, zz_enc(q.ball_vx));
    if (dmask & DELTA_BALL_VY)
        p += vlq_write(p, zz_enc(q.ball_vy));
    if (dmask & DELTA_PADDLE_A)
        p += vlq_write(p, zz_enc(q.paddle_a_y - prev.paddle_a_y));
    if (dmask & DELTA_PADDLE_B)
        p += vlq_write(p, zz_enc(q.paddle_b_y - prev.paddle_b_y));
    if (dmask & DELTA_SCORE)
        *p++ = static_cast<uint8_t>((q.score_a << 4) | (q.score_b & 0x0F));
    return static_cast<int>(p - buf);
}

// Decode a game state message; updates prev with the decoded quantized values.
inline bool decode_game_state(std::span<const uint8_t> buf, SimState& sim, QuantState& prev) {
    if (buf.size() < 3 || buf[0] != static_cast<uint8_t>(MsgType::GameState))
        return false;
    const uint8_t* p = buf.data() + 1;
    const uint8_t* end = buf.data() + buf.size();

    uint32_t tick = vlq_read(p, end);
    if (p >= end) return false;
    uint8_t dmask = *p++;

    QuantState q = prev;
    q.tick = tick;

    if (dmask & DELTA_BALL_X)
        q.ball_x = prev.ball_x + zz_dec(vlq_read(p, end));
    if (dmask & DELTA_BALL_Y)
        q.ball_y = prev.ball_y + zz_dec(vlq_read(p, end));
    if (dmask & DELTA_BALL_VX)
        q.ball_vx = zz_dec(vlq_read(p, end));
    if (dmask & DELTA_BALL_VY)
        q.ball_vy = zz_dec(vlq_read(p, end));
    if (dmask & DELTA_PADDLE_A)
        q.paddle_a_y = prev.paddle_a_y + zz_dec(vlq_read(p, end));
    if (dmask & DELTA_PADDLE_B)
        q.paddle_b_y = prev.paddle_b_y + zz_dec(vlq_read(p, end));
    if (dmask & DELTA_SCORE) {
        if (p >= end) return false;
        uint8_t sc = *p++;
        q.score_a = sc >> 4;
        q.score_b = sc & 0x0F;
    }

    prev = q;
    sim.tick = q.tick;
    sim.ball_x = q.ball_x * 100;
    sim.ball_y = q.ball_y * 100;
    sim.ball_vx = q.ball_vx * 100;
    sim.ball_vy = q.ball_vy * 100;
    sim.paddle_a_y = q.paddle_a_y * 100;
    sim.paddle_b_y = q.paddle_b_y * 100;
    sim.score_a = q.score_a;
    sim.score_b = q.score_b;
    return true;
}

// Input wire format (variable length, max 9 bytes):
//   msg_id     (1)    — MsgType::Input = 0x10
//   tick       (VLQ)  — absolute tick
//   dir        (VLQ)  — zz-encoded int8: -1/0/+1 → 1/0/2
//   checksum   (2 LE) — CRC-16, raw bytes (not compressible)
static constexpr int INPUT_MAX_BYTES = 9;

inline int encode_input(uint8_t* buf, uint32_t tick, int8_t dir, uint16_t checksum) {
    uint8_t* p = buf;
    *p++ = static_cast<uint8_t>(MsgType::Input);
    p += vlq_write(p, tick);
    p += vlq_write(p, zz_enc(static_cast<int32_t>(dir)));
    p[0] = checksum & 0xFF;
    p[1] = (checksum >> 8) & 0xFF;
    p += 2;
    return static_cast<int>(p - buf);
}

struct DecodedInput {
    uint32_t tick = 0;
    int8_t dir = 0;
    uint16_t checksum = 0;
};

inline bool decode_input(std::span<const uint8_t> buf, DecodedInput& out) {
    if (buf.empty() || buf[0] != static_cast<uint8_t>(MsgType::Input))
        return false;
    const uint8_t* p = buf.data() + 1;
    const uint8_t* end = buf.data() + buf.size();
    out.tick = vlq_read(p, end);
    out.dir = static_cast<int8_t>(zz_dec(vlq_read(p, end)));
    if (p + 2 > end) return false;
    out.checksum = static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
    return true;
}

// Ping / Pong codec
// Wire format: raw packed struct (9 bytes each).
static constexpr int PING_BYTES = 9;
static constexpr int PONG_BYTES = 9;

inline int encode_ping(uint8_t* buf, uint32_t seq, uint32_t client_ts) {
    PingMsg m;
    m.seq = seq;
    m.client_ts = client_ts;
    std::memcpy(buf, &m, sizeof(m));
    return sizeof(m);
}

inline bool decode_ping(std::span<const uint8_t> buf, uint32_t& seq, uint32_t& client_ts) {
    if (buf.size() < sizeof(PingMsg) || buf[0] != static_cast<uint8_t>(MsgType::Ping))
        return false;
    PingMsg m;
    std::memcpy(&m, buf.data(), sizeof(m));
    seq = m.seq;
    client_ts = m.client_ts;
    return true;
}

inline int encode_pong(uint8_t* buf, uint32_t seq, uint32_t client_ts) {
    PongMsg m;
    m.seq = seq;
    m.client_ts = client_ts;
    std::memcpy(buf, &m, sizeof(m));
    return sizeof(m);
}

inline bool decode_pong(std::span<const uint8_t> buf, uint32_t& seq, uint32_t& client_ts) {
    if (buf.size() < sizeof(PongMsg) || buf[0] != static_cast<uint8_t>(MsgType::Pong))
        return false;
    PongMsg m;
    std::memcpy(&m, buf.data(), sizeof(m));
    seq = m.seq;
    client_ts = m.client_ts;
    return true;
}

// Username wire format: 1 type byte + 1 length byte + up to 31 UTF-8 chars.
static constexpr int USERNAME_MAX_LEN   = 31;
static constexpr int USERNAME_MAX_BYTES = 2 + USERNAME_MAX_LEN;

inline int encode_username(uint8_t* buf, const std::string& name) {
    uint8_t* p = buf;
    *p++ = static_cast<uint8_t>(MsgType::Username);
    uint8_t len = static_cast<uint8_t>(std::min(name.size(), static_cast<size_t>(USERNAME_MAX_LEN)));
    *p++ = len;
    std::memcpy(p, name.data(), len);
    return 2 + len;
}

inline bool decode_username(std::span<const uint8_t> buf, std::string& out) {
    if (buf.size() < 2 || buf[0] != static_cast<uint8_t>(MsgType::Username))
        return false;
    uint8_t len = buf[1];
    if (buf.size() < static_cast<size_t>(2 + len))
        return false;
    out.assign(reinterpret_cast<const char*>(buf.data() + 2), len);
    return true;
}

} // namespace pong
