# Accounts, Saved Settings, and Career Stats Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Registered web players log in with a username and password and get their settings, solo bests, and a server-written career record of online duels and royales back on any browser; guests play exactly as today.

**Architecture:**
- A SQLite `Store` (`server/store.go`) owns users, sessions, settings text, solo bests, and career totals.
- A same-origin JSON API (`server/api.go`) sets the `jestris_session` cookie.
- `Hub.ServeHTTP` resolves that cookie into `player.accountID` and the account's name, and keeps one live socket per account.
- The match runner caches each participant's `GameStats` and sim time from a new `sim_stats` wasm export. The hub turns a duel `finish` or a royale `eliminate` into a `Career` delta, captured before `endMatch`, and a background `careerWriter` adds it to the database.
- On the web client, a pure `account.cpp` builds request bodies and parses replies. A polling fetch bridge (`account_store_web.cpp`) sits next to `settings_store_web.cpp`. `App` routes `saveSettings()` and new bests to the API while logged in.

**Tech Stack:** Go 1.25 module `tetris-server` with `github.com/coder/websocket`, `github.com/tetratelabs/wazero`, new `modernc.org/sqlite` and `golang.org/x/crypto/bcrypt` (pure Go, `CGO_ENABLED=0` still builds). C++20, Dear ImGui, doctest, Emscripten (`EM_JS`).

**Spec:** `docs/superpowers/specs/2026-10-29-accounts-and-stats-design.md`

## Global Constraints

- **Database:** `modernc.org/sqlite`, flag `-db` default `jestris.db` (relative to the working directory). WAL on open. Tables created with `CREATE TABLE IF NOT EXISTS`, using the exact columns in spec section 1. No migration tool.
- **Usernames:** `validName` in `server/hub.go` (1 to 12 of `A-Z a-z 0-9 _ - .`). Unique with `COLLATE NOCASE`. Stored and shown as registered. Never changed.
- **Passwords:** 8 to 72 **bytes**. bcrypt at `bcrypt.DefaultCost`. Never stored in plaintext or logged.
- **Session token:** 32 random bytes, hex in the cookie. `sessions.token_hash` is the hex SHA-256 of the 32 raw bytes.
- **Cookie:** `jestris_session`, `Path=/`, `HttpOnly`, `SameSite=Lax`, `Max-Age` 30 days (2592000). `Secure` when `r.TLS != nil` or `X-Forwarded-Proto: https`.
- **Error copy, exactly:** `wrong username or password` (login mismatch and unknown user, both `401`), `username is taken` (`409`), `username must be 1 to 12 of A-Z a-z 0-9 _ - .` (`409`), `password must be 8 to 72 bytes` (`400`), `try again later` (`429`), `not logged in` (`401`), `wrong password` (`403`, password change), `bad request` (`400`). Bodies are plain text from `http.Error`, and the client trims the trailing newline.
- **Limits:** login failures 10 per lower-cased username per 15 minutes. Registrations 5 per client IP per hour, counting accounts created **(plan call)**. Both in memory. Client IP is the first `X-Forwarded-For` entry only when the TCP peer is loopback, and otherwise the peer address. **(plan call)** Every API body is capped at 16 KiB with `http.MaxBytesReader`.
- **Career:** account players only. A duel `LEFT` writes nothing. `max_combo` takes the max. Every other column is a sum. A match with no runner records the verdict, KOs, place, and `sent`, but no sim counters and no `ms_played`.
- **Rates:** PPS `pieces/(ms/1000)`, APM `attack/(ms/60000)`, VS `(attack+garbage_cleared)/(ms/1000)*100`. All three are 0 when `ms_played` is 0. The average place is omitted when `royale_matches` is 0.
- **Desktop build:** no account UI, no fetches, unchanged settings file. `accountsSupported()` returns false there.
- **Git-ignored:** `server/sim/jestris_sim.wasm` (rebuild after Task 1, before any `go test`) and `jestris.db*`.
- **Unchanged:** room codes, host start, match rules, wire protocol (except that `HELLO` is ignored on an authenticated socket), solo rules, vs Bot. Every existing test still passes.

## Verification Commands

- **WASM module:** `emcmake cmake -S . -B build-sim -DCMAKE_BUILD_TYPE=Release -DTETRIS_SIM_ONLY=ON && cmake --build build-sim --target jestris_sim && cp build-sim/jestris_sim.wasm server/sim/`
- **Go:** `go -C server test -race ./...`. To run one test: `go -C server test -race -run TestName ./...`.
- **C++:** `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`. To run one suite: `./build/tetris_tests -tc="account:*"`, or `./build/tetris_ui_tests -tc="ui:*"`.
- **Web build:** `emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web`

## Review Focus

1. **A refresh or a second tab with the same account.** The older socket is closed. When the older socket's leave runs late, it must not unregister the newer one, so a third tab still kicks the second. Pinned in Task 5 by `TestOlderSocketsLeaveDoesNotUnregisterTheNewer`.
2. **Logging in with different case.** `sam` logs into `Sam`, and rooms show `Sam`, not `sam`. Pinned in Task 3 by `TestLoginIgnoresCaseAndKeepsTheRegisteredName`, and in Task 5 by `TestAuthenticatedSocketUsesTheAccountNameAndIgnoresHello`.
3. **Passwords with quotes, backslashes, non-ASCII, or exactly 72 bytes.** Lengths count bytes, not runes. The client escapes the password in JSON so it reaches the server byte for byte. Pinned in Task 3 by `TestPasswordLengthCountsBytes` and in Task 9 by `account: credentials escape quotes, backslashes and control bytes`.
4. **A stale, malformed, or logged-out cookie on `/ws`.** The socket is a guest and uses `HELLO`. It is not refused. Pinned in Task 5 by `TestBadSessionCookieIsAGuest`.
5. **A server restart.** A session cookie issued before the restart still works, and the settings, bests, and career survive. Pinned in Task 4 by `TestReopenedStoreKeepsSessionsAndData`.

---

### Task 1: `sim_stats` wasm export and `Match.Stats`

**Files:**
- Modify: `src/sim/sim_api.cpp` (add the export after `sim_snapshot`)
- Modify: `server/sim/sim.go` (`exportNames`, new `Stats` type and method)
- Test: `server/sim/sim_test.go`

**Interfaces:**
- Produces: `type Stats struct { Pieces, Attack, GarbageCleared, Cancelled, Tetrises, TSpins, MaxCombo, T int }` and `func (m *Match) Stats(h int) (Stats, error)` in package `sim`.

- [ ] **Step 1: Write the failing tests** in `server/sim/sim_test.go`

