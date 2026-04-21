#include "pong/transport.h"

#ifndef PONG_WASM

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <algorithm>

namespace pong {

// Relay transport
class NativeTransport : public Transport {
public:
    void host(const std::string& signaling_url, const std::string& username) override {
        open_ws(signaling_url, [this, username]() {
            nlohmann::json msg = { {"type", "host"}, {"username", username} };
            ws_->send(msg.dump());
        });
    }

    void join(const std::string& signaling_url, const std::string& code) override {
        open_ws(signaling_url, [this, code]() {
            nlohmann::json msg = { {"type", "join"}, {"code", code} };
            ws_->send(msg.dump());
        });
    }

    void send(std::span<const uint8_t> data) override {
        if (!ws_ || ws_->readyState() != rtc::WebSocket::State::Open)
            return;
        auto* bytes = reinterpret_cast<const std::byte*>(data.data());
        rtc::binary bin(bytes, bytes + data.size());
        ws_->send(std::move(bin));
    }

    void send_signaling_keepalive() override {
        if (ws_ && ws_->readyState() == rtc::WebSocket::State::Open) {
            ws_->send(R"({"type":"keepalive"})");
        }
    }

    void close() override {
        if (ws_) ws_->close();
    }

private:
    std::shared_ptr<rtc::WebSocket> ws_;

    void open_ws(const std::string& url, std::function<void()> on_open_send) {
        ws_ = std::make_shared<rtc::WebSocket>();

        ws_->onOpen([this, on_open_send]() {
            std::cout << "[transport] signaling connected\n";
            on_open_send();
        });

        ws_->onMessage([this](rtc::message_variant data) {
            if (std::holds_alternative<rtc::binary>(data)) {
                // Relay binary = game data
                auto& bin = std::get<rtc::binary>(data);
                if (on_message)
                    on_message({ reinterpret_cast<const uint8_t*>(bin.data()), bin.size() });
                return;
            }
            // Text = signaling JSON
            nlohmann::json j;
            try { j = nlohmann::json::parse(std::get<std::string>(data)); }
            catch (...) { return; }

            const std::string type = j.value("type", "");

            if (type == "keepalive") return;

            std::cout << "[transport] signaling rx: " << type << "\n";

            if (type == "code") {
                if (on_lobby_code) on_lobby_code(j["code"].get<std::string>());
            } else if (type == "guest_ready" || type == "ready") {
                // Both sides use the same callback name; "guest_ready" for host,
                // "ready" for guest — either signals the relay is live.
                std::cout << "[transport] relay established, peer connected\n";
                if (on_open) on_open();
            } else if (type == "error") {
                std::cerr << "[transport] signaling error: " << j.value("msg", "?") << "\n";
                // ws_ will be closed by the server; onClosed fires on_close
            }
        });

        ws_->onError([](const std::string& err) {
            std::cerr << "[transport] WS error: " << err << "\n";
        });

        ws_->onClosed([this]() {
            std::cout << "[transport] WS closed\n";
            if (on_close) on_close();
        });

        ws_->open(url);
    }
};

std::unique_ptr<Transport> make_transport() {
    return std::make_unique<NativeTransport>();
}

// Global store to keep temporary fetch sockets alive until they finish
static std::mutex g_temp_ws_mtx;
static std::vector<std::shared_ptr<rtc::WebSocket>> g_temp_sockets;

void fetch_lobbies(const std::string& url, OnLobbiesFetched callback) {
    auto ws = std::make_shared<rtc::WebSocket>();

    {
        std::lock_guard lk(g_temp_ws_mtx);
        g_temp_sockets.push_back(ws);
    }

    ws->onOpen([ws_ptr = ws.get()]() {
        ws_ptr->send(R"({"type":"list"})");
    });

    ws->onMessage([ws_ptr = ws.get(), callback](rtc::message_variant data) {
        if (std::holds_alternative<std::string>(data)) {
            try {
                auto j = nlohmann::json::parse(std::get<std::string>(data));
                if (j.value("type", "") == "list") {
                    std::vector<LobbyInfo> result;
                    for (const auto& l : j["lobbies"]) {
                        result.push_back({
                            l.value("code", ""),
                            l.value("host", ""),
                            l.value("players", 1)
                        });
                    }
                    callback(result);
                    ws_ptr->close();
                }
            } catch (...) {}
        }
    });

    ws->onClosed([ws]() {
        std::lock_guard lk(g_temp_ws_mtx);
        g_temp_sockets.erase(
            std::remove(g_temp_sockets.begin(), g_temp_sockets.end(), ws),
            g_temp_sockets.end()
        );
    });

    ws->open(url);
}

} // namespace pong

#endif