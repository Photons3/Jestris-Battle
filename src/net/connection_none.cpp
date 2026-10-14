#include "net/connection.hpp"

namespace tetris {

bool duelsSupported() { return false; }
std::unique_ptr<Connection> connectToServer() { return nullptr; }
std::string takeInviteCode() { return {}; }
std::string inviteLinkBase() { return {}; }
bool canCopyToClipboard() { return false; }
void copyInviteLink(const std::string&) {}

}  // namespace tetris
