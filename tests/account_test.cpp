#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "app/account.hpp"
#include "app/settings.hpp"

using namespace tetris;

TEST_CASE("account: routes") {
    CHECK(std::string(routeFor(AccountCall::Me).method) == "GET");
    CHECK(std::string(routeFor(AccountCall::Settings).method) == "PUT");
    CHECK(std::string(routeFor(AccountCall::Best).path) == "/api/bests");
    CHECK(std::string(routeFor(AccountCall::Password).path) == "/api/password");
}

TEST_CASE("account: credentials escape quotes, backslashes and control bytes") {
    CHECK(credentialsBody("Sam", "pa\"ss\\w\n\x01rd") ==
          R"({"username":"Sam","password":"pa\"ss\\w\n\u0001rd"})");
    CHECK(credentialsBody("Sam", "p\xC3\xA9ssword") == "{\"username\":\"Sam\",\"password\":\"p\xC3\xA9ssword\"}");
}

TEST_CASE("account: a best request names the mode") {
    CHECK(bestBody(SoloMode::Sprint, 61234) == R"({"mode":"sprint","result":61234})");
    CHECK(bestBody(SoloMode::Cheese, 5) == R"({"mode":"cheese","result":5})");
    CHECK(bestBody(SoloMode::Blitz, 90000) == R"({"mode":"blitz","result":90000})");
    CHECK_FALSE(bestBody(SoloMode::Marathon, 1));
    CHECK_FALSE(bestBody(SoloMode::Zen, 1));
}

TEST_CASE("account: a profile parses, settings text included") {
    const auto p = parseProfile(
        R"({"username":"Sam","settings":"version=1\nsound=off\n","sprintMs":61234,"cheeseMs":null,"blitzScore":90000,)"
        R"("career":{"duelWins":3,"duelLosses":1,"duelDraws":0,"kosDealt":9,"kosTaken":4,"royaleMatches":5,)"
        R"("royaleWins":2,"avgPlace":2.6,"pps":1.52,"apm":45.3,"vs":98.7}})");
    REQUIRE(p);
    CHECK(p->username == "Sam");
    CHECK(p->settings == "version=1\nsound=off\n");
    CHECK(p->bests.sprintMs == 61234);
    CHECK_FALSE(p->bests.cheeseMs);
    CHECK(p->bests.blitzScore == 90000);
    CHECK(p->career.duelWins == 3);
    CHECK(p->career.royaleWins == 2);
    REQUIRE(p->career.avgPlace);
    CHECK(*p->career.avgPlace == doctest::Approx(2.6));
    CHECK(p->career.vs == doctest::Approx(98.7));
}

TEST_CASE("account: a new account's empty settings load as the defaults") {
    const auto p = parseProfile(R"({"username":"Sam","settings":"","sprintMs":null,"cheeseMs":null,)"
                                R"("blitzScore":null,"career":{"avgPlace":null,"pps":0,"apm":0,"vs":0}})");
    REQUIRE(p);
    CHECK(parseSettings(p->settings).keys.keys == defaultKeymap().keys);
    CHECK_FALSE(p->career.avgPlace);
}

TEST_CASE("account: malformed replies are rejected") {
    CHECK_FALSE(parseProfile(""));
    CHECK_FALSE(parseProfile("{"));
    CHECK_FALSE(parseProfile(R"({"settings":""})"));  // no username
    CHECK_FALSE(parseBests("not json"));
    CHECK(parseBests(R"({"sprintMs":null,"cheeseMs":7,"blitzScore":null})")->cheeseMs == 7);
}

TEST_CASE("account: the record block") {
    CareerView c{3, 1, 0, 9, 4, 5, 2, 2.6, 1.523, 45.27, 98.66};
    CHECK(recordLines(c) == std::vector<std::string>{"Duels 3-1-0", "KOs 9 dealt, 4 taken", "Royale 2 wins in 5",
                                                     "Avg place 2.6", "PPS 1.52  APM 45.3  VS 98.7"});
    c.avgPlace.reset();
    CHECK(recordLines(c).size() == 4);  // no Avg place line before a royale
}

const char* kProfile =
    R"({"username":"Sam","settings":"version=1\nsound=off\n","sprintMs":1000,"cheeseMs":null,"blitzScore":null,)"
    R"("career":{"duelWins":4,"duelLosses":1,"duelDraws":0,"kosDealt":9,"kosTaken":4,"royaleMatches":5,)"
    R"("royaleWins":2,"avgPlace":2.6,"pps":1.52,"apm":45.3,"vs":98.7}})";

