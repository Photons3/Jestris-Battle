# Player Names Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each player picks a display name on the start screen. It is saved between visits, sent to the server, and shown to the opponent on the duel screen in place of YOU / FOE.

**Architecture:**
- `src/net/protocol` gains the name rules (`isNameChar`, `isValidName`, `kMaxNameLength`) and two messages: `HELLO <name>` (client -> server) and `OPP <name>` (server -> client).
- `Settings` stores the name as `name=<name>`. The app fills a missing name with a random `PlayerNNNN` and saves it.
- The Go hub keeps a name per connection (default `Player`). On join it sends `OPP` to both seats before `START`.
- `DuelSession` remembers the opponent's name. The render code draws both names on the bars, a "you vs them" line under the countdown, and the name in the opponent-left result text.

**Tech Stack:** C++20, CMake ≥ 3.25, SDL3, Dear ImGui 1.92, doctest 2.4.12, Emscripten for the web build, Go 1.27 + `github.com/coder/websocket`.

**Spec:** `docs/superpowers/specs/2026-10-13-player-names-design.md`

## Global Constraints

- C++20. Every C++ target builds warning-free with `-Wall -Wextra -Wpedantic`. Desktop Debug builds keep ASan/UBSan.
- Names are 1–12 characters from `A-Z a-z 0-9 _ - .` (`kMaxNameLength = 12` in C++, `maxNameLength` in Go, each citing the other).
- The server is authoritative: an invalid or missing `HELLO` gives `Player`. `HELLO` inside a room is dropped. `OPP` is server-only and never relayed.
- `OPP` is sent once per room, immediately before the room's first `START`. Rematches don't resend it.
- Old clients and servers keep working: a client that never sends `HELLO` is `Player`; a client that never gets `OPP` shows `FOE`.
- Solo play and the desktop build behave as before. The desktop start screen gets no name field.
- Conventional commits scoped like the existing history (`feat(net): …`, `feat(server): …`, `feat(app): …`, `docs: …`).
- Work on `develop`, or on a feature branch off it (`feat/player-names`).

## Commands

- Desktop build and all C++ tests: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- One doctest group: `./build/tetris_tests -tc="protocol:*"` (wildcards allowed)
- Go tests: `go -C server test -race ./...` and `go -C server vet ./...`
- Web build: `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web` (prefix with `EMSDK_PYTHON=/opt/homebrew/bin/python3.14` if emcc complains about Python)

## File map

| File | Change |
|---|---|
| `src/net/protocol.hpp/.cpp` | `kMaxNameLength`, `isNameChar`, `isValidName`, `Hello`, `OpponentName` |
| `CMakeLists.txt` | `tetris_settings` links `tetris_duel` (for `isValidName`) |
| `src/app/settings.hpp/.cpp` | `Settings::playerName`, `name=` line, `defaultPlayerName` |
| `server/hub.go` | `player.name`, `HELLO`, `OPP` on join, `OPP` server-only |
| `src/app/duel.hpp/.cpp` | `opponentName()`, `OPP` in Waiting, name in `resultReasonText` |
| `src/app/render.hpp/.cpp` | `StartMenu` name fields, Name input, names on bars and countdown |
| `src/app/app.hpp/.cpp` | load/default/save the name, send `HELLO` in `openDuel`, `SaveName` command |
| `tests/protocol_test.cpp`, `tests/settings_test.cpp`, `tests/duel_test.cpp`, `server/hub_test.go` | tests per task |
| `README.md` | the Name field in "Playing a duel" |

---

### Task 1: Name rules and the HELLO / OPP messages

**Files:**
- Modify: `src/net/protocol.hpp`, `src/net/protocol.cpp`
- Test: `tests/protocol_test.cpp`

**Interfaces:**
- Produces: `inline constexpr int kMaxNameLength = 12;`, `bool isNameChar(char c);`, `bool isValidName(std::string_view text);`, `struct Hello { std::string name; };`, `struct OpponentName { std::string name; };`, both appended to `Message`.

- [ ] **Step 1: Write the failing tests**

