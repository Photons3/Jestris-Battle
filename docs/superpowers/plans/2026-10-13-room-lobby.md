# Room Lobby Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let players find a duel without passing a code around:
- public rooms listed live in a lobby;
- a one-button Quick match;
- `?room=CODE` invite links.

Private code rooms keep working exactly as today.

**Architecture:**
- The protocol gains `CREATE <lvl> PUB`, `LIST`, `UNLIST`, `ROOMS …` and `QUICK <lvl>`.
- The Go hub marks rooms public or private and keeps a set of lobby watchers. Whenever the set of open public rooms changes, it pushes the full list to every watcher. `QUICK` joins the oldest open public room, or opens one.
- `DuelSession` gains a `Lobby` phase (connected, not in a room, holding the list). A refused join from the lobby returns there instead of the start screen.
- The start screen gets Quick match, Browse rooms and a Public checkbox. A new lobby screen lists rooms, and the waiting screen shows an invite link.
- Invite links are read and cleared with small Emscripten helpers next to the WebSocket client.

**Tech Stack:** C++20, CMake ≥ 3.25, SDL3, Dear ImGui 1.92, doctest 2.4.12, Emscripten for the web build, Go 1.27 + `github.com/coder/websocket`.

**Spec:** `docs/superpowers/specs/2026-10-13-room-lobby-design.md`

**Depends on:** `docs/superpowers/plans/2026-10-13-player-names.md`. It must be merged first: `ROOMS` carries host names, and `openDuel()` already sends `HELLO`.

## Global Constraints

- C++20. Every C++ target builds warning-free with `-Wall -Wextra -Wpedantic`. Desktop Debug builds keep ASan/UBSan.
- `CREATE <lvl>` without a second argument is still a private room. A private `CreateRoom` encodes as plain `CREATE <lvl>`, so the wire and the existing tests are unchanged. `PRIV` is accepted on decode.
- `ROOMS` lists only open public rooms (one player, public), oldest first, at most 20 (`kMaxListedRooms` in C++, `maxListedRooms` in Go, each citing the other). Entries are `CODE:lvl:host`.
- `LIST` subscribes. `UNLIST`, `CREATE`, `JOIN`, `QUICK` and disconnecting unsubscribe. Each change to the open public set sends exactly one `ROOMS` to each watcher and nothing to anyone else.
- Inside a room, `LIST`/`UNLIST` are dropped and never relayed, and `QUICK` gets `ERR already in a room`. `ROOMS` is server-only.
- `maxRooms = 500`. Opening a room beyond that gets `ERR server is full`.
- Quick match ignores the caller's level when it joins: the room's level wins and arrives in `START`.
- Invite links prefill and highlight a Join button. They never join automatically.
- Conventional commits scoped like the existing history. Work on `develop`, or on a feature branch off it (`feat/room-lobby`).

## Commands

- Desktop build and all C++ tests: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- One doctest group: `./build/tetris_tests -tc="duel:*"`
- Go tests: `go -C server test -race ./...` and `go -C server vet ./...`
- Web build: `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web`

## File map

| File | Change |
|---|---|
| `src/net/protocol.hpp/.cpp` | `CreateRoom::isPublic`, `ListRooms`, `UnlistRooms`, `QuickMatch`, `RoomInfo`, `RoomList`, `kMaxListedRooms`, public `isRoomCode` |
| `server/hub.go` | room visibility, host, seq; watchers; `LIST`/`UNLIST`/`ROOMS`; `QUICK`; `maxRooms` |
| `src/app/duel.hpp/.cpp` | `DuelPhase::Lobby`, `openLobby`, `quickMatch`, `createRoom(lvl, public)`, `rooms()`, back-to-lobby on `ERR` |
| `src/app/settings.hpp/.cpp` | `Settings::publicRooms` (`public=on/off`) |
| `src/net/connection.hpp`, `ws_client_web.cpp`, `connection_none.cpp` | invite-link helpers |
| `src/app/render.hpp/.cpp` | start-screen rows, lobby screen, waiting-screen text and invite link |
| `src/app/app.hpp/.cpp` | new commands, idempotent `openDuel`, invite code at startup |
| `tests/protocol_test.cpp`, `tests/duel_test.cpp`, `tests/settings_test.cpp`, `tests/sound_cues_test.cpp`, `server/hub_test.go` | tests per task |
| `README.md` | lobby, Quick match, invite links |

---

### Task 1: Protocol messages

**Files:**
- Modify: `src/net/protocol.hpp`, `src/net/protocol.cpp`
- Test: `tests/protocol_test.cpp`

**Interfaces:**
- Produces:
  - `CreateRoom{int startLevel; bool isPublic = false;}`
  - `ListRooms{}`, `UnlistRooms{}`
  - `QuickMatch{int startLevel;}`
  - `RoomInfo{std::string code; int startLevel; std::string host;}`
  - `RoomList{std::vector<RoomInfo> rooms;}`
  - `inline constexpr int kMaxListedRooms = 20;`
  - `bool isRoomCode(std::string_view)`, moved out of the anonymous namespace.

- [ ] **Step 1: Write the failing tests**

Add these to the round-trip list: `CreateRoom{3, true}`, `ListRooms{}`, `UnlistRooms{}`, `QuickMatch{9}`, `RoomList{}`, and `RoomList{{{"ABCD", 3, "Sam"}, {"WXYZ", 15, "Player0001"}}}`. Then append:

