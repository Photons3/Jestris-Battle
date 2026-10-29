#include <emscripten/emscripten.h>

#include <cstdlib>
#include <string_view>

#include "app/account_store.hpp"

// EM_JS bodies aren't scanned for the JS helpers they call, so list them here.
EM_JS_DEPS(account_store, "$stringToNewUTF8,$UTF8ToString");

// Replies queue on Module so the C++ side can poll them once per frame. A rejected fetch is status 0.
EM_JS(void, accountFetch, (int call, const char* method, const char* path, const char* body), {
    const verb = UTF8ToString(method);
    const text = UTF8ToString(body);
    const queue = (Module.jestrisAccountReplies = Module.jestrisAccountReplies || []);
    fetch(UTF8ToString(path), {
        method: verb,
        credentials: "include",
        cache: "no-store",
        body: verb === "GET" ? undefined : text
    }).then(
        (r) => r.text().then(
            (t) => { queue.push({call: call, status: r.status, text: t}); },
            () => { queue.push({call: call, status: 0, text: ""}); }),
        () => { queue.push({call: call, status: 0, text: ""}); });
});

EM_JS(char*, accountTakeReply, (), {
    const queue = Module.jestrisAccountReplies;
    if (!queue || queue.length === 0) return 0;
    const r = queue.shift();
    return stringToNewUTF8(r.call + " " + r.status + "\n" + r.text);
});

namespace tetris {
namespace {

constexpr int kCallCount = static_cast<int>(AccountCall::Best) + 1;

// "<call> <status>\n<text>". Anything that does not parse is dropped rather than guessed at.
bool parseReply(std::string_view raw, AccountReply& out) {
    const size_t space = raw.find(' ');
    const size_t newline = raw.find('\n');
    if (space == std::string_view::npos || newline == std::string_view::npos || space > newline) return false;

    auto number = [](std::string_view digits, int& value) {
        if (digits.empty() || digits.size() > 4) return false;
        value = 0;
        for (const char c : digits) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        return true;
    };
    int call = 0, status = 0;
    if (!number(raw.substr(0, space), call) || call >= kCallCount) return false;
    if (!number(raw.substr(space + 1, newline - space - 1), status)) return false;

    out.call = static_cast<AccountCall>(call);
    out.status = status;
    out.body = std::string(raw.substr(newline + 1));
    return true;
}

}  // namespace

bool accountsSupported() { return true; }

void sendAccountRequest(AccountCall call, const std::string& body) {
    const AccountRoute route = routeFor(call);
    accountFetch(static_cast<int>(call), route.method, route.path, body.c_str());
}

std::vector<AccountReply> takeAccountReplies() {
    std::vector<AccountReply> replies;
    while (char* raw = accountTakeReply()) {
        AccountReply reply;
        if (parseReply(raw, reply)) replies.push_back(std::move(reply));
        std::free(raw);
    }
    return replies;
}

}  // namespace tetris
