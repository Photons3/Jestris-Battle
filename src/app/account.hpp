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

// One call the account sync wants sent. `body` is empty for GET and logout.
struct AccountRequest {
    AccountCall call = AccountCall::Me;
    std::string body;
};

// Which Me is the startup one, one settings PUT at a time, and a best that has not yet got a 200.
// Replies are applied in arrival order, which is the order the requests were sent.
struct AccountSync {
    // Set once a register or login has succeeded, or Logout is sent. Drops the startup Me only.
    bool ignoreStartupMe = false;
    bool logoutPending = false;  // Logout was sent and its reply has not arrived

    std::vector<AccountRequest> noteEnabled(bool accountsEnabled);       // the constructor's Me, once
    std::vector<AccountRequest> noteReturnedToStart(bool loggedIn);      // refresh Me, record and bests only
    std::vector<AccountRequest> save(bool loggedIn, const std::string& text);
    std::vector<AccountRequest> submitBest(bool loggedIn, SoloMode mode, int result);
    void onEnteredAccount();
    void onLogoutSent();

    enum class Action { None, SignIn, Refresh, LoginFailed, LogoutFailed, PasswordChanged, PasswordFailed, Bests };
    struct Outcome {
        Action action = Action::None;
        Profile profile;      // SignIn and Refresh
        PersonalBests bests;  // Bests
        std::string error;    // LoginFailed, LogoutFailed, PasswordFailed
        std::vector<AccountRequest> more;
    };
    Outcome take(const AccountReply& reply, bool loggedIn);

private:
    struct PendingBest {
        SoloMode mode = SoloMode::Marathon;
        int result = 0;
        int epoch = 0;  // which login the request belonged to
    };

    bool startupOutstanding = false;
    int epoch = 0;  // bumped on logout so a late reply is not the next session's
    std::vector<int> settingsInFlight;
    std::string settingsSent;
    std::optional<std::string> settingsWaiting;
    std::vector<PendingBest> bestsInFlight;
    std::vector<PendingBest> bestsWaiting;

    void sendSettings(const std::string& text, std::vector<AccountRequest>& out);
    void queueBestRetries(std::vector<AccountRequest>& out);
    void rememberBest(PendingBest best);
    Outcome takeMe(const AccountReply& reply, bool loggedIn);
    Outcome takeSettings(bool loggedIn);
    Outcome takeBest(const AccountReply& reply, bool loggedIn);
};

// True when `onStart` becomes true. `wasOnStart` begins true, so the first frame is not a return to Start.
inline bool returnedToStart(bool onStart, bool& wasOnStart) {
    const bool returned = onStart && !wasOnStart;
    wasOnStart = onStart;
    return returned;
}

}  // namespace tetris
