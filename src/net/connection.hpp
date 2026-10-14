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
    virtual void update(int /*ms*/) {}               // advances a local opponent; network links do nothing
};

// True only in the browser build, which is served by (and can reach) the duel server.
bool duelsSupported();
// Connects to ws://<page host>/ws, or wss:// when the page is served over https. Only called when duelsSupported().
std::unique_ptr<Connection> connectToServer();

// Invite links (browser only; the desktop stubs return nothing).
// The raw ?room= value of the page address, which is then removed from the address bar so a reload doesn't rejoin.
std::string takeInviteCode();
std::string inviteLinkBase();  // e.g. "http://192.168.1.5:8080/?room="; a room code completes the link
bool canCopyToClipboard();     // false on plain-http LAN pages, which have no Clipboard API
void copyInviteLink(const std::string& code);  // does nothing unless `code` is a room code

}  // namespace tetris
