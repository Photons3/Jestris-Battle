#pragma once

#include <memory>
#include <string>
#include <vector>

namespace tetris {

// Text-message link to the duel server. Messages sent before the link opens are queued.
class Connection {
public:
    virtual ~Connection() = default;
    virtual void send(const std::string& text) = 0;
    virtual std::vector<std::string> receive() = 0;  // messages that arrived since the last call
    virtual bool closed() const = 0;                 // the server went away or could not be reached
};

// True only in the browser build, which is served by (and can reach) the duel server.
bool duelsSupported();
// Connects to ws://<page host>/ws. Only called when duelsSupported().
std::unique_ptr<Connection> connectToServer();

}  // namespace tetris
