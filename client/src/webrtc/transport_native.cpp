#include "pong/transport.h"

#ifndef PONG_WASM   // entire file excluded from WASM build

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include <iostream>
#include <memory>
#include <mutex>
#include <string>

namespace pong {

class NativeTransport : public Transport {
public:
    // ── Host ─────────────────────────────────────────────────
    void host(const std::string& signaling_url) override {
        connect_signaling(signaling_url, /*is_host=*/true, "");
    }

    // ── Guest ────────────────────────────────────────────────
    void join(const std::string& signaling_url,
              const std::string& code) override {
        connect_signaling(signaling_url, /*is_host=*/false, code);
    }

    // ── Send ─────────────────────────────────────────────────
    void send(std::span<const uint8_t> data) override {
        if (!dc_ || !dc_->isOpen()) return;
        dc_->send(reinterpret_cast<const std::byte*>(data.data()), data.size());
    }

    void close() override {
        if (dc_) dc_->close();
        if (pc_) pc_->close();
        if (ws_) ws_->close();
    }

private:
    std::shared_ptr<rtc::WebSocket>     ws_;
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::DataChannel>   dc_;
    std::string                         lobby_code_;

    void connect_signaling(const std::string& url,
                           bool is_host,
                           const std::string& code)
    {
        ws_ = std::make_shared<rtc::WebSocket>();

        ws_->onOpen([this, is_host, code]() {
            std::cout << "[transport] signaling connected\n";
            if (is_host) {
                nlohmann::json msg = { {"type", "host"} };
                ws_->send(msg.dump());
            } else {
                nlohmann::json msg = { {"type", "join"}, {"code", code} };
                ws_->send(msg.dump());
            }
        });

        ws_->onMessage([this, is_host](rtc::message_variant data) {
            if (!std::holds_alternative<std::string>(data)) return;
            auto j = nlohmann::json::parse(std::get<std::string>(data));
            handle_signaling(j, is_host);
        });

        ws_->onError([](const std::string& err) {
            std::cerr << "[transport] signaling error: " << err << "\n";
        });

        ws_->open(url);
    }

    void handle_signaling(const nlohmann::json& j, bool is_host) {
        const std::string type = j.value("type", "");

        if (type == "code") {
            // Host receives lobby code from signaling server
            lobby_code_ = j["code"].get<std::string>();
            if (on_lobby_code) on_lobby_code(lobby_code_);
            setup_peer(/*initiator=*/true);

        } else if (type == "guest_ready") {
            // Signaling server tells host a guest has arrived; create offer
            create_offer();

        } else if (type == "offer") {
            // Guest receives host's SDP offer
            setup_peer(/*initiator=*/false);
            pc_->setRemoteDescription({ j["sdp"].get<std::string>(), "offer" });

        } else if (type == "answer") {
            // Host receives guest's SDP answer
            pc_->setRemoteDescription({ j["sdp"].get<std::string>(), "answer" });

        } else if (type == "ice") {
            // ICE candidate from peer
            rtc::Candidate c{ j["candidate"].get<std::string>(),
                              j["mid"].get<std::string>() };
            pc_->addRemoteCandidate(c);
        }
    }

    void setup_peer(bool initiator) {
        rtc::Configuration cfg;
        cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");

        pc_ = std::make_shared<rtc::PeerConnection>(cfg);

        pc_->onLocalDescription([this](rtc::Description desc) {
            nlohmann::json msg = {
                {"type", desc.typeString()},
                {"sdp",  std::string(desc)}
            };
            ws_->send(msg.dump());
        });

        pc_->onLocalCandidate([this](rtc::Candidate c) {
            nlohmann::json msg = {
                {"type",      "ice"},
                {"candidate", std::string(c)},
                {"mid",       c.mid()}
            };
            ws_->send(msg.dump());
        });

        if (initiator) {
            dc_ = pc_->createDataChannel("pong");
            setup_dc();
            pc_->setLocalDescription(); // triggers offer generation
        } else {
            pc_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
                dc_ = dc;
                setup_dc();
            });
        }
    }

    void create_offer() {
        if (pc_) pc_->setLocalDescription();
    }

    void setup_dc() {
        dc_->onOpen([this]() {
            std::cout << "[transport] DataChannel open\n";
            if (on_open) on_open();
        });
        dc_->onMessage([this](rtc::message_variant data) {
            if (!std::holds_alternative<rtc::binary>(data)) return;
            auto& bin = std::get<rtc::binary>(data);
            if (on_message) {
                on_message({ reinterpret_cast<const uint8_t*>(bin.data()), bin.size() });
            }
        });
        dc_->onClosed([this]() {
            std::cout << "[transport] DataChannel closed\n";
            if (on_close) on_close();
        });
    }
};

std::unique_ptr<Transport> make_transport() {
    return std::make_unique<NativeTransport>();
}

} // namespace pong

#endif // !PONG_WASM
