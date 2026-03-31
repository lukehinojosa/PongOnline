#pragma once

namespace pong {

enum class Role {
    None,   // not yet chosen
    Host,   // runs the authoritative simulation
    Guest,  // sends input, receives game state
};

} // namespace pong