Add `Hello{"Sam"}` and `OpponentName{"x_y-z.9"}` to the round-trip list in `"protocol: every message survives an encode/decode round trip"`, and append:

```cpp
TEST_CASE("protocol: name rules") {
    CHECK(isValidName("A"));
    CHECK(isValidName("Player4821"));
    CHECK(isValidName("abcdefghijkl"));  // 12
    CHECK(isValidName("a_b-c.9"));
    CHECK_FALSE(isValidName(""));
    CHECK_FALSE(isValidName("abcdefghijklm"));  // 13
    for (std::string_view bad : {"a b", "a:b", "a=b", "a\nb", "\xC3\xA9", "a/b", "a+b"}) {
        CAPTURE(bad);
        CHECK_FALSE(isValidName(bad));
    }
}

TEST_CASE("protocol: HELLO and OPP on the wire") {
    CHECK(encode(Hello{"Sam"}) == "HELLO Sam");
    CHECK(encode(OpponentName{"Kai"}) == "OPP Kai");
    for (std::string_view text : {"HELLO", "HELLO ", "HELLO a b", "HELLO a:b", "HELLO abcdefghijklm", "OPP",
                                  "OPP ", "OPP a b", "OPP a=b"}) {
        CAPTURE(text);
        CHECK_FALSE(decode(text).has_value());
    }
}
```

`"HELLO"` is already in the malformed list of `"protocol: malformed messages are rejected"`. It stays there, because a bare `HELLO` has no name.

- [ ] **Step 2: Run them and watch them fail**

`cmake --build build && ./build/tetris_tests -tc="protocol:*"` fails to compile (`Hello` is not declared).

- [ ] **Step 3: Implement**

In `src/net/protocol.hpp`, next to `kRoomCodeLength`:

```cpp
inline constexpr int kMaxNameLength = 12;  // maxNameLength in server/hub.go

// A display name is 1 to kMaxNameLength of A-Z a-z 0-9 _ - . (no spaces or ':', which the protocol uses).
bool isNameChar(char c);
bool isValidName(std::string_view text);
```

Add the structs after `ServerError`:

```cpp
struct Hello {  // the sender's display name; the first message on every connection
    std::string name;
    friend bool operator==(const Hello&, const Hello&) = default;
};
struct OpponentName {  // from the server, just before a room's first START
    std::string name;
    friend bool operator==(const OpponentName&, const OpponentName&) = default;
};
```

Append both to the `Message` variant (after `MatchOver`).

In `src/net/protocol.cpp`, outside the anonymous namespace:

```cpp
bool isNameChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.';
}

bool isValidName(std::string_view text) {
    return !text.empty() && static_cast<int>(text.size()) <= kMaxNameLength &&
           std::all_of(text.begin(), text.end(), isNameChar);
}
```

In `encode`, before the final `else`:

```cpp
            } else if constexpr (std::is_same_v<T, Hello>) {
                return "HELLO " + m.name;
            } else if constexpr (std::is_same_v<T, OpponentName>) {
                return "OPP " + m.name;
```

The final `else` keeps its `static_assert` for `MatchOver`. Move it so it stays the last branch.

In `decode`, before `else if (args == 0)`:

```cpp
    } else if (verb == "HELLO" && args == 1) {
        if (isValidName(parts[1])) return Hello{std::string(parts[1])};
    } else if (verb == "OPP" && args == 1) {
        if (isValidName(parts[1])) return OpponentName{std::string(parts[1])};
```

- [ ] **Step 4: Run the tests**

`./build/tetris_tests -tc="protocol:*"` passes.

`DuelSession::onMessage` ignores the new alternatives: its `if/else if` chain has no catch-all, so it still compiles.

- [ ] **Step 5: Commit**

```bash
git add src/net/protocol.hpp src/net/protocol.cpp tests/protocol_test.cpp
git commit -m "feat(net): name rules and HELLO/OPP messages"
```

---

### Task 2: Save the name in the settings

**Files:**
- Modify: `CMakeLists.txt` (`tetris_settings` links `tetris_duel`)
- Modify: `src/app/settings.hpp`, `src/app/settings.cpp`
- Test: `tests/settings_test.cpp`