const char* kBests = R"({"sprintMs":1000,"cheeseMs":null,"blitzScore":null})";

AccountReply reply(AccountCall call, int status, std::string body = {}) {
    return AccountReply{call, status, std::move(body)};
}

bool hasCall(const std::vector<AccountRequest>& reqs, AccountCall call, const std::string& body) {
    for (const AccountRequest& req : reqs)
        if (req.call == call && req.body == body) return true;
    return false;
}

TEST_CASE("account: the first frame on Start is not a return") {
    bool wasOnStart = true;
    CHECK_FALSE(returnedToStart(true, wasOnStart));
    CHECK_FALSE(returnedToStart(false, wasOnStart));
    CHECK(returnedToStart(true, wasOnStart));
    CHECK_FALSE(returnedToStart(true, wasOnStart));
}

TEST_CASE("account: a failed login still lets the startup Me sign in") {
    AccountSync sync;
    const auto start = sync.noteEnabled(true);
    REQUIRE(start.size() == 1);
    CHECK(start[0].call == AccountCall::Me);

    const auto failed = sync.take(reply(AccountCall::Login, 401, "wrong username or password\n"), false);
    CHECK(failed.action == AccountSync::Action::LoginFailed);
    CHECK(failed.error == "wrong username or password");
    CHECK_FALSE(sync.ignoreStartupMe);

    const auto me = sync.take(reply(AccountCall::Me, 200, kProfile), false);
    CHECK(me.action == AccountSync::Action::SignIn);
    CHECK(me.profile.username == "Sam");
    CHECK(sync.ignoreStartupMe);
}

TEST_CASE("account: a startup Me after a successful login is dropped, a refresh Me is not") {
    AccountSync sync;
    REQUIRE(sync.noteEnabled(true).size() == 1);
    const auto loggedIn = sync.take(reply(AccountCall::Login, 200, kProfile), false);
    CHECK(loggedIn.action == AccountSync::Action::SignIn);
    CHECK(sync.ignoreStartupMe);

    const auto startup = sync.take(reply(AccountCall::Me, 200, kProfile), true);
    CHECK(startup.action == AccountSync::Action::None);

    const auto refresh = sync.noteReturnedToStart(true);
    REQUIRE(refresh.size() == 1);
    CHECK(refresh[0].call == AccountCall::Me);
    CHECK(sync.noteReturnedToStart(false).empty());

    const auto applied = sync.take(reply(AccountCall::Me, 200, kProfile), true);
    CHECK(applied.action == AccountSync::Action::Refresh);
    CHECK(applied.profile.career.duelWins == 4);
    CHECK(applied.profile.bests.sprintMs == 1000);
}

TEST_CASE("account: a refresh Me after logout does not sign back in") {
    AccountSync sync;
    REQUIRE_FALSE(sync.noteReturnedToStart(false).size());
    sync.noteReturnedToStart(true);
    const auto late = sync.take(reply(AccountCall::Me, 200, kProfile), false);
    CHECK(late.action == AccountSync::Action::None);
}

TEST_CASE("account: one settings PUT at a time, then the latest text") {
    AccountSync sync;
    const auto first = sync.save(true, "one");
    REQUIRE(first.size() == 1);
    CHECK(first[0].body == "one");
    CHECK(sync.save(true, "two").empty());
    CHECK(sync.save(true, "one").empty());  // back to the body already in flight
    const auto same = sync.take(reply(AccountCall::Settings, 204), true);
    CHECK(same.more.empty());

    CHECK(sync.save(true, "two").size() == 1);
    const auto queued = sync.save(true, "three");
    CHECK(queued.empty());
    const auto flushed = sync.take(reply(AccountCall::Settings, 500, "internal error\n"), true);
    REQUIRE(flushed.more.size() == 1);
    CHECK(flushed.more[0].call == AccountCall::Settings);
    CHECK(flushed.more[0].body == "three");
    CHECK(sync.save(false, "guest").empty());
}

TEST_CASE("account: a settings reply from before logout does not free the next PUT") {
    AccountSync sync;
    REQUIRE(sync.save(true, "one").size() == 1);
    sync.onLogoutSent();
    const auto next = sync.save(true, "two");
    REQUIRE(next.size() == 1);
    CHECK(next[0].body == "two");
    CHECK(sync.take(reply(AccountCall::Settings, 204), true).more.empty());
    CHECK(sync.save(true, "three").empty());
    const auto done = sync.take(reply(AccountCall::Settings, 204), true);
    REQUIRE(done.more.size() == 1);
    CHECK(done.more[0].body == "three");
}

