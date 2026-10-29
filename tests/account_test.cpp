#include <doctest/doctest.h>

#include <string>
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

TEST_CASE("account: error text") {
    CHECK(accountErrorText(0, "") == "cannot reach the server");
    CHECK(accountErrorText(429, "try again later\n") == "try again later");
    CHECK(accountErrorText(409, "username is taken\n") == "username is taken");
    CHECK(accountErrorText(500, "") == "error 500");
}