**Interfaces:**
- Produces: `Settings::playerName` (empty = none saved); `std::string defaultPlayerName(std::uint32_t random);` returns `Player` plus 4 zero-padded digits.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("settings: the player name round-trips") {
    Settings settings;
    settings.playerName = "Sam_99";
    CHECK(parseSettings(serializeSettings(settings)).playerName == "Sam_99");
}

TEST_CASE("settings: a missing or invalid name loads as empty") {
    CHECK(parseSettings("version=1\n").playerName.empty());
    CHECK(parseSettings("version=1\nname=two words\n").playerName.empty());
    CHECK(parseSettings("version=1\nname=abcdefghijklm\n").playerName.empty());
    CHECK(parseSettings("version=1\nname=\n").playerName.empty());
}

TEST_CASE("settings: the default name is Player and four digits") {
    CHECK(defaultPlayerName(4821) == "Player4821");
    CHECK(defaultPlayerName(7) == "Player0007");
    CHECK(defaultPlayerName(123456) == "Player3456");
    CHECK(isValidName(defaultPlayerName(0xFFFFFFFFu)));
}
```

Add `#include "net/protocol.hpp"` at the top of the test file for `isValidName`.

- [ ] **Step 2: Run them and watch them fail**

`./build/tetris_tests -tc="settings:*"` fails to compile.

- [ ] **Step 3: Implement**

In `CMakeLists.txt`, change the `tetris_settings` link line to:

```cmake
target_link_libraries(tetris_settings PUBLIC tetris_input tetris_duel SDL3::SDL3)
```

In `src/app/settings.hpp`, add `#include <cstdint>` and:

```cpp
struct Settings {
    Keymap keys = defaultKeymap();
    bool muted = false;
    bool musicOff = false;
    std::string playerName;  // empty until one is chosen or generated
};

// "Player" and four digits taken from `random`, for a player who has not picked a name.
std::string defaultPlayerName(std::uint32_t random);
```

In `src/app/settings.cpp`, include `net/protocol.hpp`, `<cstdio>` and `<cstdint>`. In `serializeSettings`, after the `music=` line:

```cpp
    if (isValidName(settings.playerName)) out += "name=" + settings.playerName + "\n";
```

In `parseSettings`, after the `music` branch:

```cpp
        } else if (name == "name") {
            if (isValidName(value)) settings.playerName = std::string(value);
```

Add:

```cpp
std::string defaultPlayerName(std::uint32_t random) {
    char digits[5];
    std::snprintf(digits, sizeof digits, "%04u", static_cast<unsigned>(random % 10000));
    return "Player" + std::string(digits);
}
```

- [ ] **Step 4: Run all C++ tests**

`cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure` passes. The existing `isDefault` helper still works, because the defaults have an empty name.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/app/settings.hpp src/app/settings.cpp tests/settings_test.cpp
git commit -m "feat(app): save the player name in the settings"
```

---

### Task 3: Server names and OPP

**Files:**
- Modify: `server/hub.go`
- Test: `server/hub_test.go`

**Interfaces:**
- Consumes: `HELLO <name>` from clients.
- Produces: `OPP <name>` to each seat, before the `START` that a `JOIN` triggers.

- [ ] **Step 1: Update `pair` and write the failing tests**

In `server/hub_test.go`, `pair` now expects the names first:

```go
	b.send("JOIN AAAA")
	a.expect("OPP Player")
	b.expect("OPP Player")
	a.expect("START 101 3")
	b.expect("START 101 3")
```

Add `"OPP Zed"` to the forged list in `TestServerOnlyVerbsAreNotRelayed`, and append:

```go
func TestNamesAreSentBeforeStart(t *testing.T) {
	srv := newTestServer(t)
	a, b := dial(t, srv), dial(t, srv)
	a.send("HELLO Sam")
	a.send("CREATE 3")
	a.expect("ROOM AAAA")
	b.send("HELLO Kai")
	b.send("JOIN AAAA")
	a.expect("OPP Kai")
	a.expect("START 101 3")
	b.expect("OPP Sam")
	b.expect("START 101 3")
}

