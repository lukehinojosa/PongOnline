#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>

// Lobby

struct Lobby {
    std::shared_ptr<rtc::WebSocket> host_ws;
};

static std::mutex g_mutex;
static std::unordered_map<std::string, Lobby> g_lobbies;

// Generate a random 6-character uppercase alphanumeric code.
static std::string make_code() {
    static const char chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static std::mt19937 rng{ std::random_device{}() };
    static std::uniform_int_distribution<> dist(0, sizeof(chars) - 2);
    std::string code(6, ' ');
    for (auto& c : code)
        c = chars[dist(rng)];
    return code;
}

static void handle_client(std::shared_ptr<rtc::WebSocket> ws) {
    ws->onMessage([ws](rtc::message_variant data) {
        if (!std::holds_alternative<std::string>(data))
            return;

        nlohmann::json msg;
        try
        {
            msg = nlohmann::json::parse(std::get<std::string>(data));
        }
        catch (...)
        {
            return;
        }

        const std::string type = msg.value("type", "");

        if (type == "host") {
            // Assign a lobby code and store the host's WS.
            std::string code;
            {
                std::lock_guard lk(g_mutex);
                do
                {
                    code = make_code();
                } while (g_lobbies.count(code));
                g_lobbies[code].host_ws = ws;
            }
            ws->send(nlohmann::json{ {"type","code"}, {"code", code} }.dump());
            std::cout << "[signaling] new lobby: " << code << "\n";

        } else if (type == "join") {
            const std::string code = msg.value("code", "");
            std::shared_ptr<rtc::WebSocket> host_ws;
            {
                std::lock_guard lk(g_mutex);
                auto it = g_lobbies.find(code);
                if (it == g_lobbies.end()) {
                    ws->send(nlohmann::json{{"type","error"},{"msg","bad code"}}.dump());
                    return;
                }
                host_ws = it->second.host_ws;
            }
            // Tell the host a guest is ready, then relay SDP/ICE between them.
            host_ws->send(nlohmann::json{{"type","guest_ready"}}.dump());

            // Each side relays its SDP/ICE to the other.
            ws->onMessage([host_ws, ws](rtc::message_variant relay) {
                if (std::holds_alternative<std::string>(relay))
                    host_ws->send(std::get<std::string>(relay));
            });
            host_ws->onMessage([host_ws, ws](rtc::message_variant relay) {
                if (std::holds_alternative<std::string>(relay))
                    ws->send(std::get<std::string>(relay));
            });

            std::cout << "[signaling] guest joined lobby: " << code << "\n";

        } else if (type == "offer" || type == "answer" || type == "ice") {
            // Lobby setup hasn't happened yet; ignore.
        }
    });

    ws->onClosed([ws]() {
        std::lock_guard lk(g_mutex);
        for (auto it = g_lobbies.begin(); it != g_lobbies.end(); ) {
            if (it->second.host_ws == ws)
                it = g_lobbies.erase(it);
            else
                ++it;
        }
    });
}

int main() {
    std::cout << "[signaling] Online Pong signaling server starting on port 9000\n";

    rtc::WebSocketServer::Configuration cfg;
    cfg.port = 9000;

    auto server = std::make_shared<rtc::WebSocketServer>(cfg);

    server->onClient([](std::shared_ptr<rtc::WebSocket> ws) {
        std::cout << "[signaling] client connected\n";
        handle_client(ws);
    });

    std::cout << "[signaling] Listening. Press ENTER to stop.\n";
    std::cin.get();
    return 0;
}
