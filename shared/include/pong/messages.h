#pragma once
#include <cstdint>
#include <cstring>
#include <span>

namespace pong {

// ── Message type discriminator ────────────────────────────────
enum class MsgType : uint8_t {
    LobbyOffer  = 0x01,
    LobbyCode   = 0x02,
    Input       = 0x10,
    GameState   = 0x20,
    Reconcile   = 0x21,
    Ping        = 0x30,
    Pong        = 0x31,
    Disconnect  = 0xFF,
};

enum class DisconnectReason : uint8_t {
    Graceful = 0,
    Timeout  = 1,
    Desync   = 2,
};

// delta_mask bits for GameStateMsg — set means field changed vs last broadcast
enum DeltaBit : uint8_t {
    DELTA_BALL_X     = 1 << 0,
    DELTA_BALL_Y     = 1 << 1,
    DELTA_BALL_VX    = 1 << 2,
    DELTA_BALL_VY    = 1 << 3,
    DELTA_PADDLE_A   = 1 << 4,
    DELTA_PADDLE_B   = 1 << 5,
    DELTA_SCORE      = 1 << 6,
    DELTA_ALL        = 0x7F,
};

// ── All structs are 1-byte packed little-endian ───────────────
#pragma pack(push, 1)

// Client → Signaling Server
// Variable-length; sdp and ice fields are UTF-8 strings.
// Not used at runtime gameplay — signaling only.
struct LobbyOfferMsg {
    uint8_t  msg_id = static_cast<uint8_t>(MsgType::LobbyOffer);
    uint16_t sdp_len;
    // uint8_t sdp_payload[sdp_len]  — follows in buffer
    // uint16_t ice_len              — follows sdp_payload
    // uint8_t ice_candidates[...]   — follows ice_len
};

// Signaling Server → Client A
struct LobbyCodeMsg {
    uint8_t msg_id = static_cast<uint8_t>(MsgType::LobbyCode);
    char    code[6];  // ASCII alphanumeric, null-terminated not required
};
static_assert(sizeof(LobbyCodeMsg) == 7);

// Client → Game Server — sent every tick (unreliable, unordered)
struct InputMsg {
    uint8_t  msg_id   = static_cast<uint8_t>(MsgType::Input);
    uint32_t tick;
    int8_t   dir;      // -1 = up, 0 = none, +1 = down
    uint16_t checksum; // CRC-16 of client's predicted game state
};
static_assert(sizeof(InputMsg) == 8);

// Game Server → Both Clients — sent every 2 ticks (~33 ms)
// All positions in fixed-point 1/100-pixel units (int16) for determinism.
// Fields absent from delta are omitted from the wire; delta_mask says which are present.
struct GameStateMsg {
    uint8_t  msg_id    = static_cast<uint8_t>(MsgType::GameState);
    uint32_t tick;
    int16_t  ball_x,   ball_y;
    int16_t  ball_vx,  ball_vy;
    int16_t  paddle_a_y;
    int16_t  paddle_b_y;
    uint8_t  score_a,  score_b;
    uint8_t  delta_mask;  // which fields changed vs last broadcast
};
static_assert(sizeof(GameStateMsg) == 20);

// Game Server → Client — only when checksum mismatch detected
struct ReconcileMsg {
    uint8_t  msg_id = static_cast<uint8_t>(MsgType::Reconcile);
    uint32_t tick;
    int16_t  ball_x,    ball_y;
    int16_t  ball_vx,   ball_vy;
    int16_t  paddle_a_y;
    int16_t  paddle_b_y;
};
static_assert(sizeof(ReconcileMsg) == 17);

// Client → Server (Ping) / Server → Client (Pong) — every 500 ms
struct PingMsg {
    uint8_t  msg_id    = static_cast<uint8_t>(MsgType::Ping);
    uint32_t seq;
    uint32_t client_ts; // ms timestamp, echoed back in Pong
};
static_assert(sizeof(PingMsg) == 9);

struct PongMsg {
    uint8_t  msg_id    = static_cast<uint8_t>(MsgType::Pong);
    uint32_t seq;
    uint32_t client_ts;
};
static_assert(sizeof(PongMsg) == 9);

// Bidirectional — graceful shutdown or error
struct DisconnectMsg {
    uint8_t msg_id  = static_cast<uint8_t>(MsgType::Disconnect);
    uint8_t reason;
};
static_assert(sizeof(DisconnectMsg) == 2);

#pragma pack(pop)

// ── Helpers ───────────────────────────────────────────────────

// Read the discriminator byte from a raw buffer without copying.
inline MsgType peek_type(std::span<const uint8_t> buf) {
    return static_cast<MsgType>(buf[0]);
}

// Safe cast from raw buffer to a fixed-size message type.
template<typename T>
inline const T* msg_cast(std::span<const uint8_t> buf) {
    if (buf.size() < sizeof(T)) return nullptr;
    return reinterpret_cast<const T*>(buf.data());
}

} // namespace pong