```cpp
TEST_CASE("protocol: lobby messages on the wire") {
    CHECK(encode(CreateRoom{3}) == "CREATE 3");  // private stays the old format
    CHECK(encode(CreateRoom{3, true}) == "CREATE 3 PUB");
    CHECK(decode("CREATE 3 PRIV") == Message{CreateRoom{3, false}});
    CHECK(encode(ListRooms{}) == "LIST");
    CHECK(encode(UnlistRooms{}) == "UNLIST");
    CHECK(encode(QuickMatch{4}) == "QUICK 4");
    CHECK(encode(RoomList{}) == "ROOMS");
    CHECK(encode(RoomList{{{"ABCD", 3, "Sam"}, {"WXYZ", 12, "Kai"}}}) == "ROOMS ABCD:3:Sam WXYZ:12:Kai");
}

TEST_CASE("protocol: malformed lobby messages are rejected") {
    for (std::string_view text : {"CREATE 3 OPEN", "CREATE 3 pub", "CREATE 3 PUB x", "LIST x", "UNLIST x", "QUICK",
                                  "QUICK 0", "QUICK 16", "QUICK x", "ROOMS ", "ROOMS ABCD", "ROOMS ABCD:3",
                                  "ROOMS ABCD:3:", "ROOMS abcd:3:Sam", "ROOMS ABCD:0:Sam", "ROOMS ABCD:3:Sam:x",
                                  "ROOMS ABCD:3:a b", "ROOMS ABCD:x:Sam", "ROOMS ABCD:3:Sam "}) {
        CAPTURE(text);
        CHECK_FALSE(decode(text).has_value());
    }
    std::string tooMany = "ROOMS";
    for (int i = 0; i <= kMaxListedRooms; ++i) tooMany += " ABCD:1:Sam";
    CHECK_FALSE(decode(tooMany).has_value());
}
```

- [ ] **Step 2: Run them and watch them fail**

`./build/tetris_tests -tc="protocol:*"` fails to compile.

- [ ] **Step 3: Implement**

In `protocol.hpp`:
- Add `#include <vector>`.
- Add `inline constexpr int kMaxListedRooms = 20;  // maxListedRooms in server/hub.go`.
- Declare `bool isRoomCode(std::string_view text);`.
- Give `CreateRoom` a `bool isPublic = false;` member.
- Add the new structs, each with a defaulted `operator==` like the rest.
- Append `ListRooms, UnlistRooms, QuickMatch, RoomList` to `Message`.

In `protocol.cpp`, move `isRoomCode` out of the anonymous namespace. Inside the anonymous namespace add:

```cpp
// "CODE:lvl:host"; nullopt unless all three parts are valid.
std::optional<RoomInfo> decodeRoomInfo(std::string_view entry) {
    const std::size_t first = entry.find(':');
    if (first == std::string_view::npos) return std::nullopt;
    const std::size_t second = entry.find(':', first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    const std::string_view code = entry.substr(0, first);
    const auto level = parseNumber(entry.substr(first + 1, second - first - 1), kMinStartLevel, kMaxStartLevel);
    const std::string_view host = entry.substr(second + 1);
    if (!isRoomCode(code) || !level || !isValidName(host)) return std::nullopt;  // a third ':' fails isValidName
    return RoomInfo{std::string(code), *level, std::string(host)};
}
```

Changes to `encode`:
- `CreateRoom` becomes `return "CREATE " + std::to_string(m.startLevel) + (m.isPublic ? " PUB" : "");`.
- New branches:
  - `ListRooms` returns `"LIST"`.
  - `UnlistRooms` returns `"UNLIST"`.
  - `QuickMatch` returns `"QUICK " + std::to_string(m.startLevel)`.
  - `RoomList` builds `"ROOMS"` and appends `" " + code + ":" + std::to_string(level) + ":" + host` for each room.

In `decode`, replace the `CREATE` branch and add new ones before `else if (args == 0)`:

```cpp
    if (verb == "CREATE" && (args == 1 || args == 2)) {
        const auto level = parseNumber(parts[1], kMinStartLevel, kMaxStartLevel);
        if (!level) return std::nullopt;
        if (args == 1 || parts[2] == "PRIV") return CreateRoom{*level, false};
        if (parts[2] == "PUB") return CreateRoom{*level, true};
    } else if (verb == "QUICK" && args == 1) {
        if (const auto level = parseNumber(parts[1], kMinStartLevel, kMaxStartLevel)) return QuickMatch{*level};
    } else if (verb == "ROOMS" && args <= static_cast<std::size_t>(kMaxListedRooms)) {
        RoomList list;
        for (std::size_t i = 1; i < parts.size(); ++i) {
            const auto room = decodeRoomInfo(parts[i]);
            if (!room) return std::nullopt;
            list.rooms.push_back(*room);
        }
        return list;
```

Inside the `args == 0` block, add `if (verb == "LIST") return ListRooms{};` and `if (verb == "UNLIST") return UnlistRooms{};`.

`"ROOMS "` splits into `["ROOMS", ""]`, and the empty entry fails, as intended.

- [ ] **Step 4: Run the tests**

`ctest --test-dir build --output-on-failure` passes. The existing duel test that expects `"CREATE 3"` is unchanged.

- [ ] **Step 5: Commit**