```go
func TestStatsCountPiecesAndTime(t *testing.T) {
	m := newMatch(t)
	h, err := m.New(5, 1, 2, 20)
	if err != nil {
		t.Fatal(err)
	}
	if err := m.Input(h, CodeHardDrop); err != nil {
		t.Fatal(err)
	}
	if err := m.Advance(h, 250); err != nil {
		t.Fatal(err)
	}
	st, err := m.Stats(h)
	now, _ := m.T(h)
	if err != nil || st.Pieces != 1 || st.T != now || st.T <= 0 || st.Attack != 0 {
		t.Fatalf("stats %+v (t %d) err %v", st, now, err)
	}
}

func TestStatsOfABadHandleIsAnError(t *testing.T) {
	m := newMatch(t)
	if _, err := m.Stats(99); err == nil {
		t.Fatal("want an error")
	}
}
```

- [ ] **Step 2: Run them and confirm they fail**

Run: `go -C server test -run TestStats ./sim`
Expected: a compile failure, `m.Stats undefined`.

- [ ] **Step 3: Implement `sim_stats` in `src/sim/sim_api.cpp`**

`SIM_EXPORT("sim_stats") int sim_stats(int h)` writes `"<pieces> <attack> <garbageCleared> <cancelled> <tetrises> <tSpins> <maxCombo> <t>"` (the fields of `at(h).game().stats()` in `GameStats` order, then `at(h).t()`) into `g_buffer` and returns the length. Use `std::snprintf`, and `__builtin_trap()` if the length is ever `>= kBufferSize`.

- [ ] **Step 4: Implement `Match.Stats` in `server/sim/sim.go`**

Add `"sim_stats"` to `exportNames`. `Stats` calls the export and reads `n` bytes at `m.bufPtr`, as `Snapshot` does. It parses exactly 8 space-separated fields with `parseDigits(field, math.MaxInt32)`. A wrong count or a bad field poisons the match (`m.err`) and returns that error.

- [ ] **Step 5: Rebuild the module and run all sim tests**

Run the **WASM module** command, then `go -C server test -race ./...`.
Expected: PASS, including `TestFixturesReplay`.

- [ ] **Step 6: Commit**

```bash
git add src/sim/sim_api.cpp server/sim/sim.go server/sim/sim_test.go
git commit -m "feat(sim): sim_stats export with GameStats and sim time"
```

---

### Task 2: The runner keeps each participant's latest stats

**Files:**
- Modify: `server/authority.go` (`gameSim`, `playerSim`, `advanceTo`)
- Test: `server/authority_test.go` (`fakeSim` gains `stats`)

**Interfaces:**
- Consumes: `sim.Stats` and `(*sim.Match).Stats` from Task 1.
- Produces: `gameSim` gains `Stats(h int) (sim.Stats, error)`. `func (r *matchRunner) statsOf(p *player) (sim.Stats, bool)` is safe to call under `Hub.mu` while the runner goroutine runs. It returns false for a non-participant or a game that never started.

- [ ] **Step 1: Write the failing test** in `server/authority_test.go`

Add the field `stats map[int]sim.Stats` to `fakeSim`, initialize it in `newFakeSim`, and add `func (f *fakeSim) Stats(h int) (sim.Stats, error) { return f.stats[h], nil }`. The stub is not recorded, so `steps` is unchanged. Then:

```go
func TestRunnerKeepsTheLatestStats(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	r := d.runner()
	if _, ok := r.statsOf(d.ps[0]); ok {
		t.Fatal("stats before the game started")
	}
	want := sim.Stats{Pieces: 7, Attack: 3, MaxCombo: 2, T: 1000}
	d.sim().stats[0] = want
	d.at(2000) // horizon 1000: tick advances both games
	r.tick(d.h, d.now)
	if got, ok := r.statsOf(d.ps[0]); !ok || got != want {
		t.Fatalf("statsOf = %+v %v, want %+v", got, ok, want)
	}
	if _, ok := r.statsOf(testPlayer("Zed")); ok {
		t.Fatal("stats for a non-participant")
	}
}
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `go -C server test -race -run TestRunnerKeepsTheLatestStats ./...`
Expected: a compile failure, `r.statsOf undefined`.

- [ ] **Step 3: Implement it in `server/authority.go`**

Add `Stats(h int) (sim.Stats, error)` to `gameSim`, and `stats atomic.Pointer[sim.Stats]` to `playerSim`. At the end of `advanceTo`, when the simulation has not failed, call the new `func (r *matchRunner) refreshStats(ps *playerSim)`. It calls `r.sim.Stats(ps.h)`, calls `r.fail(ps, err)` on an error, and otherwise stores the result. `statsOf` looks up `r.players[p]` and loads the pointer.

- [ ] **Step 4: Run all Go tests**

Run: `go -C server test -race ./...`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add server/authority.go server/authority_test.go
git commit -m "feat(server): runner caches each player's GameStats and sim time"
```

---

### Task 3: SQLite store: schema, accounts, sessions, password change

**Files:**
- Create: `server/store.go`
- Test: `server/store_test.go`
- Modify: `server/go.mod`, `server/go.sum` (`go -C server get modernc.org/sqlite golang.org/x/crypto`)
- Modify: `.gitignore` (add `jestris.db*`)

**Interfaces:**
- Produces:
  - `type Store struct { db *sql.DB; now func() time.Time }`
  - `func OpenStore(path string) (*Store, error)` and `func (s *Store) Close() error`
  - `type Account struct { ID int64; Username string }`
  - Sentinel errors `errBadName`, `errNameTaken`, `errBadPassword`, `errWrongLogin`, `errNoSession`
  - `func (s *Store) Register(ctx context.Context, username, password string) (Account, string, error)`, which returns the account and the cookie token
  - `func (s *Store) Login(ctx context.Context, username, password string) (Account, string, error)`
  - `func (s *Store) Session(ctx context.Context, token string) (Account, error)`
  - `func (s *Store) Logout(ctx context.Context, token string) error`
  - `func (s *Store) ChangePassword(ctx context.Context, userID int64, keepToken, current, next string) error`, which returns `errWrongLogin` for a wrong current password and `errBadPassword` for a bad new one

- [ ] **Step 1: Write the failing tests** in `server/store_test.go`

Add a helper `openTestStore(t) *Store` that opens `filepath.Join(t.TempDir(), "test.db")` and closes the store in `t.Cleanup`. Then write these tests:

```go
func TestOpenEnablesWAL(t *testing.T) {
	s := openTestStore(t)
	var mode string
	if err := s.db.QueryRow("PRAGMA journal_mode").Scan(&mode); err != nil || mode != "wal" {
		t.Fatalf("journal_mode %q err %v", mode, err)
	}
}

func TestRegisterRejectsADuplicateRegardlessOfCase(t *testing.T) {
	s := openTestStore(t)
	if _, _, err := s.Register(ctx, "Sam", "password1"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := s.Register(ctx, "sAM", "password2"); !errors.Is(err, errNameTaken) {
		t.Fatalf("err %v, want errNameTaken", err)
	}
}

func TestRegisterRejectsBadNames(t *testing.T) // "", "thirteenchars", "a b", "a:b" -> errBadName

func TestPasswordLengthCountsBytes(t *testing.T) {
	// "1234567" -> errBadPassword; strings.Repeat("é", 4) (8 bytes) -> ok;
	// strings.Repeat("x", 72) -> ok; strings.Repeat("x", 73) -> errBadPassword
}

func TestLoginIgnoresCaseAndKeepsTheRegisteredName(t *testing.T) {
	// Register "Sam"; Login("sam", same password) -> Account{Username: "Sam"} and a token whose
	// Session() resolves to the same Account.
}

func TestLoginMismatchAndUnknownUserAreTheSameError(t *testing.T) {
	// wrong password and "Nobody" both -> errWrongLogin
}

func TestTokensAreHashedAtRest(t *testing.T) {
	// After Register: len(token) == 64, and the only sessions.token_hash equals
	// hex(sha256(hex-decoded token)), which is not the token.
}

func TestLogoutEndsTheSession(t *testing.T) {
	// Logout(token); Session(token) -> errNoSession. Session("zz"), Session("") -> errNoSession.
}

func TestChangePasswordKeepsThisSessionAndEndsOthers(t *testing.T) {
	// Register (token a) and Login (token b). ChangePassword(id, a, "wrong-pass", "newpass12") -> errWrongLogin.
	// ChangePassword(id, a, "password1", "short") -> errBadPassword.
	// ChangePassword(id, a, "password1", "newpass12") -> nil. Then Session(a) ok, Session(b) -> errNoSession,
	// Login with "password1" -> errWrongLogin, and Login with "newpass12" -> ok.
}
```

- [ ] **Step 2: Run them and confirm they fail**

Run: `go -C server test -race -run 'TestOpen|TestRegister|TestPassword|TestLogin|TestTokens|TestLogout|TestChangePassword' ./...`
Expected: a compile failure, `undefined: OpenStore`.

- [ ] **Step 3: Implement `server/store.go`**

`OpenStore` opens `"file:" + path + "?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)&_pragma=foreign_keys(1)"` with driver `"sqlite"`, calls `db.SetMaxOpenConns(1)` **(plan call: one connection, so there is never `SQLITE_BUSY`)**, and runs the five `CREATE TABLE IF NOT EXISTS` statements from spec section 1 verbatim. `created_at` is `s.now().UTC().Format(time.RFC3339)`.

`Register` checks `validName`, then the byte length, then `bcrypt.GenerateFromPassword(..., bcrypt.DefaultCost)`. In one transaction it inserts the `users` row, an empty `settings`, `solo_bests`, and `career` row, and the session. A unique-constraint failure is recognized with `errors.As(err, &*sqlite.Error)` and `Code() == sqlite3.SQLITE_CONSTRAINT_UNIQUE` (`modernc.org/sqlite/lib`), and becomes `errNameTaken`.

`Login` with an unknown user still runs `bcrypt.CompareHashAndPassword` against a package-level dummy hash, so both failures take the same time.

The token helpers are `newToken() (token, hash string, err error)`, which uses `crypto/rand` for 32 bytes, and `tokenHash(token string) (string, bool)`, which is false unless the token is 64 hex characters. `ChangePassword` runs `DELETE FROM sessions WHERE user_id = ? AND token_hash != ?`.

- [ ] **Step 4: Run all Go tests**

