#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "net/match_game.hpp"
#include "net/protocol.hpp"

// A scripted random match, shared by the replay tests and the golden fixtures for the server's simulation. Draws
// use rng() % n rather than <random> distributions, whose output differs between standard libraries.

// The match a script plays: its seed, start level 1, players {1, 2} for a duel or {1, 2, 3} for a royale.
inline tetris::MatchSetup scriptSetup(std::uint32_t seed, bool royale) {
    tetris::MatchSetup setup;
    setup.start = tetris::StartMatch{seed, 1, royale ? std::vector<int>{1, 2, 3} : std::vector<int>{1, 2}};
    return setup;
}

// `count` random inputs and soft-drop changes over `durationMs`, plus a garbage batch of 1-4 lines about every
// 5 s (ids 1, 2, ...), all from std::mt19937(seed) and ordered by t. The entries are what a player does, not a
// log: a SoftOn while soft drop is already on, or an input during a freeze, logs nothing.
inline std::vector<tetris::LogEntry> scriptEvents(std::uint32_t seed, int count = 3000,
                                                  int durationMs = tetris::kMatchMs) {
    using tetris::LogCode;
    std::mt19937 rng(seed);
    std::vector<tetris::LogEntry> events;
    for (int i = 0; i < count; ++i) {
        const int t = static_cast<int>(rng() % static_cast<std::uint32_t>(durationMs));
        const std::uint32_t pick = rng() % 20;
        LogCode code = LogCode::Left;
        if (pick < 3) code = LogCode::Left;
        else if (pick < 6) code = LogCode::Right;
        else if (pick < 9) code = LogCode::RotateCW;
        else if (pick < 11) code = LogCode::RotateCCW;
        else if (pick < 12) code = LogCode::Hold;
        else if (pick < 13) code = LogCode::LeftWall;
        else if (pick < 14) code = LogCode::RightWall;
        else if (pick < 16) code = LogCode::HardDrop;
        else if (pick < 18) code = LogCode::SoftOn;
        else code = LogCode::SoftOff;
        events.push_back({t, code});
    }
    for (int id = 1; id * 5000 < durationMs; ++id) {
        const int t = std::clamp(id * 5000 + static_cast<int>(rng() % 1001) - 500, 0, durationMs - 1);
        events.push_back({t, LogCode::Garbage, id, 1 + static_cast<int>(rng() % 4)});
    }
    std::stable_sort(events.begin(), events.end(),
                     [](const tetris::LogEntry& a, const tetris::LogEntry& b) { return a.t < b.t; });
    return events;
}

// Plays `events` live up to t == endMs in frames of `frameMs`: each frame first applies, at the frame's start t,
// every event due before the frame ends, then advances. frameMs 1 applies each event at its own t; frameMs >=
// endMs applies them all before one advance. Stops early once eliminated when `stopWhenEliminated` is set.
inline void runScript(tetris::MatchGame& match, const std::vector<tetris::LogEntry>& events, int frameMs, int endMs,
                      bool stopWhenEliminated = false) {
    std::size_t next = 0;
    while (match.t() < endMs && !(stopWhenEliminated && match.eliminated())) {
        const int ms = std::min(frameMs, endMs - match.t());
        for (; next < events.size() && events[next].t < match.t() + ms; ++next) match.apply(events[next]);
        match.advance(ms);
    }
}