func TestBadHelloLeavesTheDefaultName(t *testing.T) {
	srv := newTestServer(t)
	a, b := dial(t, srv), dial(t, srv)
	a.send("HELLO two words")
	a.send("CREATE 3")
	a.expect("ROOM AAAA")
	b.send("HELLO abcdefghijklm")
	b.send("JOIN AAAA")
	a.expect("OPP Player")
	b.expect("OPP Player")
}

func TestHelloInsideARoomIsIgnored(t *testing.T) {
	srv := newTestServer(t)
	a, b := pair(t, srv)
	a.send("HELLO Zed") // dropped: a is in a room
	b.conn.Close(websocket.StatusNormalClosure, "")
	a.expect("LEFT")
	a.send("CREATE 1")
	a.expect("ROOM AAAB")
	c := dial(t, srv)
	c.send("JOIN AAAB")
	c.expect("OPP Player")
}
```

- [ ] **Step 2: Run them and watch them fail**

`go -C server test -race ./...` fails: `pair` times out waiting for `OPP Player`.

- [ ] **Step 3: Implement**

In `server/hub.go`, add these constants:

```go
	maxNameLength = 12       // kMaxNameLength in src/net/protocol.hpp
	defaultName   = "Player" // for clients that send no valid HELLO
```

`player` gains `name string`. In `ServeHTTP`: `p := &player{conn: conn, name: defaultName}`.

Update the `Hub` doc comment: "It handles HELLO, CREATE, JOIN, REMATCH, ATK and DEAD itself … the verbs only the server sends (ROOM, START, LEFT, ERR, KO, OVER, OPP) are dropped."

In `route`, right after `verb, arg, _ := strings.Cut(text, " ")`:

```go
	if verb == "HELLO" {
		if p.room == nil && validName(arg) {
			p.name = arg
		}
		return nil
	}
```

Add `"OPP"` to the server-only `case` list. Replace the last line of `join`:

```go
	out := []outgoing{{r.players[0], "OPP " + p.name}, {p, "OPP " + r.players[0].name}}
	return append(out, h.startMatch(r)...)
```

Add:

```go
// validName mirrors isValidName in src/net/protocol.cpp.
func validName(s string) bool {
	if len(s) == 0 || len(s) > maxNameLength {
		return false
	}
	for _, c := range []byte(s) {
		ok := c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_' || c == '-' || c == '.'
		if !ok {
			return false
		}
	}
	return true
}
```

- [ ] **Step 4: Run the Go tests**

`go -C server test -race ./... && go -C server vet ./...` passes.

- [ ] **Step 5: Commit**

```bash
git add server/hub.go server/hub_test.go
git commit -m "feat(server): player names via HELLO, sent to the opponent as OPP"
```

---

### Task 4: The session remembers the opponent's name

**Files:**
- Modify: `src/app/duel.hpp`, `src/app/duel.cpp`
- Test: `tests/duel_test.cpp`

**Interfaces:**
- Produces: `const std::string& DuelSession::opponentName() const` (empty until `OPP` arrives in Waiting; cleared by `reset()`). `resultReasonText` says "<name> left the match" when the name is known.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("duel: OPP while waiting names the opponent, and leaving forgets it") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("OPP Kai");
    CHECK(duel.opponentName() == "Kai");
    duel.onMessage("START 7 1");
    CHECK(duel.opponentName() == "Kai");
    duel.leave();
    CHECK(duel.opponentName().empty());
}

TEST_CASE("duel: OPP outside Waiting is ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.onMessage("OPP Kai");  // Idle
    CHECK(duel.opponentName().empty());
    startMatch(duel, wire);
    duel.onMessage("OPP Zed");  // Playing
    CHECK(duel.opponentName().empty());
}

TEST_CASE("duel: the opponent-left line uses the name when known") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("OPP Kai");
    duel.onMessage("START 7 1");
    duel.onMessage("LEFT");
    CHECK(resultReasonText(duel) == "Kai left the match");
}
```