TEST_CASE("account: a failed best is sent again on the next save until a 200") {
    AccountSync sync;
    const auto sent = sync.submitBest(true, SoloMode::Sprint, 100);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].body == R"({"mode":"sprint","result":100})");
    CHECK(sync.take(reply(AccountCall::Best, 503, "nope\n"), true).action == AccountSync::Action::None);

    const auto again = sync.save(true, "A");
    REQUIRE(again.size() == 2);
    CHECK(again[0].call == AccountCall::Settings);
    CHECK(again[1].body == R"({"mode":"sprint","result":100})");

    const auto ok = sync.take(reply(AccountCall::Best, 200, kBests), true);
    CHECK(ok.action == AccountSync::Action::Bests);
    CHECK(ok.bests.sprintMs == 1000);  // the 200 body replaces the bests, whatever was posted
    const auto later = sync.save(true, "B");
    CHECK_FALSE(hasCall(later, AccountCall::Best, R"({"mode":"sprint","result":100})"));
}

TEST_CASE("account: a 200 that does not parse is retried, Marathon and Zen are not") {
    AccountSync sync;
    CHECK(sync.submitBest(true, SoloMode::Marathon, 5).empty());
    CHECK(sync.submitBest(true, SoloMode::Zen, 5).empty());
    CHECK(sync.submitBest(false, SoloMode::Sprint, 5).empty());
    REQUIRE(sync.submitBest(true, SoloMode::Cheese, 8).size() == 1);
    CHECK(sync.take(reply(AccountCall::Best, 200, "nope"), true).action == AccountSync::Action::None);
    CHECK(hasCall(sync.save(true, "A"), AccountCall::Best, R"({"mode":"cheese","result":8})"));
}

TEST_CASE("account: a best reply from before logout is not the next session's") {
    AccountSync sync;
    REQUIRE(sync.submitBest(true, SoloMode::Sprint, 10).size() == 1);
    sync.onLogoutSent();
    REQUIRE(sync.submitBest(true, SoloMode::Cheese, 20).size() == 1);
    CHECK(sync.take(reply(AccountCall::Best, 200, kBests), true).action == AccountSync::Action::None);
    CHECK(sync.take(reply(AccountCall::Best, 500, "x\n"), true).action == AccountSync::Action::None);
    const auto again = sync.save(true, "A");
    CHECK(hasCall(again, AccountCall::Best, R"({"mode":"cheese","result":20})"));
    CHECK_FALSE(hasCall(again, AccountCall::Best, R"({"mode":"sprint","result":10})"));
}

TEST_CASE("account: logout failure is an error and rooms wait until the reply") {
    AccountSync sync;
    sync.onLogoutSent();
    CHECK(sync.logoutPending);
    CHECK(sync.ignoreStartupMe);
    const auto failed = sync.take(reply(AccountCall::Logout, 500, "internal error\n"), false);
    CHECK(failed.action == AccountSync::Action::LogoutFailed);
    CHECK(failed.error == "internal error");
    CHECK_FALSE(sync.logoutPending);

    sync.onLogoutSent();
    const auto ok = sync.take(reply(AccountCall::Logout, 204), false);
    CHECK(ok.action == AccountSync::Action::None);
    CHECK(ok.error.empty());
    CHECK_FALSE(sync.logoutPending);
}

TEST_CASE("account: password replies") {
    AccountSync sync;
    CHECK(sync.take(reply(AccountCall::Password, 204), true).action == AccountSync::Action::PasswordChanged);
    const auto bad = sync.take(reply(AccountCall::Password, 403, "wrong password\n"), true);
    CHECK(bad.action == AccountSync::Action::PasswordFailed);
    CHECK(bad.error == "wrong password");
}

TEST_CASE("account: the web fetch bypasses the HTTP cache") {
    std::ifstream in(TETRIS_SOURCE_DIR "/src/app/account_store_web.cpp");
    REQUIRE(in);
    std::stringstream text;
    text << in.rdbuf();
    CHECK(text.str().find("cache: \"no-store\"") != std::string::npos);
}

TEST_CASE("account: error text") {
    CHECK(accountErrorText(0, "") == "cannot reach the server");
    CHECK(accountErrorText(429, "try again later\n") == "try again later");
    CHECK(accountErrorText(409, "username is taken\n") == "username is taken");
    CHECK(accountErrorText(500, "") == "error 500");
}
