#include "pong/transport.h"

#ifdef PONG_WASM

#include <emscripten/emscripten.h>
#include <emscripten/val.h>
#include <memory>
#include <vector>

// JS bridge
EM_JS(void, js_transport_host, (const char* signaling_url), {
    const url = UTF8ToString(signaling_url);
    console.log('[pong] host: connecting to', url);
    window._pongWS = new WebSocket(url);
    window._pongWS.binaryType = 'arraybuffer';
    window._pongWS.onopen  = () => {
        console.log('[pong] host: WS open');
        window._pongWS.send(JSON.stringify({ type: 'host' }));
    };
    window._pongWS.onerror = (e) => console.error('[pong] host: WS error', e);
    window._pongWS.onclose = (e) => {
        console.log('[pong] host: WS closed', e.code);
        Module.ccall('wasm_on_close', null, [], []);
    };
    window._pongWS.onmessage = (e) => {
        if (e.data instanceof ArrayBuffer) {
            // Binary = game data relayed from guest
            const buf = new Uint8Array(e.data);
            Module.ccall('wasm_on_message', null, ['array','number'], [buf, buf.length]);
            return;
        }
        // Text = signaling JSON
        console.log('[pong] host: signaling rx:', e.data);
        const msg = JSON.parse(e.data);
        if (msg.type === 'code') {
            Module.ccall('wasm_on_lobby_code', null, ['string'], [msg.code]);
        } else if (msg.type === 'guest_ready' || msg.type === 'ready') {
            console.log('[pong] host: relay established');
            Module.ccall('wasm_on_open', null, [], []);
        } else if (msg.type === 'error') {
            console.error('[pong] host: signaling error:', msg.msg);
            // server will close the WS; onclose fires wasm_on_close
        }
    };
});

EM_JS(void, js_transport_join, (const char* signaling_url, const char* code), {
    const url      = UTF8ToString(signaling_url);
    const joinCode = UTF8ToString(code);
    console.log('[pong] join: connecting to', url, 'code:', joinCode);
    window._pongWS = new WebSocket(url);
    window._pongWS.binaryType = 'arraybuffer';
    window._pongWS.onopen  = () => {
        console.log('[pong] join: WS open');
        window._pongWS.send(JSON.stringify({ type: 'join', code: joinCode }));
    };
    window._pongWS.onerror = (e) => console.error('[pong] join: WS error', e);
    window._pongWS.onclose = (e) => {
        console.log('[pong] join: WS closed', e.code);
        Module.ccall('wasm_on_close', null, [], []);
    };
    window._pongWS.onmessage = (e) => {
        if (e.data instanceof ArrayBuffer) {
            // Binary = game data relayed from host
            const buf = new Uint8Array(e.data);
            Module.ccall('wasm_on_message', null, ['array','number'], [buf, buf.length]);
            return;
        }
        console.log('[pong] join: signaling rx:', e.data);
        const msg = JSON.parse(e.data);
        if (msg.type === 'guest_ready' || msg.type === 'ready') {
            console.log('[pong] join: relay established');
            Module.ccall('wasm_on_open', null, [], []);
        } else if (msg.type === 'error') {
            console.error('[pong] join: signaling error:', msg.msg);
        }
    };
});

EM_JS(void, js_transport_send, (const uint8_t* data, int len), {
    if (!window._pongWS || window._pongWS.readyState !== 1 /* OPEN */)
        return;
    window._pongWS.send(HEAPU8.slice(data, data + len));
});

EM_JS(void, js_transport_close, (), {
    if (window._pongWS) window._pongWS.close();
});

// C++ callbacks invoked from JS

namespace pong {
    static Transport* g_transport = nullptr;
}

extern "C" {
    EMSCRIPTEN_KEEPALIVE void wasm_on_open() {
        if (pong::g_transport && pong::g_transport->on_open)
            pong::g_transport->on_open();
    }
    EMSCRIPTEN_KEEPALIVE void wasm_on_close() {
        if (pong::g_transport && pong::g_transport->on_close)
            pong::g_transport->on_close();
    }
    EMSCRIPTEN_KEEPALIVE void wasm_on_message(const uint8_t* data, int len) {
        if (pong::g_transport && pong::g_transport->on_message)
            pong::g_transport->on_message({ data, static_cast<size_t>(len) });
    }
    EMSCRIPTEN_KEEPALIVE void wasm_on_lobby_code(const char* code) {
        if (pong::g_transport && pong::g_transport->on_lobby_code)
            pong::g_transport->on_lobby_code(code);
    }
}

// WasmTransport

namespace pong {

class WasmTransport : public Transport {
public:
    WasmTransport()  { g_transport = this; }
    ~WasmTransport() { if (g_transport == this) g_transport = nullptr; }

    void host(const std::string& signaling_url) override {
        js_transport_host(signaling_url.c_str());
    }
    void join(const std::string& signaling_url, const std::string& code) override {
        js_transport_join(signaling_url.c_str(), code.c_str());
    }
    void send(std::span<const uint8_t> data) override {
        js_transport_send(data.data(), static_cast<int>(data.size()));
    }
    void close() override {
        js_transport_close();
    }
};

std::unique_ptr<Transport> make_transport() {
    return std::make_unique<WasmTransport>();
}

} // namespace pong

#endif // PONG_WASM