The existing table entry `{{"LEFT"}, "Opponent left the match"}` stays as it is. It covers the no-name fallback.

- [ ] **Step 2: Run them and watch them fail**

`./build/tetris_tests -tc="duel:*"` fails to compile (`opponentName` is not a member).

- [ ] **Step 3: Implement**

In `duel.hpp`, after `opponentGone()`:

```cpp
    const std::string& opponentName() const { return opponentName_; }  // from OPP; empty if not sent
```

Add the member `std::string opponentName_;` after `roomCode_`.

In `duel.cpp` `onMessage`, after the `RoomCreated` branch:

```cpp
    } else if (const auto* opp = std::get_if<OpponentName>(&*message)) {
        if (phase_ == DuelPhase::Waiting) opponentName_ = opp->name;
```

In `reset()`, add `opponentName_.clear();`. In `resultReasonText`:

```cpp
    if (duel.outcome() == DuelOutcome::OpponentLeft)
        return (duel.opponentName().empty() ? std::string("Opponent") : duel.opponentName()) + " left the match";
```

- [ ] **Step 4: Run all C++ tests**

`ctest --test-dir build --output-on-failure` passes.

- [ ] **Step 5: Commit**

```bash
git add src/app/duel.hpp src/app/duel.cpp tests/duel_test.cpp
git commit -m "feat(app): remember the opponent's name from OPP"
```

---

### Task 5: Name field, HELLO and names on the duel screen

**Files:**
- Modify: `src/app/render.hpp`, `src/app/render.cpp`
- Modify: `src/app/app.hpp`, `src/app/app.cpp`
- Test: `tests/ui_test.cpp` (draw smoke test), manual checklist

