#include "pong/transport.h"

#ifdef PONG_WASM

#include <emscripten/emscripten.h>
#include <emscripten/val.h>
#include <memory>
#include <vector>
#include <unordered_map>
#include <nlohmann/json.hpp>

// JS bridge
EM_JS(void, js_transport_host, (int id, const char* signaling_url, const char* username), {
    if (!window._pongSockets) window._pongSockets = {};
    const url = UTF8ToString(signaling_url);
    const uname = UTF8ToString(username);

    const ws = new WebSocket(url);
    window._pongSockets[id] = ws; // Store uniquely by C++ pointer ID
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
        ws.send(JSON.stringify({ type: 'host', username: uname }));
    };
    ws.onerror = (e) => console.error('[pong] host WS error on', url);
    ws.onclose = (e) => {
        Module.ccall('wasm_on_close', 'void', ['number'], [id]);
    };
    ws.onmessage = (e) => {
        if (e.data instanceof ArrayBuffer) {
            const buf = new Uint8Array(e.data);
            Module.ccall('wasm_on_message', 'void', ['number', 'array', 'number'], [id, buf, buf.length]);
            return;
        }

        const msg = JSON.parse(e.data);
        if (msg.type === 'keepalive') return;

        if (msg.type === 'code') {
            Module.ccall('wasm_on_lobby_code', 'void', ['number', 'string'], [id, msg.code]);
        } else if (msg.type === 'guest_ready' || msg.type === 'ready') {
            Module.ccall('wasm_on_open', 'void', ['number'], [id]);
        }
    };
});

EM_JS(void, js_transport_join, (int id, const char* signaling_url, const char* code), {
    if (!window._pongSockets) window._pongSockets = {};
    const url = UTF8ToString(signaling_url);
    const joinCode = UTF8ToString(code);

    const ws = new WebSocket(url);
    window._pongSockets[id] = ws;
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
        ws.send(JSON.stringify({ type: 'join', code: joinCode }));
    };
    ws.onerror = (e) => console.error('[pong] join WS error on', url);
    ws.onclose = (e) => {
        Module.ccall('wasm_on_close', 'void', ['number'], [id]);
    };
    ws.onmessage = (e) => {
        if (e.data instanceof ArrayBuffer) {
            const buf = new Uint8Array(e.data);
            Module.ccall('wasm_on_message', 'void', ['number', 'array', 'number'], [id, buf, buf.length]);
            return;
        }

        const msg = JSON.parse(e.data);
        if (msg.type === 'keepalive') return;

        if (msg.type === 'guest_ready' || msg.type === 'ready') {
            Module.ccall('wasm_on_open', 'void', ['number'], [id]);
        }
    };
});

EM_JS(void, js_transport_send, (int id, const uint8_t* data, int len), {
    if (window._pongSockets && window._pongSockets[id]) {
        const ws = window._pongSockets[id];
        if (ws.readyState === 1 /* OPEN */) {
            ws.send(HEAPU8.slice(data, data + len));
        }
    }
});

EM_JS(void, js_transport_keepalive, (int id), {
    if (window._pongSockets && window._pongSockets[id]) {
        const ws = window._pongSockets[id];
        if (ws.readyState === 1 /* OPEN */) {
            ws.send(JSON.stringify({ type: 'keepalive' }));
        }
    }
});

EM_JS(void, js_transport_close, (int id), {
    if (window._pongSockets && window._pongSockets[id]) {
        window._pongSockets[id].close();
        delete window._pongSockets[id]; // Cleanup JS memory
    }
});

EM_JS(void, js_fetch_lobbies, (int cb_id, const char* signaling_url), {
    const url = UTF8ToString(signaling_url);
    const ws = new WebSocket(url);
    ws.onopen = () => { ws.send(JSON.stringify({ type: 'list' })); };
    ws.onmessage = (e) => {
        const msg = JSON.parse(e.data);
        if (msg.type === 'list') {
            Module.ccall('wasm_on_lobbies_fetched', 'void', ['number', 'string'], [cb_id, JSON.stringify(msg.lobbies)]);
            ws.close();
        }
    };
    ws.onerror = (e) => ws.close();
});

// Global state for mapping temporary fetch requests
static std::unordered_map<int, pong::OnLobbiesFetched> g_lobby_cbs;
static int g_cb_id = 0;

// C++ callbacks invoked from JS
extern "C" {
    EMSCRIPTEN_KEEPALIVE void wasm_on_open(int id) {
        auto* t = reinterpret_cast<pong::Transport*>(id);
        if (t && t->on_open) t->on_open();
    }
    EMSCRIPTEN_KEEPALIVE void wasm_on_close(int id) {
        auto* t = reinterpret_cast<pong::Transport*>(id);
        if (t && t->on_close) t->on_close();
    }
    EMSCRIPTEN_KEEPALIVE void wasm_on_message(int id, const uint8_t* data, int len) {
        auto* t = reinterpret_cast<pong::Transport*>(id);
        if (t && t->on_message) t->on_message({ data, static_cast<size_t>(len) });
    }
    EMSCRIPTEN_KEEPALIVE void wasm_on_lobby_code(int id, const char* code) {
        auto* t = reinterpret_cast<pong::Transport*>(id);
        if (t && t->on_lobby_code) t->on_lobby_code(code);
    }
    EMSCRIPTEN_KEEPALIVE void wasm_on_lobbies_fetched(int cb_id, const char* json_str) {
        auto it = g_lobby_cbs.find(cb_id);
        if (it != g_lobby_cbs.end()) {
            std::vector<pong::LobbyInfo> result;
            try {
                auto j = nlohmann::json::parse(json_str);
                for (const auto& l : j) {
                    result.push_back({l.value("code", ""), l.value("host", ""), l.value("players", 1)});
                }
            } catch(...) {}
            it->second(result);
            g_lobby_cbs.erase(it);
        }
    }
}

// WasmTransport Class
namespace pong {

class WasmTransport : public Transport {
public:
    WasmTransport() {}

    // Automatically kill the JS socket when C++ destroys this object
    ~WasmTransport() { close(); }

    void host(const std::string& signaling_url, const std::string& username) override {
        js_transport_host(reinterpret_cast<int>(this), signaling_url.c_str(), username.c_str());
    }
    void join(const std::string& signaling_url, const std::string& code) override {
        js_transport_join(reinterpret_cast<int>(this), signaling_url.c_str(), code.c_str());
    }
    void send(std::span<const uint8_t> data) override {
        js_transport_send(reinterpret_cast<int>(this), data.data(), static_cast<int>(data.size()));
    }
    void send_signaling_keepalive() override {
        js_transport_keepalive(reinterpret_cast<int>(this));
    }
    void close() override {
        js_transport_close(reinterpret_cast<int>(this));
    }
};

std::unique_ptr<Transport> make_transport() {
    return std::make_unique<WasmTransport>();
}

void fetch_lobbies(const std::string& signaling_url, OnLobbiesFetched callback) {
    int id = ++g_cb_id;
    g_lobby_cbs[id] = std::move(callback);
    js_fetch_lobbies(id, signaling_url.c_str());
}

} // namespace pong

#endif // PONG_WASM