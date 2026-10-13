#include "net/connection.hpp"

namespace tetris {

bool duelsSupported() { return false; }
std::unique_ptr<Connection> connectToServer() { return nullptr; }

}  // namespace tetris