```bash
git add src/net/protocol.hpp src/net/protocol.cpp tests/protocol_test.cpp
git commit -m "feat(net): lobby messages CREATE PUB, LIST, UNLIST, ROOMS and QUICK"
```

---

### Task 2: Server public rooms and the live list

**Files:**
- Modify: `server/hub.go`
- Test: `server/hub_test.go`

**Interfaces:**
- `room` gains `public bool`, `host string` and `seq uint64`.
- `Hub` gains:
  - `watchers map[*player]bool`
  - `nextSeq uint64`
  - `maxRooms int` (default 500; tests lower it)
- Consumes `CREATE <lvl> [PUB|PRIV]`, `LIST` and `UNLIST`. Produces `ROOMS …` and `ERR bad room options` / `ERR server is full`.

- [ ] **Step 1: A configurable test hub**

In `hub_test.go`, split `newTestHub` so a test can tweak the hub before the server starts:

```go
func newConfiguredHub(t *testing.T, configure func(*Hub)) (*httptest.Server, *fakeClock) {
	t.Helper()
	hub := NewHub()
	// ... the existing body of newTestHub, then:
	configure(hub)
	srv := httptest.NewServer(hub)
	t.Cleanup(srv.Close)
	return srv, clock
}

func newTestHub(t *testing.T) (*httptest.Server, *fakeClock) {
	return newConfiguredHub(t, func(*Hub) {})
}
```

- [ ] **Step 2: Write the failing tests**

```go
// watch dials a lobby watcher and consumes its first (empty) list.
func watch(t *testing.T, srv *httptest.Server) *client {
	t.Helper()
	w := dial(t, srv)
	w.send("LIST")
	w.expect("ROOMS")
	return w
}

func TestPublicRoomsAreListedUntilFull(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	a, b := dial(t, srv), dial(t, srv)
	a.send("HELLO Sam")
	a.send("CREATE 3 PUB")
	a.expect("ROOM AAAA")
	w.expect("ROOMS AAAA:3:Sam")
	b.send("JOIN AAAA")
	w.expect("ROOMS")
}

func TestPrivateRoomsAreNotListed(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	a, b := dial(t, srv), dial(t, srv)
	a.send("CREATE 3")
	a.expect("ROOM AAAA")
	b.send("CREATE 5 PUB")
	b.expect("ROOM AAAB")
	w.expect("ROOMS AAAB:5:Player") // the private room sent nothing before this
}

func TestHostLeavingRemovesTheRoom(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	a := dial(t, srv)
	a.send("CREATE 2 PUB")
	a.expect("ROOM AAAA")
	w.expect("ROOMS AAAA:2:Player")
	a.conn.Close(websocket.StatusNormalClosure, "")
	w.expect("ROOMS")
}

func TestCreatingUnsubscribes(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	w.send("CREATE 1 PUB")
	w.expect("ROOM AAAA") // no ROOMS: w stopped watching
	x := dial(t, srv)
	x.send("CREATE 2 PUB")
	x.expect("ROOM AAAB")
	y := dial(t, srv)
	y.send("JOIN AAAA")
	w.expect("OPP Player") // still nothing in between
}

func TestUnlistStopsUpdates(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	w.send("UNLIST")
	a, b := dial(t, srv), dial(t, srv)
	a.send("CREATE 1 PUB")
	a.expect("ROOM AAAA")
	b.send("CREATE 1 PUB")
	b.expect("ROOM AAAB")
	w.send("LIST")
	w.expect("ROOMS AAAA:1:Player AAAB:1:Player") // a push would have arrived first with one room
}

func TestListIsOldestFirstAndCapped(t *testing.T) {
	srv := newTestServer(t)
	for i := 0; i < maxListedRooms+1; i++ {
		c := dial(t, srv)
		c.send("CREATE 1 PUB")
		c.expect(fmt.Sprintf("ROOM AAA%c", 'A'+i))
	}
	w := dial(t, srv)
	w.send("LIST")
	var want strings.Builder
	want.WriteString("ROOMS")
	for i := 0; i < maxListedRooms; i++ {
		fmt.Fprintf(&want, " AAA%c:1:Player", 'A'+i)
	}
	w.expect(want.String())
}

func TestBadRoomOptionsAndFullServer(t *testing.T) {
	srv, _ := newConfiguredHub(t, func(h *Hub) { h.maxRooms = 1 })
	a, b := dial(t, srv), dial(t, srv)
	a.send("CREATE 3 OPEN")
	a.expect("ERR bad room options")
	a.send("CREATE 3")
	a.expect("ROOM AAAA")
	b.send("CREATE 3 PUB")
	b.expect("ERR server is full")
}

func TestLobbyVerbsInsideARoomAreNotRelayed(t *testing.T) {
	a, b := pair(t, newTestServer(t))
	a.send("LIST")
	a.send("UNLIST")
	a.send("ROOMS AAAA:1:Sam")
	a.send("ATK 1")
	b.expect("ATK 1")
}
```

- [ ] **Step 3: Run them and watch them fail**

`go -C server test -race ./...` fails or times out.

- [ ] **Step 4: Implement**

Add the constants `maxListedRooms = 20 // kMaxListedRooms in src/net/protocol.hpp` and `defaultMaxRooms = 500`. In `NewHub`, set `watchers: map[*player]bool{}` and `maxRooms: defaultMaxRooms`. Add `"ROOMS"` to the server-only verbs and to the `Hub` doc comment, along with LIST/UNLIST/QUICK as verbs the hub handles.

