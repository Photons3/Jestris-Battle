#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/solo.hpp"

namespace tetris {

inline constexpr int kMinPasswordBytes = 8;   // counted in bytes, as the server counts them
inline constexpr int kMaxPasswordBytes = 72;  // bcrypt ignores everything past this

enum class AccountCall { Me, Register, Login, Logout, Password, Settings, Best };

struct AccountRoute {
    const char* method;
    const char* path;
};
AccountRoute routeFor(AccountCall call);

struct AccountReply {
    AccountCall call = AccountCall::Me;
    int status = 0;  // 0: the server could not be reached
    std::string body;
};

// `text` as a JSON string literal, quotes included. Bytes of 0x20 and above pass through unchanged.
std::string jsonQuote(std::string_view text);

std::string credentialsBody(std::string_view username, std::string_view password);
std::string passwordBody(std::string_view current, std::string_view next);
// nullopt for Marathon and Zen, which have no best.
std::optional<std::string> bestBody(SoloMode mode, int result);

// The server-written totals shown on the Record screen.
struct CareerView {
    int duelWins = 0, duelLosses = 0, duelDraws = 0;
    int kosDealt = 0, kosTaken = 0;
    int royaleMatches = 0, royaleWins = 0;
    std::optional<double> avgPlace;  // unset before the first royale
    double pps = 0, apm = 0, vs = 0;
};

struct Profile {
    std::string username;
    std::string settings;  // the saved settings text, empty for a new account
    PersonalBests bests;
    CareerView career;
};

// nullopt for anything that is not a well-formed reply.
std::optional<Profile> parseProfile(std::string_view json);
std::optional<PersonalBests> parseBests(std::string_view json);

std::vector<std::string> recordLines(const CareerView& career);

// The server's plain-text error without its trailing newline, or a stand-in when there is none.
std::string accountErrorText(int status, std::string_view body);

}  // namespace tetris
