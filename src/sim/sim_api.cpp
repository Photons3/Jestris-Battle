// The server's replay module: jestris_sim.wasm, a standalone WASM build of MatchGame that the Go server runs
// (via wazero) to replay each player's game with the same code the browser plays. Handles index a table of
// matches; a bad handle traps, which the host sees as an error.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "net/match_game.hpp"
#include "net/protocol.hpp"

namespace {

using tetris::MatchGame;

constexpr int kBufferSize = 4096;
char g_buffer[kBufferSize];
std::vector<std::optional<MatchGame>> g_games;

MatchGame& at(int h) {
    if (h < 0 || h >= static_cast<int>(g_games.size()) || !g_games[h]) __builtin_trap();
    return *g_games[h];
}

}  // namespace

#define SIM_EXPORT(name) extern "C" __attribute__((export_name(name)))

SIM_EXPORT("sim_new") int sim_new(std::uint32_t seed, int level, int players, int sdf) {
    tetris::MatchSetup setup;
    setup.start.seed = seed;
    setup.start.startLevel = level;
    for (int id = 1; id <= players; ++id) setup.start.players.push_back(id);
    setup.softDropFactor = sdf;
    for (std::size_t h = 0; h < g_games.size(); ++h) {
        if (!g_games[h]) {
            g_games[h].emplace(setup, false);  // the server keeps the canonical log itself
            return static_cast<int>(h);
        }
    }
    g_games.emplace_back(std::in_place, setup, false);
    return static_cast<int>(g_games.size() - 1);
}

SIM_EXPORT("sim_free") void sim_free(int h) {
    at(h);
    g_games[h].reset();
}

SIM_EXPORT("sim_input") void sim_input(int h, int code) {
    MatchGame& match = at(h);
    if (code < 0 || code >= static_cast<int>(tetris::LogCode::Garbage)) __builtin_trap();
    tetris::LogEntry entry;
    entry.t = match.t();
    entry.code = static_cast<tetris::LogCode>(code);
    match.apply(entry);
}

SIM_EXPORT("sim_accept_garbage") void sim_accept_garbage(int h, int id, int lines) { at(h).acceptGarbage(id, lines); }

SIM_EXPORT("sim_advance") void sim_advance(int h, int ms) { at(h).advance(ms); }

SIM_EXPORT("sim_t") int sim_t(int h) { return at(h).t(); }

SIM_EXPORT("sim_take_attack") int sim_take_attack(int h) { return at(h).game().takeOutgoingAttack(); }

SIM_EXPORT("sim_top_outs") int sim_top_outs(int h) { return at(h).topOuts(); }

SIM_EXPORT("sim_eliminated") int sim_eliminated(int h) { return at(h).eliminated() ? 1 : 0; }

SIM_EXPORT("sim_buried") int sim_buried(int h) { return at(h).buried() ? 1 : 0; }

SIM_EXPORT("sim_hash") std::uint64_t sim_hash(int h) { return at(h).hash(); }

// The SNAP arguments (the encoding minus "SNAP "): 11 numbers of at most 11 characters, a few separators and
// 200 field cells come to well under 400 bytes, but trap rather than overflow if that ever changes.
SIM_EXPORT("sim_snapshot") int sim_snapshot(int h) {
    const std::string text = tetris::encode(tetris::snapshotOf(at(h).game()));
    constexpr std::size_t kPrefix = 5;  // "SNAP "
    if (text.size() < kPrefix || text.size() - kPrefix > kBufferSize) __builtin_trap();
    const std::size_t length = text.size() - kPrefix;
    std::memcpy(g_buffer, text.data() + kPrefix, length);
    return static_cast<int>(length);
}

// "<pieces> <attack> <garbageCleared> <cancelled> <tetrises> <tSpins> <maxCombo> <t>": eight numbers of at most
// 11 characters, so trap rather than overflow if that ever changes.
SIM_EXPORT("sim_stats") int sim_stats(int h) {
    const MatchGame& match = at(h);
    const tetris::GameStats& s = match.game().stats();
    const int length = std::snprintf(g_buffer, kBufferSize, "%d %d %d %d %d %d %d %d", s.pieces, s.attack,
                                     s.garbageCleared, s.cancelled, s.tetrises, s.tSpins, s.maxCombo, match.t());
    if (length < 0 || length >= kBufferSize) __builtin_trap();
    return length;
}

SIM_EXPORT("sim_buffer") char* sim_buffer() { return g_buffer; }

// STANDALONE_WASM with a growable memory otherwise imports this from "env"; the server provides only WASI, and
// nothing here caches the memory's address.
extern "C" void emscripten_notify_memory_growth(int) {}