In `route`, in the `p.room == nil` switch:

```go
		case "LIST":
			if arg != "" {
				return nil
			}
			h.watchers[p] = true
			return []outgoing{{p, h.roomsMessage()}}
		case "UNLIST":
			delete(h.watchers, p)
			return nil
```

In the in-room switch, add `case "LIST", "UNLIST": return nil`.

Replace `create` with a parser plus an opener:

```go
func (h *Hub) create(p *player, arg string) []outgoing {
	levelText, vis, hasVis := strings.Cut(arg, " ")
	level, err := strconv.Atoi(levelText)
	if err != nil || level < minStartLevel || level > maxStartLevel {
		return []outgoing{{p, "ERR bad start level"}}
	}
	public := false
	if hasVis {
		switch vis {
		case "PUB":
			public = true
		case "PRIV":
		default:
			return []outgoing{{p, "ERR bad room options"}}
		}
	}
	return h.open(p, level, public)
}

// open puts p alone in a new room and, if it is public, tells the lobby.
func (h *Hub) open(p *player, level int, public bool) []outgoing {
	if len(h.rooms) >= h.maxRooms {
		return []outgoing{{p, "ERR server is full"}}
	}
	delete(h.watchers, p)
	code := h.newCode()
	for h.rooms[code] != nil {
		code = h.newCode()
	}
	h.nextSeq++
	r := &room{code: code, level: level, public: public, host: p.name, seq: h.nextSeq}
	r.players[0] = p
	p.room = r
	h.rooms[code] = r
	out := []outgoing{{p, "ROOM " + code}}
	if public {
		out = append(out, h.lobbyUpdate()...)
	}
	return out
}
```

In `join`, call `delete(h.watchers, p)` first. After seating the player, append `h.lobbyUpdate()...` when `r.public`. Do this for an error reply too: a refused join also unsubscribes, and the client re-sends `LIST` (Task 4).

In `leave`, call `delete(h.watchers, p)`. Inside the room branch, compute `wasListed := r.public && r.players[1] == nil` before deleting the room, and append `h.lobbyUpdate()...` to `out` when it is true.

Add:

```go
// openPublic lists the rooms a lobby can join, oldest first.
func (h *Hub) openPublic() []*room {
	var open []*room
	for _, r := range h.rooms {
		if r.public && r.players[1] == nil {
			open = append(open, r)
		}
	}
	slices.SortFunc(open, func(a, b *room) int { return cmp.Compare(a.seq, b.seq) })
	return open
}

func (h *Hub) roomsMessage() string {
	var b strings.Builder
	b.WriteString("ROOMS")
	for i, r := range h.openPublic() {
		if i == maxListedRooms {
			break
		}
		fmt.Fprintf(&b, " %s:%d:%s", r.code, r.level, r.host)
	}
	return b.String()
}

// lobbyUpdate sends the current list to every watcher.
func (h *Hub) lobbyUpdate() []outgoing {
	if len(h.watchers) == 0 {
		return nil
	}
	msg := h.roomsMessage()
	out := make([]outgoing, 0, len(h.watchers))
	for w := range h.watchers {
		out = append(out, outgoing{w, msg})
	}
	return out
}
```

Import `cmp` and `slices`. `route` splits `LIST x` into verb `LIST` and arg `x`, so the `arg != ""` check rejects it.

- [ ] **Step 5: Run the Go tests**

`go -C server test -race ./... && go -C server vet ./...` passes.

- [ ] **Step 6: Commit**

```bash
git add server/hub.go server/hub_test.go
git commit -m "feat(server): public rooms and a live lobby list"
```

---

### Task 3: Server Quick match

**Files:**
- Modify: `server/hub.go`
- Test: `server/hub_test.go`

**Interfaces:**
- Consumes `QUICK <lvl>`. Replies `ROOM <code>` (it opened a public room) or `OPP` + `START` (it joined one).

- [ ] **Step 1: Write the failing tests**

```go
func TestQuickOpensAPublicRoomWhenNoneIsOpen(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	a := dial(t, srv)
	a.send("QUICK 4")
	a.expect("ROOM AAAA")
	w.expect("ROOMS AAAA:4:Player")
}

func TestQuickJoinsTheOldestOpenRoomAtItsLevel(t *testing.T) {
	srv := newTestServer(t)
	a, b, c := dial(t, srv), dial(t, srv), dial(t, srv)
	a.send("CREATE 3 PUB")
	a.expect("ROOM AAAA")
	b.send("CREATE 5 PUB")
	b.expect("ROOM AAAB")
	c.send("QUICK 9")
	c.expect("OPP Player")
	c.expect("START 101 3")
	a.expect("OPP Player")
	a.expect("START 101 3")
}

func TestQuickIgnoresPrivateRooms(t *testing.T) {
	srv := newTestServer(t)
	a, b := dial(t, srv), dial(t, srv)
	a.send("CREATE 3")
	a.expect("ROOM AAAA")
	b.send("QUICK 2")
	b.expect("ROOM AAAB")
}

func TestTwoQuicksPairUp(t *testing.T) {
	srv := newTestServer(t)
	a, b := dial(t, srv), dial(t, srv)
	a.send("QUICK 2")
	a.expect("ROOM AAAA")
	b.send("QUICK 7")
	b.expect("OPP Player")
	b.expect("START 101 2")
}

func TestQuickErrors(t *testing.T) {
	srv, _ := newConfiguredHub(t, func(h *Hub) { h.maxRooms = 1 })
	a, _ := pair(t, srv)
	a.send("QUICK 1")
	a.expect("ERR already in a room")
	c := dial(t, srv)
	c.send("QUICK 0")
	c.expect("ERR bad start level")
	c.send("QUICK 1")
	c.expect("ERR server is full")
}
```

