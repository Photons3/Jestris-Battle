#include "app/account.hpp"

#include <charconv>
#include <climits>
#include <cstdio>
#include <cstdlib>

namespace tetris {

namespace {

struct Json {
    enum class Type { Null, Bool, Number, String, Object };
    Type type = Type::Null;
    bool integral = false;  // a Number written without a fraction or exponent, and in range for int
    long long integer = 0;
    double number = 0;
    std::string text;
    std::vector<std::string> keys;  // Object members, in order
    std::vector<Json> values;

    const Json* find(std::string_view key) const {
        const Json* found = nullptr;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) found = &values[i];  // the last duplicate wins
        }
        return found;
    }
};

constexpr int kMaxDepth = 16;

void appendUtf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    std::optional<Json> readDocument() {
        Json value;
        skipSpace();
        if (!readValue(value, 0)) return std::nullopt;
        skipSpace();
        if (pos_ != text_.size()) return std::nullopt;  // trailing garbage
        return value;
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;

    bool atEnd() const { return pos_ >= text_.size(); }

    void skipSpace() {
        while (!atEnd() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool consume(char c) {
        if (atEnd() || text_[pos_] != c) return false;
        ++pos_;
        return true;
    }

    bool consumeWord(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool readValue(Json& out, int depth) {
        if (atEnd()) return false;
        const char c = text_[pos_];
        if (c == '{') return readObject(out, depth);
        if (c == '"') {
            out.type = Json::Type::String;
            return readString(out.text);
        }
        if (c == '-' || (c >= '0' && c <= '9')) return readNumber(out);
        if (consumeWord("null")) {
            out.type = Json::Type::Null;
            return true;
        }
        if (consumeWord("true") || consumeWord("false")) {
            out.type = Json::Type::Bool;
            return true;
        }
        return false;
    }

    bool readObject(Json& out, int depth) {
        if (depth >= kMaxDepth) return false;
        out.type = Json::Type::Object;
        ++pos_;  // '{'
        skipSpace();
        if (consume('}')) return true;
        while (true) {
            skipSpace();
            std::string key;
            if (atEnd() || text_[pos_] != '"' || !readString(key)) return false;
            skipSpace();
            if (!consume(':')) return false;
            skipSpace();
            Json value;
            if (!readValue(value, depth + 1)) return false;
            out.keys.push_back(std::move(key));
            out.values.push_back(std::move(value));
            skipSpace();
            if (consume('}')) return true;
            if (!consume(',')) return false;
        }
    }

    bool readHex4(unsigned& code) {
        if (text_.size() - pos_ < 4) return false;
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            code <<= 4;
            if (c >= '0' && c <= '9') {
                code |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                code |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                code |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool readString(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        while (!atEnd()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (atEnd()) return false;
            switch (text_[pos_++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned code = 0;
                    if (!readHex4(code)) return false;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        // A high surrogate needs its low half; anything else becomes U+FFFD.
                        unsigned low = 0;
                        const std::size_t saved = pos_;
                        if (consume('\\') && consume('u') && readHex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos_ = saved;
                            code = 0xFFFD;
                        }
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        code = 0xFFFD;
                    }
                    appendUtf8(out, code);
                    break;
                }
                default: return false;
            }
        }
        return false;  // unterminated
    }

    bool readNumber(Json& out) {
        const std::size_t start = pos_;
        consume('-');
        if (consume('0')) {
            // no leading zeros
        } else if (!readDigits()) {
            return false;
        }
        bool fractional = false;
        if (consume('.')) {
            fractional = true;
            if (!readDigits()) return false;
        }
        if (!atEnd() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            fractional = true;
            ++pos_;
            if (!atEnd() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (!readDigits()) return false;
        }
        const std::string token(text_.substr(start, pos_ - start));
        out.type = Json::Type::Number;
        out.number = std::strtod(token.c_str(), nullptr);
        if (!fractional) {
            long long whole = 0;
            const auto result = std::from_chars(token.data(), token.data() + token.size(), whole);
            out.integral = result.ec == std::errc() && whole >= INT_MIN && whole <= INT_MAX;
            out.integer = whole;
        }
        return true;
    }

    bool readDigits() {
        const std::size_t start = pos_;
        while (!atEnd() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        return pos_ > start;
    }
};

// Each reader below leaves `out` alone when the key is missing and returns false for a wrong type.
bool readInt(const Json& object, std::string_view key, int& out) {
    const Json* v = object.find(key);
    if (!v) return true;
    if (v->type != Json::Type::Number || !v->integral) return false;
    out = static_cast<int>(v->integer);
    return true;
}

bool readDouble(const Json& object, std::string_view key, double& out) {
    const Json* v = object.find(key);
    if (!v) return true;
    if (v->type != Json::Type::Number) return false;
    out = v->number;
    return true;
}

bool readOptionalInt(const Json& object, std::string_view key, std::optional<int>& out) {
    const Json* v = object.find(key);
    if (!v || v->type == Json::Type::Null) {
        out.reset();
        return true;
    }
    if (v->type != Json::Type::Number || !v->integral) return false;
    out = static_cast<int>(v->integer);
    return true;
}

bool readString(const Json& object, std::string_view key, std::string& out) {
    const Json* v = object.find(key);
    if (!v) return true;
    if (v->type != Json::Type::String) return false;
    out = v->text;
    return true;
}

bool readBests(const Json& object, PersonalBests& out) {
    return readOptionalInt(object, "sprintMs", out.sprintMs) && readOptionalInt(object, "cheeseMs", out.cheeseMs) &&
           readOptionalInt(object, "blitzScore", out.blitzScore);
}

bool readCareer(const Json& object, CareerView& out) {
    if (!readInt(object, "duelWins", out.duelWins) || !readInt(object, "duelLosses", out.duelLosses) ||
        !readInt(object, "duelDraws", out.duelDraws) || !readInt(object, "kosDealt", out.kosDealt) ||
        !readInt(object, "kosTaken", out.kosTaken) || !readInt(object, "royaleMatches", out.royaleMatches) ||
        !readInt(object, "royaleWins", out.royaleWins) || !readDouble(object, "pps", out.pps) ||
        !readDouble(object, "apm", out.apm) || !readDouble(object, "vs", out.vs)) {
        return false;
    }
    const Json* place = object.find("avgPlace");
    if (place && place->type != Json::Type::Null) {
        if (place->type != Json::Type::Number) return false;
        out.avgPlace = place->number;
    }
    return true;
}

std::string formatted(const char* format, double a) {
    char buf[64];
    std::snprintf(buf, sizeof buf, format, a);
    return buf;
}

}  // namespace

AccountRoute routeFor(AccountCall call) {
    switch (call) {
        case AccountCall::Me: return {"GET", "/api/me"};
        case AccountCall::Register: return {"POST", "/api/register"};
        case AccountCall::Login: return {"POST", "/api/login"};
        case AccountCall::Logout: return {"POST", "/api/logout"};
        case AccountCall::Password: return {"POST", "/api/password"};
        case AccountCall::Settings: return {"PUT", "/api/settings"};
        case AccountCall::Best: return {"POST", "/api/bests"};
    }
    return {"GET", "/api/me"};
}

std::string jsonQuote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(byte));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

std::string credentialsBody(std::string_view username, std::string_view password) {
    return "{\"username\":" + jsonQuote(username) + ",\"password\":" + jsonQuote(password) + "}";
}

std::string passwordBody(std::string_view current, std::string_view next) {
    return "{\"current\":" + jsonQuote(current) + ",\"next\":" + jsonQuote(next) + "}";
}

std::optional<std::string> bestBody(SoloMode mode, int result) {
    const char* name = nullptr;
    switch (mode) {
        case SoloMode::Sprint: name = "sprint"; break;
        case SoloMode::Cheese: name = "cheese"; break;
        case SoloMode::Blitz: name = "blitz"; break;
        case SoloMode::Marathon:
        case SoloMode::Zen: return std::nullopt;
    }
    return std::string("{\"mode\":\"") + name + "\",\"result\":" + std::to_string(result) + "}";
}

std::optional<Profile> parseProfile(std::string_view json) {
    const auto root = JsonReader(json).readDocument();
    if (!root || root->type != Json::Type::Object) return std::nullopt;
    const Json* name = root->find("username");
    if (!name || name->type != Json::Type::String) return std::nullopt;

    Profile profile;
    profile.username = name->text;
    if (!readString(*root, "settings", profile.settings) || !readBests(*root, profile.bests)) return std::nullopt;
    if (const Json* career = root->find("career")) {
        if (career->type != Json::Type::Object || !readCareer(*career, profile.career)) return std::nullopt;
    }
    return profile;
}

std::optional<PersonalBests> parseBests(std::string_view json) {
    const auto root = JsonReader(json).readDocument();
    if (!root || root->type != Json::Type::Object) return std::nullopt;
    PersonalBests bests;
    if (!readBests(*root, bests)) return std::nullopt;
    return bests;
}

std::vector<std::string> recordLines(const CareerView& career) {
    std::vector<std::string> lines;
    lines.push_back("Duels " + std::to_string(career.duelWins) + "-" + std::to_string(career.duelLosses) + "-" +
                    std::to_string(career.duelDraws));
    lines.push_back("KOs " + std::to_string(career.kosDealt) + " dealt, " + std::to_string(career.kosTaken) +
                    " taken");
    lines.push_back("Royale " + std::to_string(career.royaleWins) + " wins in " +
                    std::to_string(career.royaleMatches));
    if (career.avgPlace) lines.push_back("Avg place " + formatted("%.1f", *career.avgPlace));
    lines.push_back("PPS " + formatted("%.2f", career.pps) + "  APM " + formatted("%.1f", career.apm) + "  VS " +
                    formatted("%.1f", career.vs));
    return lines;
}

AccountSync::Outcome signedIn(AccountSync& sync, const Profile& profile) {
    sync.ignoreStartupMe = true;
    AccountSync::Outcome out;
    out.action = AccountSync::Action::SignIn;
    out.profile = profile;
    return out;
}

std::vector<AccountRequest> AccountSync::noteEnabled(bool accountsEnabled) {
    if (!accountsEnabled || startupOutstanding) return {};
    startupOutstanding = true;
    return {AccountRequest{AccountCall::Me, ""}};
}

std::vector<AccountRequest> AccountSync::noteReturnedToStart(bool loggedIn) {
    if (!loggedIn) return {};
    return {AccountRequest{AccountCall::Me, ""}};
}

std::vector<AccountRequest> AccountSync::save(bool loggedIn, const std::string& text) {
    std::vector<AccountRequest> out;
    if (!loggedIn) return out;
    const bool inFlight = !settingsInFlight.empty() && settingsInFlight.back() == epoch;
    if (inFlight) {
        if (text != settingsSent) settingsWaiting = text;
        else settingsWaiting.reset();
    } else {
        sendSettings(text, out);
    }
    queueBestRetries(out);
    return out;
}

std::vector<AccountRequest> AccountSync::submitBest(bool loggedIn, SoloMode mode, int result) {
    if (!loggedIn) return {};
    const auto body = bestBody(mode, result);
    if (!body) return {};
    bestsInFlight.push_back(PendingBest{mode, result, epoch});
    return {AccountRequest{AccountCall::Best, *body}};
}

void AccountSync::onEnteredAccount() { ignoreStartupMe = true; }

void AccountSync::onLogoutSent() {
    ignoreStartupMe = true;
    logoutPending = true;
    ++epoch;
    settingsWaiting.reset();
    bestsWaiting.clear();
}

void AccountSync::sendSettings(const std::string& text, std::vector<AccountRequest>& out) {
    settingsSent = text;
    settingsWaiting.reset();
    settingsInFlight.push_back(epoch);
    out.push_back(AccountRequest{AccountCall::Settings, text});
}

void AccountSync::queueBestRetries(std::vector<AccountRequest>& out) {
    for (const PendingBest& best : bestsWaiting) {
        if (best.epoch != epoch) continue;
        const auto body = bestBody(best.mode, best.result);
        if (!body) continue;
        bestsInFlight.push_back(PendingBest{best.mode, best.result, epoch});
        out.push_back(AccountRequest{AccountCall::Best, *body});
    }
    bestsWaiting.clear();
}

void AccountSync::rememberBest(PendingBest best) {
    for (PendingBest& waiting : bestsWaiting) {
        if (waiting.mode == best.mode) {
            waiting = best;
            return;
        }
    }
    bestsWaiting.push_back(best);
}

AccountSync::Outcome AccountSync::takeMe(const AccountReply& reply, bool loggedIn) {
    if (startupOutstanding) {
        startupOutstanding = false;
        if (ignoreStartupMe || reply.status != 200) return {};
        if (const auto profile = parseProfile(reply.body)) return signedIn(*this, *profile);
        return {};
    }
    if (!loggedIn || reply.status != 200) return {};
    if (const auto profile = parseProfile(reply.body)) {
        Outcome out;
        out.action = Action::Refresh;
        out.profile = *profile;
        return out;
    }
    return {};
}

AccountSync::Outcome AccountSync::takeSettings(bool loggedIn) {
    if (settingsInFlight.empty()) return {};
    const int sentEpoch = settingsInFlight.front();
    settingsInFlight.erase(settingsInFlight.begin());
    if (sentEpoch != epoch) return {};
    Outcome out;
    if (loggedIn && settingsWaiting && *settingsWaiting != settingsSent) {
        const std::string next = *settingsWaiting;
        sendSettings(next, out.more);
    } else {
        settingsWaiting.reset();
    }
    return out;
}

AccountSync::Outcome AccountSync::takeBest(const AccountReply& reply, bool loggedIn) {
    if (bestsInFlight.empty()) return {};
    const PendingBest sent = bestsInFlight.front();
    bestsInFlight.erase(bestsInFlight.begin());
    if (sent.epoch != epoch) return {};
    if (reply.status == 200 && loggedIn) {
        if (const auto bests = parseBests(reply.body)) {
            std::vector<PendingBest> kept;
            for (const PendingBest& waiting : bestsWaiting)
                if (waiting.mode != sent.mode) kept.push_back(waiting);
            bestsWaiting = std::move(kept);
            Outcome out;
            out.action = Action::Bests;
            out.bests = *bests;
            return out;
        }
    }
    if (loggedIn) rememberBest(sent);
    return {};
}

AccountSync::Outcome AccountSync::take(const AccountReply& reply, bool loggedIn) {
    switch (reply.call) {
    case AccountCall::Me:
        return takeMe(reply, loggedIn);
    case AccountCall::Register:
    case AccountCall::Login:
        if (reply.status == 200) {
            if (const auto profile = parseProfile(reply.body)) return signedIn(*this, *profile);
        }
        return Outcome{Action::LoginFailed, {}, {}, accountErrorText(reply.status, reply.body), {}};
    case AccountCall::Logout: {
        logoutPending = false;
        if (reply.status == 204) return {};
        return Outcome{Action::LogoutFailed, {}, {}, accountErrorText(reply.status, reply.body), {}};
    }
    case AccountCall::Password:
        if (reply.status == 204) return Outcome{Action::PasswordChanged, {}, {}, {}, {}};
        return Outcome{Action::PasswordFailed, {}, {}, accountErrorText(reply.status, reply.body), {}};
    case AccountCall::Settings:
        return takeSettings(loggedIn);
    case AccountCall::Best:
        return takeBest(reply, loggedIn);
    }
    return {};
}

std::string accountErrorText(int status, std::string_view body) {
    if (status == 0) return "cannot reach the server";
    constexpr std::string_view kSpace = " \t\r\n";
    const auto first = body.find_first_not_of(kSpace);
    if (first != std::string_view::npos) {
        return std::string(body.substr(first, body.find_last_not_of(kSpace) - first + 1));
    }
    return "error " + std::to_string(status);
}

}  // namespace tetris
