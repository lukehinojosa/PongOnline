#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace pong {

// Abstract peer transport

struct Transport {
    // Called when the DataChannel is open and ready to send.
    std::function<void()> on_open;

    // Called when a binary message arrives from the peer.
    std::function<void(std::span<const uint8_t>)> on_message;

    // Called when the peer disconnects or the channel closes.
    std::function<void()> on_close;

    // Called when a lobby code is ready to display (host only).
    std::function<void(const std::string& code)> on_lobby_code;

    virtual ~Transport() = default;

    // Host: connect to signaling server and request a lobby code.
    virtual void host(const std::string& signaling_url) = 0;

    // Guest: connect to signaling server and join with the given code.
    virtual void join(const std::string& signaling_url, const std::string& code) = 0;

    // Send raw bytes to the peer. Call only after on_open fires.
    virtual void send(std::span<const uint8_t> data) = 0;

    // Keep alive while waiting for guest to arrive
    virtual void send_signaling_keepalive() = 0;

    // Convenience: send any packed struct directly.
    template<typename T>
    void send_msg(const T& msg) {
        send({ reinterpret_cast<const uint8_t*>(&msg), sizeof(T) });
    }

    virtual void close() = 0;
};

// Factory; returns the correct implementation for the current platform.
// Defined in transport_native.cpp or transport_wasm.cpp.
std::unique_ptr<Transport> make_transport();

} // namespace pong