**Interfaces:**
- `StartMenu` gains `std::string playerName` (committed, always valid) and `char nameEdit[kMaxNameLength + 1]` (the field's buffer).
- `UiCommand::SaveName`.
- `drawDuel(const DuelSession&, const Fonts&, const ComboLabel&, const StartMenu&)`. The menu supplies our name.

- [ ] **Step 1: StartMenu and the command**

In `render.hpp`, append `SaveName` to `UiCommand`. Add to `StartMenu`:

```cpp
    std::string playerName;                   // the saved display name, always valid
    char nameEdit[kMaxNameLength + 1] = {};   // the Name field while it is being edited
```

Change the `drawDuel` declaration to take `const StartMenu& menu` as a fourth parameter.

- [ ] **Step 2: The Name field**

In `render.cpp`, in the anonymous namespace:

```cpp
// Drops typed characters a name can't contain, so the field only ever holds a valid name or nothing.
int nameCharFilter(ImGuiInputTextCallbackData* data) {
    const ImWchar c = data->EventChar;
    return c < 128 && isNameChar(static_cast<char>(c)) ? 0 : 1;
}
```

In `drawStartScreen`, right after `ImGui::SeparatorText("1v1 DUEL");`:

```cpp
        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##name", "NAME", menu.nameEdit, sizeof menu.nameEdit,
                                 ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CharsNoBlank,
                                 nameCharFilter);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (menu.nameEdit[0] != '\0') {
                menu.playerName = menu.nameEdit;
                command = UiCommand::SaveName;
            } else {  // an empty name isn't allowed: put the saved one back
                std::snprintf(menu.nameEdit, sizeof menu.nameEdit, "%s", menu.playerName.c_str());
            }
        }
```

`IsItemDeactivatedAfterEdit` fires on Enter and on losing focus, so both commit the name.

- [ ] **Step 3: Names on the duel screen**

Thread `menu` through `drawDuel` to `drawDuelBoards(duel, fonts, combo, menu)`. In `drawDuelBoards`, replace the two literal names:

```cpp
    const char* mine = menu.playerName.empty() ? "YOU" : menu.playerName.c_str();
    const char* theirs = duel.opponentName().empty() ? "FOE" : duel.opponentName().c_str();
```

Pass `mine` and `theirs` to the two `drawNameBar` calls.

Add a helper under `drawCountdown` and call it right after `drawCountdown` in the `Countdown` case of `drawDuel`:

```cpp
// "Sam vs Kai" under the countdown digit; nothing until both names are known.
void drawVersus(const Fonts& fonts, const std::string& mine, const std::string& theirs) {
    if (mine.empty() || theirs.empty()) return;
    const std::string text = mine + " vs " + theirs;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float size = vp->Size.y * 0.04f;
    const ImVec2 extent = fonts.hud->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    ImGui::GetForegroundDrawList()->AddText(
        fonts.hud, size, ImVec2(vp->Pos.x + (vp->Size.x - extent.x) * 0.5f, vp->Pos.y + vp->Size.y * 0.62f),
        IM_COL32(255, 255, 255, 230), text.c_str());
}
```

The `drawDuelResult` line `"Opponent left."` (shown after a decided match) becomes `(duel.opponentName().empty() ? "Opponent" : duel.opponentName()) + " left."`, drawn with `TextUnformatted(...c_str())`.

- [ ] **Step 4: Load, default, save and send**

In `app.cpp`'s `App::App()`, after the settings are read:

```cpp
    menu.playerName = saved.playerName;
    if (menu.playerName.empty()) {
        menu.playerName = defaultPlayerName(static_cast<std::uint32_t>(SDL_GetTicksNS()));
        saveSettings();  // keep the generated name across visits
    }
    std::snprintf(menu.nameEdit, sizeof menu.nameEdit, "%s", menu.playerName.c_str());
```

Then:
- `App::saveSettings()` becomes `saveSettingsText(serializeSettings({keymap, menu.muted, menu.musicOff, menu.playerName}));`.
- `App::openDuel()` sends the name first:

```cpp
void App::openDuel() {
    connection = connectToServer();
    if (connection) connection->send(encode(Hello{menu.playerName}));  // queued until the socket opens
    menu.error.clear();
    screen = Screen::Duel;
}
```

- `applyCommand`: `case UiCommand::SaveName: app.saveSettings(); break;`
- `drawFrame`: `command = drawDuel(app.duel, fonts, app.combo, app.menu);`

Typing `m` in the Name field must not mute. `handleKey` already skips the M shortcut while `WantTextInput` is true, so nothing changes there.

- [ ] **Step 5: UI smoke test**

In `tests/ui_test.cpp`, following the existing controls-screen test's setup (context, fonts, `NewFrame`/`Render`), draw the start screen once with `menu.duelsEnabled = true` and `menu.playerName = "Sam"`. Check that it returns `UiCommand::None` and leaves `menu.playerName` unchanged. Copy the exact frame boilerplate from that test.

- [ ] **Step 6: Build everything**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web
```

Both build warning-free and the tests pass.

- [ ] **Step 7: Manual check**

Run `go -C server run .` and use two browser tabs on `http://localhost:8080`:
1. The Name field shows `PlayerNNNN`. A reload keeps the same name.
2. Typing a space, `:`, `=` or `é` does nothing. The 13th character is refused.
3. Clearing the field and clicking away puts the old name back.
4. Typing `m` in the field doesn't mute.
5. Name the tabs `Sam` and `Kai`, create and join. Each bar shows the right name, and the countdown shows "Sam vs Kai" / "Kai vs Sam".
6. Close one tab mid-match. The other shows "Kai left the match".
7. Check that a 12-character name of `W`s fits the name bar at the smallest supported window size (phone landscape). If it doesn't, lower `kMaxNameLength`/`maxNameLength` and update the spec.
8. The desktop build has no Name field and solo play is unchanged.

- [ ] **Step 8: Commit**

```bash
git add src/app/render.hpp src/app/render.cpp src/app/app.hpp src/app/app.cpp tests/ui_test.cpp
git commit -m "feat(app): name field on the start screen, names on the duel screen"
```

---

### Task 6: Docs

**Files:**
- Modify: `README.md`

- [ ] **Step 1: README**

In "Playing a duel", insert a first step:

```
1. Type the name you want your opponent to see in the Name field (it is remembered). Letters,
   digits and `_ - .`, up to 12 characters.
```

Renumber the following steps.

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: player names"
```
