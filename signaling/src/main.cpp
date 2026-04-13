#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

// Cloudflare Quick Tunnel
// Spawns cloudflared (expected next to this exe) and parses the assigned
// trycloudflare.com URL so users can copy-paste a ready-made WSS address.

static std::string get_exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    char* last = strrchr(buf, '\\');
    if (last) *last = '\0';
    return buf;
#elif defined(__linux__)
    char buf[4096] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        char* last = strrchr(buf, '/');
        if (last) *last = '\0';
        return buf;
    }
    return ".";
#else
    return ".";
#endif
}

static void start_cloudflared_tunnel(int port) {
    std::string exe_dir = get_exe_dir();
#ifdef _WIN32
    std::string cloudflared_path = exe_dir + "\\cloudflared.exe";
#else
    std::string cloudflared_path = exe_dir + "/cloudflared";
#endif

    if (!std::ifstream(cloudflared_path).good()) {
        std::cout << "[tunnel] cloudflared not found — skipping WSS tunnel\n"
                  << "[tunnel] clients must use ws://<your-ip>:" << port << " directly\n";
        return;
    }

    std::string cmd = "\"" + cloudflared_path + "\" tunnel --url http://localhost:"
                    + std::to_string(port) + " --no-autoupdate 2>&1";

    std::cout << "[tunnel] starting cloudflared quick tunnel...\n";

    std::thread([cmd]() {
#ifdef _WIN32
        FILE* pipe = _popen(cmd.c_str(), "r");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        if (!pipe) {
            std::cerr << "[tunnel] failed to spawn cloudflared\n";
            return;
        }

        static const std::regex url_re(R"(https://[\w-]+\.trycloudflare\.com)");
        char line[2048];
        while (fgets(line, sizeof(line), pipe)) {
            std::string s(line);
            std::smatch m;
            if (std::regex_search(s, m, url_re)) {
                std::string wss_url = "wss://" + m[0].str().substr(8); // https:// -> wss://
                std::cout << "\n"
                          << "[tunnel] ============================================\n"
                          << "[tunnel]  WSS URL: " << wss_url << "\n"
                          << "[tunnel]  Enter this in the game's signaling field\n"
                          << "[tunnel] ============================================\n\n"
                          << std::flush;
            }
        }
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
    }).detach();
}

// Shared state

struct Lobby {
    std::shared_ptr<rtc::WebSocket> host_ws;
};

// Once two clients are paired, each entry maps a WebSocket to its relay partner.
static std::mutex g_mutex;
static std::unordered_map<std::string, Lobby> g_lobbies;
static std::unordered_map<rtc::WebSocket*, std::shared_ptr<rtc::WebSocket>> g_relay;

// Helpers

static std::string make_code() {
    static const char chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static std::mt19937 rng{ std::random_device{}() };
    static std::uniform_int_distribution<> dist(0, sizeof(chars) - 2);
    std::string code(6, ' ');
    for (auto& c : code)
        c = chars[dist(rng)];
    return code;
}

// Per-client handler

static void handle_client(std::shared_ptr<rtc::WebSocket> ws) {
    ws->onMessage([ws](rtc::message_variant data) {
        // Relay mode; forward directly to partner (text or binary)
        {
            std::lock_guard lk(g_mutex);
            auto it = g_relay.find(ws.get());
            if (it != g_relay.end()) {
                if (std::holds_alternative<std::string>(data)) {
                    const auto& s = std::get<std::string>(data);
                    it->second->send(s);
                } else {
                    const auto& b = std::get<rtc::binary>(data);
                    // Silently forward binary data.
                    // At 60Hz per client, logging here would flood the console.
                    it->second->send(b);
                }
                return;
            }
        }

        // Signaling mode; text only
        if (!std::holds_alternative<std::string>(data))
            return;
        const std::string& raw = std::get<std::string>(data);

        nlohmann::json msg;
        try { msg = nlohmann::json::parse(raw); }
        catch (...) { return; }

        const std::string type = msg.value("type", "");
        std::cout << "[signaling] rx '" << type << "' from " << ws.get() << "\n";

        if (type == "host") {
            std::string code;
            {
                std::lock_guard lk(g_mutex);
                do { code = make_code(); } while (g_lobbies.count(code));
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
                    ws->close();
                    std::cout << "[signaling] bad code: " << code << "\n";
                    return;
                }
                host_ws = it->second.host_ws;
                // Pair the two sockets in the relay map
                g_relay[host_ws.get()] = ws;
                g_relay[ws.get()]      = host_ws;
            }
            // Notify both sides the relay is live
            host_ws->send(nlohmann::json{{"type","guest_ready"}}.dump());
            ws->send(nlohmann::json{{"type","ready"}}.dump());
            std::cout << "[signaling] guest joined lobby: " << code
                      << " host=" << host_ws.get()
                      << " guest=" << ws.get() << "\n";
        }
        // Any other type before relay is set up: ignore.
    });

    ws->onClosed([ws]() {
        std::lock_guard lk(g_mutex);
        // Remove from lobbies
        for (auto it = g_lobbies.begin(); it != g_lobbies.end(); ) {
            if (it->second.host_ws == ws)
                it = g_lobbies.erase(it);
            else
                ++it;
        }
        // Remove from relay map (both directions)
        auto it = g_relay.find(ws.get());
        if (it != g_relay.end()) {
            g_relay.erase(it->second.get()); // remove partner's entry too
            g_relay.erase(it);
        }
        std::cout << "[signaling] client disconnected: " << ws.get() << "\n";
    });
}

int main() {
    std::cout << "[signaling] Online Pong signaling server starting on port 9000\n";

    start_cloudflared_tunnel(9000);

    rtc::WebSocketServer::Configuration cfg;
    cfg.port = 9000;

    auto server = std::make_shared<rtc::WebSocketServer>(cfg);
    server->onClient([](std::shared_ptr<rtc::WebSocket> ws) {
        std::cout << "[signaling] client connected: " << ws.get() << "\n";
        handle_client(ws);
    });

    std::cout << "[signaling] Listening. Press ENTER to stop.\n";
    std::cin.get();
    return 0;
}
