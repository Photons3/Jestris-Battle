#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>

#include <cstring>
#include <utility>

#include "net/connection.hpp"

namespace tetris {
namespace {

// Browser WebSocket. Callbacks run on the main thread between frames (no pthreads).
class WebSocketConnection final : public Connection {
public:
    explicit WebSocketConnection(const std::string& url) {
        EmscriptenWebSocketCreateAttributes attrs;
        emscripten_websocket_init_create_attributes(&attrs);
        attrs.url = url.c_str();
        socket_ = emscripten_websocket_new(&attrs);
        if (socket_ <= 0) {
            closed_ = true;
            return;
        }
        emscripten_websocket_set_onopen_callback(socket_, this, onOpen);
        emscripten_websocket_set_onmessage_callback(socket_, this, onMessage);
        emscripten_websocket_set_onclose_callback(socket_, this, onClose);
        emscripten_websocket_set_onerror_callback(socket_, this, onError);
    }

    ~WebSocketConnection() override {
        if (socket_ <= 0) return;
        emscripten_websocket_close(socket_, 1000, "leaving");
        emscripten_websocket_delete(socket_);  // also drops the callbacks, so `this` is never used again
    }

    void send(const std::string& text) override {
        if (closed_) return;
        if (!open_) {
            outbox_.push_back(text);
            return;
        }
        emscripten_websocket_send_utf8_text(socket_, text.c_str());
    }

    std::vector<std::string> receive() override { return std::exchange(inbox_, {}); }
    bool closed() const override { return closed_; }

private:
    static EM_BOOL onOpen(int, const EmscriptenWebSocketOpenEvent*, void* self) {
        auto* connection = static_cast<WebSocketConnection*>(self);
        connection->open_ = true;
        for (const std::string& text : std::exchange(connection->outbox_, {})) connection->send(text);
        return EM_TRUE;
    }

    static EM_BOOL onMessage(int, const EmscriptenWebSocketMessageEvent* event, void* self) {
        if (!event->isText) return EM_TRUE;
        const auto* text = reinterpret_cast<const char*>(event->data);
        static_cast<WebSocketConnection*>(self)->inbox_.emplace_back(text, strnlen(text, event->numBytes));
        return EM_TRUE;
    }

    static EM_BOOL onClose(int, const EmscriptenWebSocketCloseEvent*, void* self) {
        static_cast<WebSocketConnection*>(self)->closed_ = true;
        return EM_TRUE;
    }

    static EM_BOOL onError(int, const EmscriptenWebSocketErrorEvent*, void* self) {
        static_cast<WebSocketConnection*>(self)->closed_ = true;
        return EM_TRUE;
    }

    EMSCRIPTEN_WEBSOCKET_T socket_ = 0;
    bool open_ = false;
    bool closed_ = false;
    std::vector<std::string> outbox_;
    std::vector<std::string> inbox_;
};

}  // namespace

bool duelsSupported() { return true; }

std::unique_ptr<Connection> connectToServer() {
    // wss:// on an https:// page (browsers block ws:// there as mixed content), ws:// otherwise.
    const std::string url =
        emscripten_run_script_string("(location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws'");
    return std::make_unique<WebSocketConnection>(url);
}

}  // namespace tetris
