#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "match_script.hpp"

using namespace tetris;

namespace {

// The wire token of `entry`, taken from the protocol's own encoding of a one-entry IN batch ("IN <t> <t>:<token>").
std::string tokenOf(const LogEntry& entry) {
    const std::string text = encode(InputBatch{entry.t, {entry}});
    return text.substr(text.find(':') + 1);
}

std::string hex16(std::uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(value));
    return buf;
}

// The fixture text for one scripted match: setup, the events in file order, a check every 10000 ms and one at the
// end (before any event stamped the same t). Each check is computed by replaying the lines above it exactly as a
// reader does: an ev advances to its t and applies the token, a check advances to its t and compares.
std::string makeFixture(std::uint32_t seed, bool royale) {
    const MatchSetup setup = scriptSetup(seed, royale);
    // Fewer events and sparser garbage than the replay tests use, so duels clear lines and royales outlive the 30 s
    // floor rise.
    const std::vector<LogEntry> events = scriptEvents(seed, royale ? 500 : 1200, tetris::kMatchMs, royale ? 15000 : 10000);
    int endMs = tetris::kMatchMs;
    if (royale) {  // until elimination, or the full clock
        MatchGame probe(setup);
        runScript(probe, events, 1, tetris::kMatchMs, true);
        endMs = probe.t();
    }
    std::ostringstream out;
    out << "setup " << seed << " " << setup.start.startLevel << " " << setup.start.players.size() << " "
        << setup.softDropFactor << "\n";
    MatchGame match(setup);
    int attackTotal = 0;
    auto check = [&](int t) {
        match.advance(t - match.t());
        attackTotal += match.game().takeOutgoingAttack();
        out << "check " << t << " " << attackTotal << " " << match.topOuts() << " " << hex16(match.hash()) << "\n";
    };
    int nextCheck = 10000;
    for (const LogEntry& e : events) {
        if (e.t >= endMs) break;
        for (; nextCheck <= e.t && nextCheck < endMs; nextCheck += 10000) check(nextCheck);
        match.advance(e.t - match.t());
        match.apply(e);
        out << "ev " << e.t << " " << tokenOf(e) << "\n";
    }
    for (; nextCheck < endMs; nextCheck += 10000) check(nextCheck);
    check(endMs);
    return out.str();
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

TEST_CASE("match_game fixtures: checked-in fixtures match the native replay") {
    struct Case {
        const char* file;
        std::uint32_t seed;
        bool royale;
    };
    const Case cases[] = {{"duel-7.txt", 7, false},
                          {"duel-1234.txt", 1234, false},
                          {"royale-7.txt", 7, true},
                          {"royale-99.txt", 99, true}};
    const char* update = std::getenv("UPDATE_FIXTURES");
    for (const Case& c : cases) {
        CAPTURE(c.file);
        const std::string path = std::string(TETRIS_SOURCE_DIR "/server/sim/testdata/") + c.file;
        const std::string text = makeFixture(c.seed, c.royale);
        if (update && std::string(update) == "1") {
            std::ofstream(path, std::ios::binary) << text;
        } else {
            CHECK(text == readFile(path));
        }
    }
}
