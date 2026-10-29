#pragma once

#include <string>
#include <vector>

#include "app/account.hpp"

namespace tetris {

// A same-origin fetch bridge to the account API. Only the web build has one; on desktop
// accountsSupported() is false and the other two do nothing.
bool accountsSupported();

// Fire and forget: the reply shows up in a later takeAccountReplies(). `body` is empty for GET and logout.
void sendAccountRequest(AccountCall call, const std::string& body);

// The replies that arrived since the last call, in arrival order.
std::vector<AccountReply> takeAccountReplies();

}  // namespace tetris