Run: `go -C server test -race ./...`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add server/store.go server/store_test.go server/go.mod server/go.sum .gitignore
git commit -m "feat(server): SQLite store with accounts, hashed sessions and bcrypt passwords"
```

---

### Task 4: Store: settings text, solo bests, career totals

**Files:**
- Modify: `server/store.go`
- Test: `server/store_test.go`

**Interfaces:**
- Consumes: `Store`, `Account`, and `openTestStore` from Task 3.
- Produces:
  - `type Bests struct { SprintMs *int \`json:"sprintMs"\`; CheeseMs *int \`json:"cheeseMs"\`; BlitzScore *int \`json:"blitzScore"\` }`
  - `type Career struct { DuelWins, DuelLosses, DuelDraws, KosDealt, KosTaken, RoyaleMatches, RoyaleWins, RoyalePlaceSum, Pieces, Attack, Sent, GarbageCleared, Cancelled, Tetrises, TSpins, MaxCombo, MsPlayed int }`. This type is both the stored totals and a delta.
  - `type Profile struct { Username, Settings string; Bests Bests; Career Career }`
  - `var errBadMode`
  - `func (s *Store) Profile(ctx context.Context, userID int64) (Profile, error)`
  - `func (s *Store) SaveSettings(ctx context.Context, userID int64, body string) error`
  - `func (s *Store) SubmitBest(ctx context.Context, userID int64, mode string, result int) (Bests, error)`, where `mode` is `sprint`, `cheese`, or `blitz`, anything else is `errBadMode`, and a negative result is `errBadMode`
  - `func (s *Store) AddCareer(ctx context.Context, userID int64, d Career) error`
  - `type careerView struct { DuelWins, DuelLosses, DuelDraws, KosDealt, KosTaken, RoyaleMatches, RoyaleWins int; AvgPlace *float64; PPS, APM, VS float64 }` with JSON names `duelWins duelLosses duelDraws kosDealt kosTaken royaleMatches royaleWins avgPlace pps apm vs`, and `func (c Career) view() careerView`

- [ ] **Step 1: Write the failing tests**

```go
func TestSettingsRoundTripVerbatim(t *testing.T) {
	// A new account's Profile has Settings "" and nil bests. SaveSettings(id, "version=1\nname=Other\nsprint=5\n")
	// makes Profile().Settings exactly that string, and leaves Bests nil: the server does not read the name or best lines.
}

func TestSprintAndCheeseKeepTheFasterTime(t *testing.T) {
	// sprint: 60000 -> 60000; 61000 -> 60000; 60000 -> 60000; 59000 -> 59000. The same for cheese.
	// Each SubmitBest returns all three bests, and the others stay nil.
}

func TestBlitzKeepsTheHigherScore(t *testing.T) { // 5000 -> 5000; 4000 -> 5000; 0 on a fresh account -> 0
}

func TestSubmitBestRejectsBadInput(t *testing.T) { // "marathon", "zen", "" and result -1 -> errBadMode
}

func TestAddCareerSumsAndKeepsTheMaxCombo(t *testing.T) {
	// AddCareer(Career{DuelWins: 1, Pieces: 100, MaxCombo: 5, MsPlayed: 60000}), then
	// AddCareer(Career{DuelLosses: 1, Pieces: 50, MaxCombo: 3, MsPlayed: 30000})
	// -> Career{DuelWins: 1, DuelLosses: 1, Pieces: 150, MaxCombo: 5, MsPlayed: 90000}
}

func TestCareerViewRates(t *testing.T) {
	c := Career{Pieces: 150, Attack: 90, GarbageCleared: 30, MsPlayed: 60000, RoyaleMatches: 4, RoyalePlaceSum: 10}
	v := c.view()
	// v.PPS == 2.5, v.APM == 90, v.VS == 200, *v.AvgPlace == 2.5
	z := Career{}.view() // PPS, APM, VS == 0, AvgPlace == nil
}

func TestReopenedStoreKeepsSessionsAndData(t *testing.T) {
	// Open a path, Register, SaveSettings, SubmitBest, AddCareer, and Close. Reopen the same path:
	// Session(token) resolves, and Profile shows the same settings, best and career.
}
```

- [ ] **Step 2: Run them and confirm they fail**

Run: `go -C server test -race -run 'TestSettings|TestSprint|TestBlitz|TestSubmitBest|TestAddCareer|TestCareerView|TestReopened' ./...`
Expected: a compile failure, `undefined: Career`.

- [ ] **Step 3: Implement them in `server/store.go`**

`SubmitBest` runs one conditional `UPDATE`: `SET sprint_ms = ? WHERE user_id = ? AND (sprint_ms IS NULL OR sprint_ms > ?)`. Cheese uses the same shape. Blitz compares with `<`. It then selects the three columns.

`AddCareer` runs one `UPDATE career SET col = col + ?, ..., max_combo = MAX(max_combo, ?) WHERE user_id = ?`.

`view()` computes the rates in `float64` as the Global Constraints define them.

- [ ] **Step 4: Run all Go tests**

Run: `go -C server test -race ./...`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add server/store.go server/store_test.go
git commit -m "feat(server): stored settings text, better-only solo bests and career totals"
```

---

### Task 5: The WebSocket knows the account

**Files:**
- Modify: `server/hub.go` (`Hub`, `player`, `NewHub`, `ServeHTTP`, `route`'s `HELLO` branch, `leave`)
- Create: `server/hub_accounts_test.go`

**Interfaces:**
- Consumes: `Account` and `errNoSession` from Task 3.
- Produces:
  - `type sessionLookup interface { Session(ctx context.Context, token string) (Account, error) }`. `*Store` satisfies it.
  - Hub fields `sessions sessionLookup` (nil means no accounts) and `accounts map[int64]*player` (guarded by `mu`).
  - Player field `accountID int64` (0 means a guest).
  - `const sessionCookie = "jestris_session"`
  - `func (h *Hub) attach(p *player, a Account)`, which takes the lock itself
  - `func (h *Hub) DisconnectAccount(userID int64)`

- [ ] **Step 1: Write the failing tests** in `server/hub_accounts_test.go`

Add these helpers:
- `type fakeSessions map[string]Account` with a `Session` method that returns `errNoSession` for an unknown token.
- `dialAs(t, srv, token) *client`, which is `dial` with `websocket.DialOptions{HTTPHeader: http.Header{"Cookie": {sessionCookie + "=" + token}}}`.
- `(c *client) expectClosed()`, which reads until an error within 5 s and fails on timeout.

Build the hub with `newConfiguredHub(t, func(h *Hub) { h.sessions = fakeSessions{"tokS": {ID: 1, Username: "Sam"}} })`.

```go
func TestAuthenticatedSocketUsesTheAccountNameAndIgnoresHello(t *testing.T) {
	c := dialAs(t, srv, "tokS")
	c.send("HELLO Bob")
	c.send("CREATE 3")
	c.expect("ROOM AAAA 1")
	c.expect("PLAYERS 1 1:Sam:0")
}

func TestBadSessionCookieIsAGuest(t *testing.T) {
	c := dialAs(t, srv, "nope")
	c.send("HELLO Bob")
	c.send("CREATE 3")
	c.expect("ROOM AAAA 1")
	c.expect("PLAYERS 1 1:Bob:0")
}

func TestSecondSocketForAnAccountClosesTheFirst(t *testing.T) {
	a := dialAs(t, srv, "tokS")
	a.send("CREATE 3")
	a.expect("ROOM AAAA 1")
	a.expect("PLAYERS 1 1:Sam:0")
	b := dialAs(t, srv, "tokS")
	a.expectClosed()
	b.send("CREATE 3")
	b.expect("ROOM AAAB 1")
	b.expect("PLAYERS 1 1:Sam:0")
}

// Driven directly, so a's late leave happens at a known point.
func TestOlderSocketsLeaveDoesNotUnregisterTheNewer(t *testing.T) {
	h := NewHub()
	a, b, c := testPlayer(""), testPlayer(""), testPlayer("")
	sam := Account{ID: 1, Username: "Sam"}
	h.attach(a, sam)
	h.attach(b, sam) // kicks a
	h.leave(a)
	h.attach(c, sam)
	if !a.kicked || !b.kicked || c.kicked {
		t.Fatalf("kicked a=%v b=%v c=%v, want true true false", a.kicked, b.kicked, c.kicked)
	}
}

func TestGuestsNeverKickAnyone(t *testing.T) {
	// Two plain dial() clients, both HELLO Sam. Both create rooms and get ROOM: neither is closed.
}

func TestDisconnectAccountClosesThatSocket(t *testing.T) {
	// Keep the *Hub from newConfiguredHub's configure. Dial "tokS", send "LIST", expect "ROOMS".
	// h.DisconnectAccount(1); expectClosed(). DisconnectAccount(2) with no socket does nothing.
}
```

- [ ] **Step 2: Run them and confirm they fail**

Run: `go -C server test -race -run 'TestAuthenticated|TestBadSession|TestSecondSocket|TestOlderSockets|TestGuestsNever|TestDisconnectAccount' ./...`
Expected: a compile failure, `h.sessions undefined`.

- [ ] **Step 3: Implement it in `server/hub.go`**

`NewHub` initializes `accounts`. In `ServeHTTP`, before `websocket.Accept`, read `r.Cookie(sessionCookie)`. When `h.sessions != nil`, call `Session(r.Context(), value)`. An error other than `errNoSession` is logged, and that socket is a guest. After `newPlayer`, when there is an account, call `attach`. It disconnects `h.accounts[a.ID]` if that is set, stores `p`, and sets `p.accountID` and `p.name`.

`route`'s `HELLO` branch returns early when `p.accountID != 0`. `leave` deletes `h.accounts[p.accountID]` only when it still equals `p`. `DisconnectAccount` takes the lock and calls `disconnect()` on the registered player, if any.

- [ ] **Step 4: Run all Go tests**

Run: `go -C server test -race ./...`
Expected: PASS. The existing hub tests have `sessions == nil`.

- [ ] **Step 5: Commit**

```bash
git add server/hub.go server/hub_accounts_test.go
git commit -m "feat(server): the session cookie names the socket; one live socket per account"
```

---

### Task 6: HTTP API with cookie and rate limits

**Files:**
- Create: `server/api.go`
- Test: `server/api_test.go`

**Interfaces:**
- Consumes: the `Store` methods from Tasks 3 and 4, `careerView`, and `Hub.DisconnectAccount` from Task 5.
- Produces:
  - `type accountAPI struct { store *Store; hub *Hub; now func() time.Time; logins, registrations *rateLimiter }`
  - `func newAccountAPI(store *Store, hub *Hub) *accountAPI`
  - `func (a *accountAPI) routes(mux *http.ServeMux)`, which registers `POST /api/register`, `POST /api/login`, `POST /api/logout`, `GET /api/me`, `PUT /api/settings`, `POST /api/password`, and `POST /api/bests`
  - `type rateLimiter`, with `newRateLimiter(max int, window time.Duration, now func() time.Time) *rateLimiter`, `blocked(key string) bool`, and `add(key string)`
  - `func clientIP(r *http.Request) string`
  - The JSON shapes:
    - `POST /api/register` and `POST /api/login` answer `200` with the same body as `GET /api/me`.
    - `GET /api/me` answers `{"username","settings","sprintMs","cheeseMs","blitzScore","career":careerView}`.
    - `POST /api/bests` answers `Bests`.
    - The other routes answer `204`.

- [ ] **Step 1: Write the failing tests** in `server/api_test.go`

Add a helper `newAPIServer(t) (*httptest.Server, *accountAPI)`. It builds a temp `Store` and `NewHub()`, calls `a.routes(mux)`, and mounts `mux.Handle("/ws", hub)`. Add `newBrowser(t) *http.Client` with a `cookiejar`, and `call(t, c, method, url, body string) (int, string)`.

```go
func TestRegisterLoginMeLogout(t *testing.T) {
	// register {"username":"Sam","password":"password1"} -> 200 with username "Sam", and a
	// Set-Cookie jestris_session with HttpOnly, SameSite=Lax, Path=/, Max-Age=2592000, and no Secure.
	// GET /api/me -> 200 {"username":"Sam","settings":"","sprintMs":null,...,"career":{...,"avgPlace":null,"pps":0,...}}
	// POST /api/logout -> 204; GET /api/me -> 401 "not logged in"
	// a fresh browser: login {"username":"sam","password":"password1"} -> 200 username "Sam"
}

func TestRegisterErrors(t *testing.T) {
	// duplicate "SAM" -> 409 "username is taken"; "bad name" -> 409 "username must be 1 to 12 of A-Z a-z 0-9 _ - ."
	// password "short" -> 400 "password must be 8 to 72 bytes"; body "{" -> 400 "bad request"
}

func TestLoginMismatchAndUnknownUserLookTheSame(t *testing.T) {
	// a wrong password and user "Nobody": both 401 with the identical body "wrong username or password"
}

func TestSecureCookieBehindHTTPS(t *testing.T) // X-Forwarded-Proto: https -> the Set-Cookie has Secure

func TestSettingsRoundTrip(t *testing.T) // PUT /api/settings "version=1\nsound=off\n" -> 204; me.settings is that string

func TestBestsKeepOnlyBetterResults(t *testing.T) {
	// sprint 60000 -> {"sprintMs":60000,"cheeseMs":null,"blitzScore":null}; sprint 65000 -> 60000 still
	// blitz 9000 then 8000 -> 9000; {"mode":"zen","result":1} -> 400; result -1 -> 400
}

func TestPasswordChange(t *testing.T) {
	// browser A registers and browser B logs in. A posts {"current":"nope-nope","next":"newpass12"} -> 403 "wrong password".
	// A posts {"current":"password1","next":"newpass12"} -> 204; A's /api/me -> 200; B's /api/me -> 401.
	// login with password1 -> 401; login with newpass12 -> 200
}

func TestLoggedInRoutesNeedASession(t *testing.T) // logout, me, settings, password, bests with no cookie -> 401

func TestLogoutClosesTheAccountsSocket(t *testing.T) {
	// register, then dial /ws with the jar's cookie, send LIST, expect ROOMS; POST /api/logout; the socket closes
}

func TestLoginFailuresAreCappedPerUsername(t *testing.T) {
	// set a.now to a settable clock. Ten wrong logins for "sam" -> 401 each; the eleventh, even with the right
	// password and as "SAM", -> 429 "try again later". Logins for another user still work. Fifteen minutes
	// later the right password -> 200
}

func TestRegistrationsAreCappedPerIP(t *testing.T) // five registrations -> 200; the sixth -> 429; an hour later -> 200

func TestClientIP(t *testing.T) {
	// RemoteAddr "127.0.0.1:5000" with X-Forwarded-For "203.0.113.9, 10.0.0.1" -> "203.0.113.9"
	// RemoteAddr "[::1]:5000" with the same header -> "203.0.113.9"
	// RemoteAddr "198.51.100.7:5000" with the same header -> "198.51.100.7"
	// RemoteAddr "127.0.0.1:5000" with no header -> "127.0.0.1"
}
```

- [ ] **Step 2: Run them and confirm they fail**

Run: `go -C server test -race -run 'TestRegisterLogin|TestRegisterErrors|TestLoginMismatch|TestSecureCookie|TestSettingsRoundTrip$|TestBests|TestPasswordChange|TestLoggedInRoutes|TestLogoutCloses|TestLoginFailures|TestRegistrationsAre|TestClientIP' ./...`
Expected: a compile failure, `undefined: newAccountAPI`.

- [ ] **Step 3: Implement `server/api.go`**

`newAccountAPI` builds `logins = newRateLimiter(10, 15*time.Minute, ...)` and `registrations = newRateLimiter(5, time.Hour, ...)`. Both read the time through `a.now`, so a test can swap the clock. `rateLimiter` keeps `map[string][]time.Time` under a mutex, prunes entries older than the window on each call, and deletes a key whose list becomes empty.

The handlers wrap `r.Body` in `http.MaxBytesReader(w, r.Body, 16<<10)`. Add a helper `func (a *accountAPI) session(r *http.Request) (Account, string, bool)`.

`login` checks `logins.blocked(strings.ToLower(username))` before bcrypt, and calls `add` only on `errWrongLogin`. `register` checks `registrations.blocked(clientIP(r))`, and calls `add` after a successful `Register`. `logout` deletes the session, clears the cookie with `MaxAge: -1`, and calls `a.hub.DisconnectAccount(id)`. `password` maps `errWrongLogin` to `403 wrong password` and `errBadPassword` to `400`. `bests` maps `errBadMode` and bad JSON to `400 bad request`. Store errors other than the sentinels are logged and return `500`.

- [ ] **Step 4: Run all Go tests**

Run: `go -C server test -race ./...`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add server/api.go server/api_test.go
git commit -m "feat(server): account API with session cookie and login/registration limits"
```

---

### Task 7: The hub records career deltas at match end

**Files:**
- Create: `server/career.go`
- Modify: `server/hub.go` (`Hub.career`, `match.career`, `begin`, `finish`, `eliminate`)
- Test: `server/career_test.go`

**Interfaces:**
- Consumes: `matchRunner.statsOf` from Task 2, `Career` and `Store.AddCareer` from Task 4, and `player.accountID` from Task 5.
- Produces:
  - `type careerRecorder interface { Record(userID int64, d Career) }`, with Hub field `career careerRecorder` (nil records nothing)
  - `func (m *match) careerBase(p *player) Career`, which fills `Sent` from `m.sent[p]` and, when `m.runner != nil` and `statsOf` is ok, the sim counters with `MsPlayed = T`
  - `func (m *match) recordDuel()` and `func (m *match) recordPlace(p *player, place int)`
  - `type careerWriter`, with `newCareerWriter(store *Store) *careerWriter`, `Record` (non-blocking, queue 256, a full queue is logged and dropped), and `Close()` (writes what is queued, then returns)

- [ ] **Step 1: Write the failing tests** in `server/career_test.go`

Add a helper `type fakeCareer struct { t *testing.T; mu sync.Mutex; got map[int64]Career }`. Its `Record` stores the delta and fails the test if one account is recorded twice in a match. Use `directMatchWith(t, func(h *Hub) { h.career = fc }, ...)` and set `d.ps[i].accountID` after setup.

```go
func TestDuelRecordsBothAccounts(t *testing.T) {
	// Ann is account 1 and Bob is account 2. Script sim stats h0 {Pieces: 40, Attack: 12, MaxCombo: 3, T: 60000}
	// and h1 {Pieces: 30, T: 60000}. d.at(2000), then tick. Ann sends "ATK 2" (log mode counts sent) and drains.
	// Bob sends DEAD three times -> OVER.
	// Ann: Career{DuelWins: 1, KosDealt: 3, Sent: 2, Pieces: 40, Attack: 12, MaxCombo: 3, MsPlayed: 60000}
	// Bob: Career{DuelLosses: 1, KosTaken: 3, Pieces: 30, MsPlayed: 60000}
}

func TestDuelDrawRecordsADrawForBoth(t *testing.T) // timer(0).fire() with nothing sent -> DuelDraws 1 each

func TestDuelGuestSeatRecordsNothingForTheGuest(t *testing.T) // only Ann has an account -> one entry, Bob's absent

func TestDuelLeftRecordsNothing(t *testing.T) // both accounts; d.h.leave(d.ps[1]) -> fc.got is empty

func TestRoyaleRecordsEachAccountsPlace(t *testing.T) {
	// Ann, Bob, Cat and Dan; Dan is a guest. Bob DEAD -> place 4; Dan DEAD -> 3; d.h.leave(Cat) -> place 2, Ann wins.
	// Bob {RoyaleMatches: 1, RoyalePlaceSum: 4}; Cat {RoyaleMatches: 1, RoyalePlaceSum: 2};
	// Ann {RoyaleMatches: 1, RoyaleWins: 1, RoyalePlaceSum: 1}; no entry for Dan
}

func TestCareerWithoutASimulationKeepsTheVerdict(t *testing.T) {
	// h.newSim returns an error (log mode: runner nil). Ann sends "ATK 3"; Bob sends DEAD three times.
	// Ann {DuelWins: 1, KosDealt: 3, Sent: 3} and every sim counter and MsPlayed is 0
}

func TestCareerWriterAddsToTheStore(t *testing.T) {
	// A temp store and a registered Sam. w := newCareerWriter(s); w.Record(id, Career{DuelWins: 1, MaxCombo: 4});
	// w.Record(id, Career{DuelWins: 1, MaxCombo: 2}); w.Close(); Profile -> DuelWins 2, MaxCombo 4
}
```

- [ ] **Step 2: Run them and confirm they fail**

Run: `go -C server test -race -run 'TestDuelRecords|TestDuelDraw|TestDuelGuest|TestDuelLeft|TestRoyaleRecords|TestCareerWithout|TestCareerWriter' ./...`
Expected: a compile failure, `h.career undefined`.

- [ ] **Step 3: Implement `server/career.go` and the hub hooks**

`begin` copies `h.career` into the new field `match.career`. In `finish`, call `m.recordDuel()` **before** `r.endMatch()`. It records `m.verdict(p)` as a win, loss, or draw for each account participant, with `KosDealt = m.kos[p]` and `KosTaken = m.kos[m.opponent(p)]`. In `eliminate`, call `m.recordPlace(p, place)`. When `place == 2`, also call `m.recordPlace(winner, 1)`, both before `r.endMatch()`.

`recordPlace` sets `RoyaleMatches = 1`, sets `RoyaleWins = 1` when the place is 1, and sets `RoyalePlaceSum = place`. Both record functions skip `accountID == 0` and a nil `m.career`.

`careerWriter` runs one goroutine that calls `store.AddCareer(context.Background(), ...)`. An error is logged as `career write user=<id>: <err>`, and the match is unaffected.

- [ ] **Step 4: Run all Go tests**

Run: `go -C server test -race ./...`
Expected: PASS. `finish` and `eliminate` still send the same messages, so the existing hub tests are unchanged.

- [ ] **Step 5: Commit**

```bash
git add server/career.go server/career_test.go server/hub.go
git commit -m "feat(server): record duel verdicts, royale places and sim totals for accounts"
```

---

### Task 8: Wire the store, API, and career writer into `main`

**Files:**
- Modify: `server/main.go`
- Create: `server/main_test.go`
- Modify: `README.md` (one paragraph under the server flags: `-db`, where the file lives, and that `jestris.db`, `jestris.db-wal`, and `jestris.db-shm` are the data)

**Interfaces:**
- Consumes: `OpenStore`, `newAccountAPI`, `newCareerWriter`, and `Hub.sessions` / `Hub.career`.
- Produces: `func newMux(web http.Handler, hub *Hub, a *accountAPI) *http.ServeMux`, which mounts `/`, `/ws`, and `a.routes`.

- [ ] **Step 1: Write the failing test** in `server/main_test.go`

```go
func TestMuxServesTheAPIBesideThePage(t *testing.T) {
	// newMux(http.HandlerFunc(func(w, r) { io.WriteString(w, "page") }), NewHub(), newAccountAPI(tempStore, hub))
	// GET /api/me -> 401; GET / -> "page"; dial /ws -> LIST -> ROOMS
}
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `go -C server test -race -run TestMuxServesTheAPIBesideThePage ./...`
Expected: a compile failure, `undefined: newMux`.

- [ ] **Step 3: Implement the wiring in `server/main.go`**

Add `db := flag.String("db", "jestris.db", "SQLite file for accounts, settings and career stats")`. `OpenStore(*db)` failing calls `log.Fatalf`. Set `hub.sessions = store` and `hub.career = newCareerWriter(store)`, then replace the inline `mux` setup with `newMux(web, hub, newAccountAPI(store, hub))`.

- [ ] **Step 4: Run all Go tests and a CGO-free build**

Run: `go -C server test -race ./... && CGO_ENABLED=0 go -C server build -o /dev/null .`
Expected: PASS, and the build succeeds.

- [ ] **Step 5: Commit**

```bash
git add server/main.go server/main_test.go README.md
git commit -m "feat(server): -db flag; serve the account API and record careers"
```

---

### Task 9: Client: pure account requests and replies

**Files:**
- Create: `src/app/account.hpp`, `src/app/account.cpp`
- Create: `tests/account_test.cpp`
- Modify: `CMakeLists.txt` (`src/app/account.cpp` in `tetris_settings`, and `tests/account_test.cpp` in `tetris_tests`)

**Interfaces:**
- Produces, in namespace `tetris`:
  - `inline constexpr int kMinPasswordBytes = 8; inline constexpr int kMaxPasswordBytes = 72;`
  - `enum class AccountCall { Me, Register, Login, Logout, Password, Settings, Best };`
  - `struct AccountRoute { const char* method; const char* path; }; AccountRoute routeFor(AccountCall call);`
  - `struct AccountReply { AccountCall call = AccountCall::Me; int status = 0; std::string body; };`, where status 0 means the server could not be reached
  - `std::string jsonQuote(std::string_view text);`
  - `std::string credentialsBody(std::string_view username, std::string_view password);`, which builds `{"username":…,"password":…}`
  - `std::string passwordBody(std::string_view current, std::string_view next);`, which builds `{"current":…,"next":…}`
  - `std::optional<std::string> bestBody(SoloMode mode, int result);`, which is nullopt for Marathon and Zen
  - `struct CareerView { int duelWins = 0, duelLosses = 0, duelDraws = 0, kosDealt = 0, kosTaken = 0, royaleMatches = 0, royaleWins = 0; std::optional<double> avgPlace; double pps = 0, apm = 0, vs = 0; };`
  - `struct Profile { std::string username; std::string settings; PersonalBests bests; CareerView career; };`
  - `std::optional<Profile> parseProfile(std::string_view json);` and `std::optional<PersonalBests> parseBests(std::string_view json);`
  - `std::vector<std::string> recordLines(const CareerView& career);`
  - `std::string accountErrorText(int status, std::string_view body);`

- [ ] **Step 1: Write the failing tests** in `tests/account_test.cpp`

```cpp
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
```

- [ ] **Step 2: Run them and confirm they fail**

Run: `cmake -B build && cmake --build build`
Expected: a compile failure, `app/account.hpp` not found.

- [ ] **Step 3: Implement `src/app/account.cpp`**

`jsonQuote` escapes `"`, `\`, and bytes below 0x20 (`\n`, `\r`, `\t` short forms, others `\u00XX`), and passes other bytes through unchanged. The parsers use a small internal recursive-descent JSON reader. It handles objects, strings (with `\" \\ \/ \b \f \n \r \t \uXXXX`, encoded to UTF-8), numbers, `null`, and `true`/`false`, and rejects trailing garbage. A best is `std::optional<int>` from an integral number or `null`. Missing career fields keep their defaults. `recordLines` formats with `%.1f` for the average place, `%.2f` for PPS, and `%.1f` for APM and VS.

- [ ] **Step 4: Run the suite**

Run: `cmake --build build && ./build/tetris_tests -tc="account:*" && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/app/account.hpp src/app/account.cpp tests/account_test.cpp CMakeLists.txt
git commit -m "feat(app): account request bodies, reply parsing and record lines"
```

---

### Task 10: Fetch bridge on the web, a stub on desktop

**Files:**
- Create: `src/app/account_store.hpp`, `src/app/account_store_web.cpp`, `src/app/account_store_desktop.cpp`
- Modify: `CMakeLists.txt` (`set(TETRIS_ACCOUNT ...)` beside `TETRIS_STORE` in both branches, and add `${TETRIS_ACCOUNT}` to `add_executable(tetris ...)`)

**Interfaces:**
- Consumes: `AccountCall`, `AccountReply`, and `routeFor` from Task 9.
- Produces:
  - `bool accountsSupported();`
  - `void sendAccountRequest(AccountCall call, const std::string& body);`, fire and forget, with an empty body for GET and logout
  - `std::vector<AccountReply> takeAccountReplies();`, which returns the replies that arrived since the last call, in arrival order

- [ ] **Step 1: Implement the desktop stub** in `src/app/account_store_desktop.cpp`

It returns `false`, does nothing, and returns `{}`.

- [ ] **Step 2: Implement the web bridge** in `src/app/account_store_web.cpp`

Follow the `settings_store_web.cpp` pattern, with `EM_JS_DEPS(account_store, "$stringToNewUTF8,$UTF8ToString")`.

`EM_JS(void, accountFetch, (int call, const char* method, const char* path, const char* body), ...)` calls `fetch(path, {method, credentials: 'include', body: method === 'GET' ? undefined : text})`. Then it pushes `{call, status: r.status, text}` onto `Module.jestrisAccountReplies`, or `{call, status: 0, text: ''}` on a rejection.

`EM_JS(char*, accountTakeReply, (), ...)` shifts one reply off as `"<call> <status>\n<text>"` through `stringToNewUTF8`, or returns 0. `takeAccountReplies` loops until 0, splits off the header, and calls `std::free` on each string.

- [ ] **Step 3: Build both targets**

Run: `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`, then run the **Web build** command.
Expected: both build, and the tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/app/account_store.hpp src/app/account_store_web.cpp src/app/account_store_desktop.cpp CMakeLists.txt
git commit -m "feat(app): same-origin fetch bridge for the account API (web only)"
```

---

### Task 11: Start menu account block

**Files:**
- Modify: `src/app/render.hpp` (`UiCommand`, `StartMenu`, `#include "app/account.hpp"`)
- Modify: `src/app/render.cpp` (`drawStartScreen`'s `1v1 DUEL` section)
- Test: `tests/ui_test.cpp` (`tetris_ui_tests` already links `tetris_settings`, which holds `account.cpp` from Task 9)

**Interfaces:**
- Consumes: `CareerView`, `recordLines`, and `kMaxPasswordBytes` / `kMinPasswordBytes` from Task 9.
- Produces:
  - `UiCommand` gains `Register, Login, Logout, ChangePassword` at the end.
  - `StartMenu` gains these fields: `bool accountsEnabled = false;` (web build only), `bool loggedIn = false;`, `bool accountBusy = false;` (a Register, Login, or Password call is in flight), `char loginName[kMaxNameLength + 1] = {};`, `char loginPassword[kMaxPasswordBytes + 1] = {};`, `char currentPassword[kMaxPasswordBytes + 1] = {};`, `char newPassword[kMaxPasswordBytes + 1] = {};`, `std::string accountError;`, `std::string accountNote;` (for example "Password changed"), and `std::optional<CareerView> record;`.

- [ ] **Step 1: Write the failing UI tests** in `tests/ui_test.cpp`

Model them on `"ui: the start screen draws the Name field and keeps the saved name"`.

```cpp
TEST_CASE("ui: logged out, the start screen draws the account fields and the guest name") {
    // duelsEnabled and accountsEnabled are true; loggedIn is false; loginName "Sam", loginPassword "short".
    // Two frames -> UiCommand::None, playerName unchanged, loginPassword still "short".
}

TEST_CASE("ui: logged in, the start screen draws the username, Log out and the record") {
    // loggedIn is true, playerName "Sam", record = CareerView{3, 1, 0, 9, 4, 5, 2, 2.6, 1.5, 45.0, 98.0},
    // and accountError "wrong password". Two frames -> None; playerName is still "Sam".
}

TEST_CASE("ui: accounts disabled draws no account block") {
    // duelsEnabled true, accountsEnabled false -> the same as today (the existing test's assertions hold)
}
```

- [ ] **Step 2: Run them and confirm they fail**

Run: `cmake --build build`
Expected: a compile failure, `no member named 'accountsEnabled'`.

- [ ] **Step 3: Implement the block in `drawStartScreen`**

Inside `if (menu.duelsEnabled)`, right after `SeparatorText("1v1 DUEL")`, add `if (menu.accountsEnabled)`.

When **logged out**, draw:
- `##user` (hint `USERNAME`, `nameCharFilter`, full width).
- `##pass` (hint `PASSWORD`, `ImGuiInputTextFlags_Password`, full width).
- **Register** and **Log in** side by side, at half width each. They are disabled while `accountBusy`, while `!isValidName(loginName)`, or while the password length is outside `kMinPasswordBytes` to `kMaxPasswordBytes`.
- Then the existing guest Name field, unchanged.

When **logged in**, the Name field is replaced by `ImGui::Text("%s", menu.playerName.c_str())`. Below it, draw:
- **Log out**.
- `##cur` and `##new` password fields (hints `CURRENT PASSWORD` and `NEW PASSWORD`).
- **Change password**, disabled while busy, while `currentPassword` is empty, or while `newPassword` is out of range.
- `SeparatorText("RECORD")`, followed by `ImGui::TextUnformatted` for each of `recordLines(*menu.record)` when `menu.record` is set.

Show `accountError` in the existing error red, and `accountNote` with `TextDisabled`, under the account buttons. The guest Name field and its `SaveName` logic are untouched when accounts are disabled or logged out.

- [ ] **Step 4: Run the UI suite**

Run: `cmake --build build && ./build/tetris_ui_tests && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/app/render.hpp src/app/render.cpp tests/ui_test.cpp
git commit -m "feat(app): start menu register, log in, log out, password and record"
```

---

### Task 12: App: account settings, bests and replies

**Files:**
- Modify: `src/app/app.hpp`, `src/app/app.cpp`, `src/app/main.cpp` (call `pumpAccount(host.app)` right after `pumpNetwork(host.app)`)

**Interfaces:**
- Consumes: `sendAccountRequest`, `takeAccountReplies`, and `accountsSupported` (Task 10); `credentialsBody`, `passwordBody`, `bestBody`, `parseProfile`, `parseBests`, and `accountErrorText` (Task 9); the `StartMenu` fields and `UiCommand`s (Task 11).
- Produces:
  - Methods on `App`: `void applySettings(const Settings& settings);` (keymap, handling, input and duel SDF, the menu's audio, name, and bests fields, `nameEdit`, mute and music, volumes), `Settings currentSettings() const;`, `void enterAccount(const Profile& profile);`, and `void leaveAccount();` (back to the guest `localStorage` text).
  - The free function `void pumpAccount(App& app);`.

- [ ] **Step 1: Refactor `App::App` to use `applySettings`**

Extract the block that copies `saved` into members into `applySettings`. Put the empty-name generate-and-save into a private helper, `ensureGuestName()`, which `leaveAccount` reuses. Then set `menu.accountsEnabled = accountsSupported()`, and send `AccountCall::Me` when it is true. `saveSettings()` serializes `currentSettings()`. When `menu.loggedIn`, it calls `sendAccountRequest(AccountCall::Settings, text)`. Otherwise it calls `saveSettingsText(text)`.

- [ ] **Step 2: Implement the account flow in `src/app/app.cpp`**

- **`enterAccount`:** `Settings s = parseSettings(profile.settings); s.playerName = profile.username; s.bests = profile.bests; applySettings(s);`. It also sets `loggedIn = true` and `record = profile.career`, and clears the three password buffers, `accountError`, `accountNote`, and `accountBusy`.
- **`leaveAccount`:** sets `loggedIn = false` and resets `record`. It then calls `applySettings(parseSettings(loadSettingsText()))` and `ensureGuestName()`.
- **`applyCommand`:**
  - `Register` and `Login` set `accountBusy` and send `credentialsBody(loginName, loginPassword)`.
  - `Logout` sends `AccountCall::Logout` and calls `leaveAccount()`.
  - `ChangePassword` sets `accountBusy` and sends `passwordBody`.
- **`pumpAccount`:** for each reply:
  - `Me` with 200 and a profile that parses calls `enterAccount`. Any other `Me` reply stays a guest, silently.
  - `Register` and `Login` clear `accountBusy`. A 200 that parses calls `enterAccount`. Anything else sets `accountError = accountErrorText(status, body)`.
  - `Password` clears busy. A 204 sets `accountNote = "Password changed"` and clears both buffers. Anything else sets `accountError`.
  - `Best` with 200 while `loggedIn` sets `menu.bests = *parseBests(body)`, when that parses.
  - `Logout` and `Settings` replies are ignored, so a failed save waits for the next one.
- **`playSoloEvents`:** after `submit` returns true and `saveSettings()`, if `menu.loggedIn` and `bestBody(mode, *result)` is set, send `AccountCall::Best`.

- [ ] **Step 3: Build and run all C++ tests, then build the web version**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`, then run the **Web build** command.
Expected: PASS, and both builds succeed.

- [ ] **Step 4: Run the whole thing in a browser**

Run: `scripts/build.sh && build-server/tetris-server -dir build-web -db /tmp/jestris-try.db`, then open `http://localhost:8080` and check each of these:
- A guest can play Sprint.
- Registering `Sam` resets to default keys and empty bests, and shows the username in place of the Name field.
- A finished Sprint shows its PB.
- Reloading the page brings back the same keys, PB, and name, and the Record block appears.
- Changing the password and logging out brings the guest's keys and PB back.
- Logging in as `sam` restores the account.
- Two tabs logged in as the same account: the older tab's room connection drops.

- [ ] **Step 5: Commit**

```bash
git add src/app/app.hpp src/app/app.cpp src/app/main.cpp
git commit -m "feat(app): logged-in players load and save settings and bests through the account API"
```
