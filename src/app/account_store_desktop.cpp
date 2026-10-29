#include "app/account_store.hpp"

namespace tetris {

bool accountsSupported() { return false; }

void sendAccountRequest(AccountCall, const std::string&) {}

std::vector<AccountReply> takeAccountReplies() { return {}; }

}  // namespace tetris
