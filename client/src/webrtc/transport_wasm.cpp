#include "pong/transport.h"

#ifdef PONG_WASM

#include <emscripten/emscripten.h>
#include <emscripten/val.h>
#include <iostream>
#include <memory>
#include <vector>

// JS bridge

EM_JS(void, js_transport_host, (const char* signaling_url), {
    const url = UTF8ToString(signaling_url);
    window._pongWS = new WebSocket(url);
    window._pongWS.binaryType = 'arraybuffer';
    window._pongWS.onopen = () => {
        window._pongWS.send(JSON.stringify({ type: 'host' }));
    };
    window._pongWS.onmessage = (e) => {
        window._pongHandleSignaling(JSON.parse(e.data), true);
    };
});

EM_JS(void, js_transport_join, (const char* signaling_url, const char* code), {
    const url = UTF8ToString(signaling_url);
    const joinCode = UTF8ToString(code);
    window._pongWS = new WebSocket(url);
    window._pongWS.binaryType = 'arraybuffer';
    window._pongWS.onopen = () => {
        window._pongWS.send(JSON.stringify({ type: 'join', code: joinCode }));
    };
    window._pongWS.onmessage = (e) => {
        window._pongHandleSignaling(JSON.parse(e.data), false);
    };
});

EM_JS(void, js_transport_send, (const uint8_t* data, int len), {
    if (!window._pongDC || window._pongDC.readyState !== 'open')
        return;
    window._pongDC.send(HEAPU8.slice(data, data + len));
});

EM_JS(void, js_transport_close, (), {
    if (window._pongDC)
        window._pongDC.close();
    if (window._pongPC)
        window._pongPC.close();
    if (window._pongWS)
        window._pongWS.close();
});

// Install the JS signaling handler and DataChannel callbacks.
// Called once at startup.
EM_JS(void, js_install_bridge, (), {
    const iceServers = [{ urls: 'stun:stun.l.google.com:19302' }];

    window._pongSetupDC = (dc) => {
        window._pongDC = dc;
        dc.binaryType = 'arraybuffer';
        dc.onopen = () => { Module.ccall('wasm_on_open', null, [], []); };
        dc.onclose = () => { Module.ccall('wasm_on_close', null, [], []); };
        dc.onmessage = (e) => {
            const buf = new Uint8Array(e.data);
            const ptr = Module._malloc(buf.length);
            Module.HEAPU8.set(buf, ptr);
            Module.ccall('wasm_on_message', null, ['number','number'], [ptr, buf.length]);
            Module._free(ptr);
        };
    };

    window._pongHandleSignaling = (msg, isHost) => {
        if (msg.type === 'code') {
            // Host got lobby code
            const codePtr = Module.stringToNewUTF8(msg.code);
            Module.ccall('wasm_on_lobby_code', null, ['number'], [codePtr]);
            Module._free(codePtr);
            // Create peer + offer
            window._pongPC = new RTCPeerConnection({ iceServers });
            window._pongPC.onicecandidate = (e) => {
                if (e.candidate) {
                    window._pongWS.send(JSON.stringify({
                        type: 'ice',
                        candidate: e.candidate.candidate,
                        mid: e.candidate.sdpMid
                    }));
                }
            };
            const dc = window._pongPC.createDataChannel('pong');
            window._pongSetupDC(dc);
            window._pongPC.createOffer()
                .then(o => window._pongPC.setLocalDescription(o))
                .then(() => window._pongWS.send(JSON.stringify({
                    type: 'offer', sdp: window._pongPC.localDescription.sdp
                })));

        } else if (msg.type === 'guest_ready') {
            // Host: guest has arrived, offer already sent above

        } else if (msg.type === 'offer') {
            // Guest receives offer
            window._pongPC = new RTCPeerConnection({ iceServers });
            window._pongPC.onicecandidate = (e) => {
                if (e.candidate) {
                    window._pongWS.send(JSON.stringify({
                        type: 'ice',
                        candidate: e.candidate.candidate,
                        mid: e.candidate.sdpMid
                    }));
                }
            };
            window._pongPC.ondatachannel = (e) => {
                window._pongSetupDC(e.channel);
            };
            window._pongPC.setRemoteDescription({ type: 'offer', sdp: msg.sdp })
                .then(() => window._pongPC.createAnswer())
                .then(a => window._pongPC.setLocalDescription(a))
                .then(() => window._pongWS.send(JSON.stringify({
                    type: 'answer', sdp: window._pongPC.localDescription.sdp
                })));

        } else if (msg.type === 'answer') {
            window._pongPC.setRemoteDescription({ type: 'answer', sdp: msg.sdp });

        } else if (msg.type === 'ice') {
            window._pongPC.addIceCandidate({
                candidate: msg.candidate, sdpMid: msg.mid
            });
        }
    };
});

// C++ callbacks invoked from JS

namespace pong {
    // Single global transport pointer so the JS callbacks can reach it.
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
    WasmTransport() { js_install_bridge(); g_transport = this; }
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