- [ ] **Step 2: Run them and watch them fail**

- [ ] **Step 3: Implement**

In `route`'s `p.room == nil` switch, add `case "QUICK": return h.quick(p, arg)`. In the in-room switch, extend `case "CREATE", "JOIN":` to `case "CREATE", "JOIN", "QUICK":`.

```go
// quick joins the oldest open public room, at that room's level, or opens a public room at level arg.
func (h *Hub) quick(p *player, arg string) []outgoing {
	level, err := strconv.Atoi(arg)
	if err != nil || level < minStartLevel || level > maxStartLevel {
		return []outgoing{{p, "ERR bad start level"}}
	}
	if open := h.openPublic(); len(open) > 0 {
		return h.join(p, open[0].code)
	}
	return h.open(p, level, true)
}
```

`join` and `open` already drop `p` from `watchers` and update the lobby.

- [ ] **Step 4: Run the Go tests, then commit**

```bash
go -C server test -race ./... && go -C server vet ./...
git add server/hub.go server/hub_test.go
git commit -m "feat(server): quick match"
```

---

### Task 4: The Lobby phase in `DuelSession`

**Files:**
- Modify: `src/app/duel.hpp`, `src/app/duel.cpp`
- Test: `tests/duel_test.cpp`, `tests/sound_cues_test.cpp`

**Interfaces:**
- `enum class DuelPhase { Idle, Lobby, Waiting, Countdown, Playing, Result };`
- `void openLobby();`, `void createRoom(int startLevel, bool isPublic = false);`, `void quickMatch(int startLevel);`
- `const std::vector<RoomInfo>& rooms() const;`, `bool publicRoom() const;`, `bool quickMatching() const;`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("duel: the lobby sends LIST and keeps the pushed list") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.openLobby();
    CHECK(duel.phase() == DuelPhase::Lobby);
    CHECK(wire.sent == std::vector<std::string>{"LIST"});
    duel.onMessage("ROOMS ABCD:3:Sam");
    REQUIRE(duel.rooms().size() == 1);
    CHECK(duel.rooms()[0] == RoomInfo{"ABCD", 3, "Sam"});
    duel.onMessage("ROOMS");
    CHECK(duel.rooms().empty());
}

TEST_CASE("duel: ROOMS outside the lobby is ignored") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.createRoom(1);
    duel.onMessage("ROOMS ABCD:3:Sam");
    CHECK(duel.rooms().empty());
}

TEST_CASE("duel: a refused join from the lobby goes back to the lobby") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.openLobby();
    duel.onMessage("ROOMS ABCD:3:Sam");
    duel.joinRoom("ABCD");
    CHECK(duel.phase() == DuelPhase::Waiting);
    wire.sent.clear();
    duel.onMessage("ERR room is full");
    CHECK(duel.phase() == DuelPhase::Lobby);
    CHECK(duel.error() == "room is full");
    CHECK(wire.sent == std::vector<std::string>{"LIST"});
}

TEST_CASE("duel: a refused join from the start screen still goes to idle") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.joinRoom("ABCD");
    duel.onMessage("ERR unknown room");
    CHECK(duel.phase() == DuelPhase::Idle);
}

TEST_CASE("duel: a public room from the lobby") {
    Wire wire;
    DuelSession duel(wire.sender());
    duel.openLobby();
    wire.sent.clear();
    duel.createRoom(4, true);
    CHECK(wire.sent == std::vector<std::string>{"CREATE 4 PUB"});
    CHECK(duel.publicRoom());
    CHECK_FALSE(duel.quickMatching());
}

TEST_CASE("duel: quick match waits as a host or starts as a joiner") {
    Wire wire;
    DuelSession host(wire.sender());
    host.quickMatch(2);
    CHECK(wire.sent == std::vector<std::string>{"QUICK 2"});
    CHECK(host.quickMatching());
    host.onMessage("ROOM ABCD");
    CHECK(host.roomCode() == "ABCD");
    CHECK(host.phase() == DuelPhase::Waiting);

    DuelSession joiner(wire.sender());
    joiner.quickMatch(9);
    joiner.onMessage("OPP Sam");
    joiner.onMessage("START 7 2");
    CHECK(joiner.phase() == DuelPhase::Countdown);
    CHECK(joiner.opponentName() == "Sam");
}
```

In `tests/sound_cues_test.cpp`, add `DuelPhase::Lobby` to the list of phases that give the stopped cue.

- [ ] **Step 2: Run them and watch them fail**

- [ ] **Step 3: Implement**

In `duel.hpp`:
- Insert `Lobby` after `Idle` in `DuelPhase`.
- Add the declarations from Interfaces. Give `createRoom` the defaulted `isPublic`.
- Add the members `std::vector<RoomInfo> rooms_;`, `bool fromLobby_ = false;`, `bool publicRoom_ = false;` and `bool quick_ = false;`.

In `duel.cpp`:

```cpp
void DuelSession::openLobby() {
    reset();
    phase_ = DuelPhase::Lobby;
    send(ListRooms{});
}

// Resets for a new room request, remembering whether it came from the lobby.
void DuelSession::beginRequest(bool isPublic, bool quick) {
    const bool fromLobby = phase_ == DuelPhase::Lobby;
    reset();
    phase_ = DuelPhase::Waiting;
    fromLobby_ = fromLobby;
    publicRoom_ = isPublic;
    quick_ = quick;
}

void DuelSession::createRoom(int startLevel, bool isPublic) {
    beginRequest(isPublic, false);
    send(CreateRoom{clampStartLevel(startLevel), isPublic});
}

void DuelSession::joinRoom(const std::string& code) {
    beginRequest(false, false);
    send(JoinRoom{code});
}

void DuelSession::quickMatch(int startLevel) {
    beginRequest(true, true);
    send(QuickMatch{clampStartLevel(startLevel)});
}
```

Declare `beginRequest` as a private member.

In `onMessage`, replace the `ServerError` branch and add a `RoomList` branch:

```cpp
    } else if (const auto* error = std::get_if<ServerError>(&*message)) {
        if (phase_ == DuelPhase::Waiting) {
            const bool backToLobby = fromLobby_;
            reset();
            error_ = error->reason;
            if (backToLobby) {  // the server stopped sending the list when we asked for a room
                phase_ = DuelPhase::Lobby;
                send(ListRooms{});
            }
        }
    } else if (const auto* list = std::get_if<RoomList>(&*message)) {
        if (phase_ == DuelPhase::Lobby) rooms_ = list->rooms;
```

`reset()` also clears `rooms_`, `fromLobby_`, `publicRoom_` and `quick_`.

`duelMusic` already returns the stopped cue for every phase but Playing, so it needs no change.

- [ ] **Step 4: Fix the remaining switches**

`drawDuel` in `render.cpp` switches over `DuelPhase` and now warns about `Lobby` under `-Wall`. For now add `case DuelPhase::Lobby: break;`. Task 6 fills it in.

- [ ] **Step 5: Run all C++ tests, then commit**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
git add src/app/duel.hpp src/app/duel.cpp src/app/render.cpp tests/duel_test.cpp tests/sound_cues_test.cpp
git commit -m "feat(app): lobby phase, public rooms and quick match in the duel session"
```

---

### Task 5: Remember the Public choice

**Files:**
- Modify: `src/app/settings.hpp`, `src/app/settings.cpp`
- Test: `tests/settings_test.cpp`

- [ ] **Step 1: Failing test**

```cpp
TEST_CASE("settings: the public-room choice round-trips and defaults to off") {
    Settings settings;
    CHECK_FALSE(parseSettings(serializeSettings(settings)).publicRooms);
    settings.publicRooms = true;
    CHECK(parseSettings(serializeSettings(settings)).publicRooms);
}
```

- [ ] **Step 2: Implement**

- Add `bool publicRooms = false;  // Create room makes a public room` to `Settings`, after `playerName`.
- In `serializeSettings`: `out += settings.publicRooms ? "public=on\n" : "public=off\n";`.
- In `parseSettings`: `} else if (name == "public") { settings.publicRooms = value == "on";`.

- [ ] **Step 3: Run tests, commit**

```bash
ctest --test-dir build --output-on-failure
git add src/app/settings.hpp src/app/settings.cpp tests/settings_test.cpp
git commit -m "feat(app): remember whether new rooms are public"
```

---

### Task 6: Start screen, lobby screen and waiting screen

**Files:**
- Modify: `src/app/render.hpp`, `src/app/render.cpp`
- Modify: `src/app/app.hpp`, `src/app/app.cpp`
- Test: `tests/ui_test.cpp` (draw smoke test), manual checklist

**Interfaces:**
- `UiCommand` gains `BrowseRooms`, `QuickMatch`, `JoinListedRoom`, `CreatePublicRoom` and `SavePublic`.
- `StartMenu` gains `bool publicRooms` and `std::string pickedRoom` (the code clicked in the lobby).
- `drawDuel(const DuelSession&, const Fonts&, const ComboLabel&, StartMenu&)`. The menu is no longer const, because the lobby writes `pickedRoom`.
- `App::openDuel()` connects and sends `HELLO` only when there is no connection yet.

- [ ] **Step 1: Start screen**

In `drawStartScreen`, the duel section after the Name field becomes:

```cpp
        if (menuButton("Quick match", width)) command = UiCommand::QuickMatch;
        if (menuButton("Browse rooms", width)) command = UiCommand::BrowseRooms;
        // Create room and the Public box share one row.
        const float createWidth = width * 0.62f;
        if (ImGui::Button("Create room", ImVec2(createWidth, 0.0f))) command = UiCommand::CreateRoom;
        ImGui::SameLine();
        if (ImGui::Checkbox("Public", &menu.publicRooms)) command = UiCommand::SavePublic;
```

The code box, Join room and the error line stay as they are. If `menuButton` applies styling that a plain `ImGui::Button` lacks, use the same style push for Create room. Check `menuButton` and match it.

- [ ] **Step 2: Lobby screen**

In the anonymous namespace of `render.cpp`:

```cpp
constexpr int kLobbyVisibleRows = 8;  // the list scrolls past this many

UiCommand drawLobby(const DuelSession& duel, const Fonts& fonts, StartMenu& menu) {
    UiCommand command = UiCommand::None;
    beginMenu("##lobby");
    const float width = std::max(menuTitle(fonts, "OPEN ROOMS"), menuButtonWidth());
    if (!duel.error().empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", duel.error().c_str());
    if (duel.rooms().empty()) {
        ImGui::TextUnformatted("No open rooms yet.");
    } else {
        const float height = ImGui::GetFrameHeightWithSpacing() * kLobbyVisibleRows;
        if (ImGui::BeginTable("##rooms", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, ImVec2(width, height))) {
            ImGui::TableSetupColumn("Host", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("##join", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const RoomInfo& room : duel.rooms()) {
                ImGui::PushID(room.code.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(room.host.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", room.startLevel);
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Join")) {
                    menu.pickedRoom = room.code;
                    command = UiCommand::JoinListedRoom;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::Spacing();
    if (menuButton("Quick match", width)) command = UiCommand::QuickMatch;
    if (menuButton("Create public room", width)) command = UiCommand::CreatePublicRoom;
    if (menuButton("Back", width)) command = UiCommand::LeaveDuel;
    ImGui::End();
    return command;
}
```

In `drawDuel`, replace the placeholder with `case DuelPhase::Lobby: command = drawLobby(duel, fonts, menu); break;`.

- [ ] **Step 3: Waiting screen**

In the `Waiting` case, once the code is known, replace the single "Waiting for an opponent to join..." line with:

```cpp
            ImGui::TextUnformatted(duel.quickMatching() ? "Looking for an opponent..."
                                                        : "Waiting for an opponent to join...");
            ImGui::TextUnformatted(duel.publicRoom() ? "Public room, listed in Browse rooms"
                                                     : "Private room, share the code");
```

While the code is still empty, a quick match shows `SEARCHING` instead of `CONNECTING`. It may join straight into a countdown without ever getting a code.

- [ ] **Step 4: App commands**

In `app.cpp`, `openDuel()` becomes idempotent:

```cpp
void App::openDuel() {
    if (!connection) {
        connection = connectToServer();
        if (connection) connection->send(encode(Hello{menu.playerName}));
    }
    menu.error.clear();
    screen = Screen::Duel;
}
```

Update its comment in `app.hpp` to "connects (once) and shows the duel screens".

In `applyCommand`:

```cpp
    case UiCommand::CreateRoom:
        app.openDuel();
        app.duel.createRoom(app.menu.startLevel, app.menu.publicRooms);
        break;
    case UiCommand::CreatePublicRoom:
        app.openDuel();
        app.duel.createRoom(app.menu.startLevel, true);
        break;
    case UiCommand::BrowseRooms:
        app.openDuel();
        app.duel.openLobby();
        break;
    case UiCommand::QuickMatch:
        app.openDuel();
        app.duel.quickMatch(app.menu.startLevel);
        break;
    case UiCommand::JoinListedRoom:
        app.duel.joinRoom(app.menu.pickedRoom);
        break;
    case UiCommand::SavePublic:
        app.saveSettings();
        break;
```

Other changes in `app.cpp`:
- The constructor reads `menu.publicRooms = saved.publicRooms;`.
- `saveSettings()` passes `menu.publicRooms` as the last field.
- `drawFrame` passes `app.menu` to `drawDuel`.

`pumpNetwork` needs no change: a session that falls back to Idle (connection lost) still returns to the start screen with the error, and Lobby is not Idle.

- [ ] **Step 5: UI smoke test**

In `tests/ui_test.cpp`, using the controls-screen test's frame boilerplate, build a `DuelSession` with a no-op sender. Call `openLobby()` and feed it `ROOMS ABCD:3:Sam WXYZ:5:Kai`. Draw `drawDuel` for one frame and check it returns `UiCommand::None`.

- [ ] **Step 6: Build, test, commit**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web
git add src/app/render.hpp src/app/render.cpp src/app/app.hpp src/app/app.cpp tests/ui_test.cpp
git commit -m "feat(app): quick match, room browser and public rooms on the start screen"
```

---

### Task 7: Invite links

**Files:**
- Modify: `src/net/connection.hpp`, `src/net/ws_client_web.cpp`, `src/net/connection_none.cpp`
- Modify: `src/app/render.hpp`, `src/app/render.cpp`, `src/app/app.cpp`

**Interfaces:**
- `std::string takeInviteCode();`: the raw `?room=` value. It is removed from the address bar and is empty on desktop.
- `std::string inviteLinkBase();`: e.g. `http://192.168.1.5:8080/?room=`. Empty on desktop.
- `bool canCopyToClipboard();`
- `void copyInviteLink(const std::string& code);`: does nothing unless `isRoomCode(code)`.
- `UiCommand::JoinInvite`, `UiCommand::CopyInviteLink`.
- `StartMenu` gains `std::string inviteCode`, `std::string inviteLinkBase` and `bool canCopyLink`.

- [ ] **Step 1: Platform helpers**

In `connection.hpp`, declare the four functions with the comments above. The desktop stubs in `connection_none.cpp` return `{}`, `{}` and `false`, and do nothing for the copy. In `ws_client_web.cpp`, include `net/protocol.hpp` and add:

```cpp
std::string takeInviteCode() {
    return emscripten_run_script_string(
        "(function () {"
        "  var url = new URL(location.href), code = url.searchParams.get('room') || '';"
        "  if (code) {"
        "    url.searchParams.delete('room');"  // a reload must not rejoin a room that is gone
        "    history.replaceState(null, '', url.pathname + url.search + url.hash);"
        "  }"
        "  return code.slice(0, 8);"
        "})()");
}

std::string inviteLinkBase() { return emscripten_run_script_string("location.origin + location.pathname + '?room='"); }

// The Clipboard API exists only in secure contexts: https://, or localhost, never a plain-http LAN address.
bool canCopyToClipboard() {
    return emscripten_run_script_int("window.isSecureContext && navigator.clipboard ? 1 : 0") != 0;
}

void copyInviteLink(const std::string& code) {
    if (!isRoomCode(code)) return;  // only A-Z ever reaches the script
    const std::string script =
        "navigator.clipboard.writeText(location.origin + location.pathname + '?room=" + code + "')";
    emscripten_run_script(script.c_str());
}
```

- [ ] **Step 2: Startup**

In `App::App()`:

```cpp
    std::string invite = takeInviteCode();
    std::transform(invite.begin(), invite.end(), invite.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (isRoomCode(invite)) menu.inviteCode = invite;
    menu.inviteLinkBase = inviteLinkBase();
    menu.canCopyLink = canCopyToClipboard();
```

Include `<algorithm>` and `<cctype>`.

- [ ] **Step 3: Start-screen invite button**

At the top of the duel section in `drawStartScreen`, before Quick match:

```cpp
        if (!menu.inviteCode.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.25f, 1.0f));
            const std::string label = "Join room " + menu.inviteCode;
            if (menuButton(label.c_str(), width)) command = UiCommand::JoinInvite;
            ImGui::PopStyleColor();
        }
```

In `applyCommand`:

```cpp
    case UiCommand::JoinInvite:
        std::snprintf(app.menu.joinCode, sizeof app.menu.joinCode, "%s", app.menu.inviteCode.c_str());
        app.menu.inviteCode.clear();
        app.openDuel();
        app.duel.joinRoom(app.menu.joinCode);
        break;
    case UiCommand::CopyInviteLink:
        copyInviteLink(app.duel.roomCode());
        break;
```

- [ ] **Step 4: Waiting-screen link**

In the `Waiting` case, after the room-type line, show the link when it is known:

```cpp
            if (!menu.inviteLinkBase.empty()) {
                static char link[256];  // InputText needs a mutable buffer even when read-only
                std::snprintf(link, sizeof link, "%s%s", menu.inviteLinkBase.c_str(), duel.roomCode().c_str());
                ImGui::SetNextItemWidth(menuButtonWidth());
                ImGui::InputText("##link", link, sizeof link, ImGuiInputTextFlags_ReadOnly);
                if (menu.canCopyLink && menuButton("Copy link")) command = UiCommand::CopyInviteLink;
            }
```

- [ ] **Step 5: Build and commit**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web
git add src/net/connection.hpp src/net/ws_client_web.cpp src/net/connection_none.cpp src/app/render.hpp src/app/render.cpp src/app/app.cpp
git commit -m "feat(app): invite links"
```

---

### Task 8: Manual check and docs

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Manual checklist**

Run `go -C server run .` and use three tabs on `http://localhost:8080`, named A, B and C.

Lobby and joining:
1. A: Browse rooms shows "No open rooms yet."
2. B: tick Public, then Create room. A's list shows B's row at once, with no refresh. The waiting screen says "Public room, listed in Browse rooms". Reload B: Public is still ticked.
3. A: Join on B's row. The match starts, and the row disappears from C's lobby if C is browsing.
4. C: create a private room. It never appears in any lobby.

Errors:
5. Two lobby tabs click Join on the same row at the same moment. The loser returns to the lobby with "room is full", and the list is already updated.
6. B: Create public room, then close the tab. The row disappears from A's lobby.

Quick match:
7. A: Quick match with no open rooms gives `SEARCHING`, then a code and "Looking for an opponent...". B: Quick match joins A at A's level.

Leaving and connection:
8. Back from the lobby returns to the start screen. Stopping the server while in the lobby gives "Connection lost".

Invite links:
9. On plain-HTTP LAN, the waiting screen shows the link with no Copy button. On `http://localhost` or behind HTTPS, Copy link fills the clipboard.
10. Open `http://localhost:8080/?room=abcd` for a live room. The address bar loses `?room=`, a green "Join room ABCD" button shows, and one click joins. Reloading shows no button.

Layout and other builds:
11. Phone-landscape size: the lobby list scrolls and the footer buttons stay visible.
12. The desktop build is unchanged: no duel section and no lobby.

- [ ] **Step 2: README**

In "Playing a duel", replace step 1 (create and share the code) with:

```
1. Quick match pairs you with whoever is waiting, or waits for the next player.
   Browse rooms lists open public rooms; click Join on one.
   Create room makes a room with a 4-letter code (tick Public to also list it in Browse rooms).
   Share the code, or the invite link on the waiting screen, which opens the game with a Join button ready.
```

Renumber the following steps.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: room lobby, quick match and invite links"
```
