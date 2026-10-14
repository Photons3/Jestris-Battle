package main

import (
	"context"
	"fmt"
	"net/http/httptest"
	"slices"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/coder/websocket"
)

// fakeClock records match timers so a test decides when they fire.
type fakeClock struct {
	mu     sync.Mutex
	timers []*fakeTimer
}

type fakeTimer struct {
	clock   *fakeClock
	d       time.Duration
	f       func()
	stopped bool
}

func (c *fakeClock) afterFunc(d time.Duration, f func()) stopper {
	c.mu.Lock()
	defer c.mu.Unlock()
	t := &fakeTimer{clock: c, d: d, f: f}
	c.timers = append(c.timers, t)
	return t
}

// timer returns the i-th timer armed so far.
func (c *fakeClock) timer(i int) *fakeTimer {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.timers[i]
}

func (t *fakeTimer) Stop() bool {
	t.clock.mu.Lock()
	defer t.clock.mu.Unlock()
	wasRunning := !t.stopped
	t.stopped = true
	return wasRunning
}

func (t *fakeTimer) isStopped() bool {
	t.clock.mu.Lock()
	defer t.clock.mu.Unlock()
	return t.stopped
}

// fire runs the callback even after Stop, like a real timer that fired just before it was stopped.
func (t *fakeTimer) fire() { t.f() }

const testMatchLength = 20 * time.Second

func newConfiguredHub(t *testing.T, configure func(*Hub)) (*httptest.Server, *fakeClock) {
	t.Helper()
	hub := NewHub()
	codes := 0
	hub.newCode = func() string { codes++; return fmt.Sprintf("AAA%c", 'A'+codes-1) }
	seed := uint32(100)
	hub.newSeed = func() uint32 { seed++; return seed }
	clock := &fakeClock{}
	hub.afterFunc = clock.afterFunc
	hub.matchLength = testMatchLength
	hub.pickTarget = func(int) int { return 0 }
	configure(hub)
	srv := httptest.NewServer(hub)
	t.Cleanup(srv.Close)
	return srv, clock
}

func newTestHub(t *testing.T) (*httptest.Server, *fakeClock) {
	t.Helper()
	return newConfiguredHub(t, func(*Hub) {})
}

func newTestServer(t *testing.T) *httptest.Server {
	t.Helper()
	srv, _ := newTestHub(t)
	return srv
}

type client struct {
	t    *testing.T
	conn *websocket.Conn
}

func dial(t *testing.T, srv *httptest.Server) *client {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, "ws"+strings.TrimPrefix(srv.URL, "http"), nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	t.Cleanup(func() { conn.CloseNow() })
	return &client{t: t, conn: conn}
}

func (c *client) send(text string) {
	c.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := c.conn.Write(ctx, websocket.MessageText, []byte(text)); err != nil {
		c.t.Fatalf("write %q: %v", text, err)
	}
}

func (c *client) expect(want string) {
	c.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, data, err := c.conn.Read(ctx)
	if err != nil {
		c.t.Fatalf("waiting for %q: %v", want, err)
	}
	if got := string(data); got != want {
		c.t.Fatalf("got %q, want %q", got, want)
	}
}

// next returns the next message c receives.
func (c *client) next() string {
	c.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, data, err := c.conn.Read(ctx)
	if err != nil {
		c.t.Fatalf("waiting for a message: %v", err)
	}
	return string(data)
}

// flush proves that everything c sent before has been handled and that nothing came back to c meanwhile:
// a JOIN from inside a room always gets this error, and each connection is read in order.
func (c *client) flush() {
	c.t.Helper()
	c.send("JOIN ZZZZ")
	c.expect("ERR already in a room")
}

// createRoom dials a client named name that creates a room with the CREATE arguments args.
func createRoom(t *testing.T, srv *httptest.Server, name, args string) *client {
	t.Helper()
	c := dial(t, srv)
	c.send("HELLO " + name)
	c.send("CREATE " + args)
	c.expect("ROOM AAAA 1")
	c.expect("PLAYERS 1 1:" + name + ":0")
	return c
}

// joinRoom dials a client named name that joins room code and gets its ROOM line with an ID.
func joinRoom(t *testing.T, srv *httptest.Server, code, name string) *client {
	t.Helper()
	c := dial(t, srv)
	c.send("HELLO " + name)
	c.send("JOIN " + code)
	got := c.next()
	id, err := strconv.Atoi(strings.TrimPrefix(got, "ROOM "+code+" "))
	if !strings.HasPrefix(got, "ROOM "+code+" ") || err != nil || id < 1 || id > maxRoomPlayers {
		t.Fatalf("got %q, want ROOM %s <id>", got, code)
	}
	return c
}

// pair puts Ann (host, ID 1) and Bob (ID 2) in room AAAA at level 3 and starts a duel, consuming START and PLAYERS.
func pair(t *testing.T, srv *httptest.Server) (a, b *client) {
	t.Helper()
	a = createRoom(t, srv, "Ann", "3")
	b = joinRoom(t, srv, "AAAA", "Bob")
	a.expect("PLAYERS 1 1:Ann:0 2:Bob:0")
	b.expect("PLAYERS 1 1:Ann:0 2:Bob:0")
	a.send("BEGIN")
	a.expect("START 101 3 1 2")
	b.expect("START 101 3 1 2")
	a.expect("PLAYERS 1 1:Ann:1 2:Bob:1")
	b.expect("PLAYERS 1 1:Ann:1 2:Bob:1")
	return a, b
}

// expectAll expects want on every client, in order.
func expectAll(want string, clients ...*client) {
	for _, c := range clients {
		c.t.Helper()
		c.expect(want)
	}
}

func TestCreateJoinAndBeginStartBothPlayers(t *testing.T) {
	pair(t, newTestServer(t))
}

func TestJoinSendsRoomAndRoster(t *testing.T) {
	srv := newTestServer(t)
	a := createRoom(t, srv, "Ann", "1")
	b := dial(t, srv)
	b.send("HELLO Bob")
	b.send("JOIN AAAA")
	b.expect("ROOM AAAA 2")
	expectAll("PLAYERS 1 1:Ann:0 2:Bob:0", a, b)
}

func TestGameMessagesAreRelayed(t *testing.T) {
	a, b := pair(t, newTestServer(t))
	a.send("ATK 2")
	b.expect("ATK 2")
	b.send("SNAP 0 0 0 board")
	a.expect("SNAP 2 0 0 0 board")
}

func TestServerOnlyVerbsAreNotRelayed(t *testing.T) {
	a, b := pair(t, newTestServer(t))
	for _, forged := range []string{"ROOM ZZZZ 1", "START 1 1 1 2", "LEFT", "ERR boom", "KO 3 0", "OVER WIN KO 0 0", "OPP Zed"} {
		a.send(forged)
	}
	a.send("ATK 1")
	b.expect("ATK 1")
}

func TestUnknownVerbsAreNotRelayed(t *testing.T) {
	a, b := pair(t, newTestServer(t))
	a.send("HELLO2 x")
	a.send("ATK 1")
	b.expect("ATK 1")
	a.flush()
}

func TestJoinErrors(t *testing.T) {
	c := dial(t, newTestServer(t))
	c.send("JOIN ZZZZ")
	c.expect("ERR unknown room")
}

func TestSixteenPlayersFitAndTheSeventeenthIsRefused(t *testing.T) {
	srv := newTestServer(t)
	createRoom(t, srv, "Ann", "1")
	for i := 2; i <= maxRoomPlayers; i++ {
		c := dial(t, srv)
		c.send("JOIN AAAA")
		c.expect(fmt.Sprintf("ROOM AAAA %d", i))
	}
	c := dial(t, srv)
	c.send("JOIN AAAA")
	c.expect("ERR room is full")
}

func TestDuplicateNamesGetSuffixes(t *testing.T) {
	srv := newTestServer(t)
	createRoom(t, srv, "Sam", "1")
	joinRoom(t, srv, "AAAA", "Sam")
	c := joinRoom(t, srv, "AAAA", "Sam")
	c.expect("PLAYERS 1 1:Sam:0 2:Sam2:0 3:Sam3:0")
	d := dial(t, srv)
	d.send("HELLO ABCDEFGHIJKL")
	d.send("CREATE 1")
	d.expect("ROOM AAAB 1")
	d.expect("PLAYERS 1 1:ABCDEFGHIJKL:0")
	e := joinRoom(t, srv, "AAAB", "ABCDEFGHIJKL")
	e.expect("PLAYERS 1 1:ABCDEFGHIJKL:0 2:ABCDEFGHIJK2:0")
}

func TestUniqueName(t *testing.T) {
	takenSet := func(names ...string) func(string) bool {
		return func(s string) bool { return slices.Contains(names, s) }
	}
	cases := []struct {
		base  string
		taken []string
		want  string
	}{
		{"Sam", nil, "Sam"},
		{"Sam", []string{"Sam"}, "Sam2"},
		{"Sam", []string{"Sam", "Sam2"}, "Sam3"},
		{"Sam", []string{"Sam", "Sam3"}, "Sam2"},
		{"ABCDEFGHIJKL", []string{"ABCDEFGHIJKL"}, "ABCDEFGHIJK2"},
		{"ABCDEFGHIJKL", []string{"ABCDEFGHIJKL", "ABCDEFGHIJK2", "ABCDEFGHIJK3", "ABCDEFGHIJK4", "ABCDEFGHIJK5",
			"ABCDEFGHIJK6", "ABCDEFGHIJK7", "ABCDEFGHIJK8", "ABCDEFGHIJK9"}, "ABCDEFGHIJ10"},
	}
	for _, tc := range cases {
		if got := uniqueName(tc.base, takenSet(tc.taken...)); got != tc.want {
			t.Errorf("uniqueName(%q, %v) = %q, want %q", tc.base, tc.taken, got, tc.want)
		}
	}
}

func TestOnlyTheHostCanBegin(t *testing.T) {
	srv, clock := newTestHub(t)
	a := createRoom(t, srv, "Ann", "1")
	a.send("BEGIN") // alone: nothing happens
	a.flush()
	b := joinRoom(t, srv, "AAAA", "Bob")
	expectAll("PLAYERS 1 1:Ann:0 2:Bob:0", a, b)
	b.send("BEGIN") // not the host: nothing happens
	b.flush()
	a.send("BEGIN now") // BEGIN takes no arguments
	a.flush()
	a.send("BEGIN")
	expectAll("START 101 1 1 2", a, b)
	expectAll("PLAYERS 1 1:Ann:1 2:Bob:1", a, b)
	clock.mu.Lock()
	defer clock.mu.Unlock()
	if len(clock.timers) != 1 {
		t.Errorf("%d timers armed, want 1", len(clock.timers))
	}
}

func TestBeginDuringAMatchIsIgnored(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	a.send("BEGIN")
	a.flush()
	a.send("ATK 1")
	b.expect("ATK 1") // a second START would have reached b first
	clock.mu.Lock()
	defer clock.mu.Unlock()
	if len(clock.timers) != 1 {
		t.Errorf("%d timers armed, want 1", len(clock.timers))
	}
}

func TestLateJoinerWaitsForTheNextMatch(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	c := joinRoom(t, srv, "AAAA", "Cat")
	expectAll("PLAYERS 1 1:Ann:1 2:Bob:1 3:Cat:0", a, b, c)
	b.send("SNAP 0 0 0 board")
	a.expect("SNAP 2 0 0 0 board")
	clock.timer(0).fire()
	expectAll("OVER DRAW TIME 0 0", a, b)
	expectAll("PLAYERS 1 1:Ann:0 2:Bob:0 3:Cat:0", a, b, c) // c got no START or SNAP before this
	a.send("BEGIN")
	expectAll("START 102 3 1 2 3", a, b, c)
	expectAll("PLAYERS 1 1:Ann:1 2:Bob:1 3:Cat:1", a, b, c)
}

func TestDisconnectTellsOpponent(t *testing.T) {
	a, b := pair(t, newTestServer(t))
	a.conn.Close(websocket.StatusNormalClosure, "")
	b.expect("LEFT")
}

func TestDuelLeftEndsTheMatchButKeepsTheRoom(t *testing.T) {
	srv := newTestServer(t)
	a, b := pair(t, srv)
	a.conn.Close(websocket.StatusNormalClosure, "")
	b.expect("LEFT")
	b.expect("PLAYERS 2 2:Bob:0")
	b.send("BEGIN") // alone: nothing happens
	b.flush()
	c := joinRoom(t, srv, "AAAA", "Cat")
	expectAll("PLAYERS 2 2:Bob:0 1:Cat:0", b, c) // Cat gets the lowest free ID
	b.send("BEGIN")
	expectAll("START 102 3 2 1", b, c)
}

func TestHostLeavingMovesTheHost(t *testing.T) {
	srv := newTestServer(t)
	a := createRoom(t, srv, "Ann", "1")
	b := joinRoom(t, srv, "AAAA", "Bob")
	expectAll("PLAYERS 1 1:Ann:0 2:Bob:0", a, b)
	c := joinRoom(t, srv, "AAAA", "Cat")
	expectAll("PLAYERS 1 1:Ann:0 2:Bob:0 3:Cat:0", a, b, c)
	a.conn.Close(websocket.StatusNormalClosure, "")
	expectAll("PLAYERS 2 2:Bob:0 3:Cat:0", b, c)
	d := joinRoom(t, srv, "AAAA", "Dan")
	expectAll("PLAYERS 2 2:Bob:0 3:Cat:0 1:Dan:0", b, c, d)
	c.send("BEGIN") // not the host
	c.flush()
	b.send("BEGIN")
	expectAll("START 101 1 2 3 1", b, c, d)
}

func TestLastPlayerLeavingDeletesTheRoom(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	a := createRoom(t, srv, "Ann", "2 PUB")
	w.expect("ROOMS AAAA:2:Ann:1")
	a.conn.Close(websocket.StatusNormalClosure, "")
	w.expect("ROOMS")
	c := dial(t, srv)
	c.send("JOIN AAAA")
	c.expect("ERR unknown room")
}

func TestAttacksAreCountedAndMalformedOnesDropped(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	for _, bad := range []string{"ATK", "ATK 0", "ATK 41", "ATK x", "ATK 2 2"} {
		a.send(bad)
	}
	a.send("ATK 2")
	b.expect("ATK 2")
	a.send("ATK 3")
	b.expect("ATK 3")
	b.send("ATK 1")
	a.expect("ATK 1")
	clock.timer(0).fire()
	a.expect("OVER WIN TIME 5 1")
	b.expect("OVER LOSE TIME 1 5")
	expectAll("PLAYERS 1 1:Ann:0 2:Bob:0", a, b)
}

func TestDeadIsAKOAndThreeKOsWin(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	for i := 1; i <= 3; i++ {
		b.send("DEAD")
		a.expect(fmt.Sprintf("KO %d 0", i))
		b.expect(fmt.Sprintf("KO 0 %d", i))
	}
	a.expect("OVER WIN KO 0 0")
	b.expect("OVER LOSE KO 0 0")
	expectAll("PLAYERS 1 1:Ann:0 2:Bob:0", a, b)
	if !clock.timer(0).isStopped() {
		t.Error("match timer still running after a KO win")
	}
}

func TestTimeUpGoesToMoreKOsBeforeLinesSent(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	a.send("ATK 5")
	b.expect("ATK 5")
	a.send("DEAD")
	a.expect("KO 0 1")
	b.expect("KO 1 0")
	clock.timer(0).fire()
	a.expect("OVER LOSE TIME 5 0")
	b.expect("OVER WIN TIME 0 5")
}

func TestTimeUpWithTiedKOsGoesToLinesSent(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	a.send("DEAD")
	a.expect("KO 0 1")
	b.expect("KO 1 0")
	b.send("DEAD")
	a.expect("KO 1 1")
	b.expect("KO 1 1")
	a.send("ATK 3")
	b.expect("ATK 3")
	b.send("ATK 1")
	a.expect("ATK 1")
	clock.timer(0).fire()
	a.expect("OVER WIN TIME 3 1")
	b.expect("OVER LOSE TIME 1 3")
}

func TestTimeUpWithEverythingTiedIsADraw(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	a.send("ATK 2")
	b.expect("ATK 2")
	b.send("ATK 2")
	a.expect("ATK 2")
	clock.timer(0).fire()
	a.expect("OVER DRAW TIME 2 2")
	b.expect("OVER DRAW TIME 2 2")
}

// Clients start their clocks when START arrives, a one-way latency after the server armed its timer.
// The grace window lets an ATK sent just before a client's 0:00 freeze still arrive in time to count.
func TestMatchTimerCoversCountdownMatchAndGrace(t *testing.T) {
	srv, clock := newTestHub(t)
	pair(t, srv)
	if got, want := clock.timer(0).d, countdown+testMatchLength+defaultMatchGrace; got != want {
		t.Errorf("timer armed for %v, want %v", got, want)
	}
}

func TestMessagesAfterOverAreIgnored(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	clock.timer(0).fire()
	expectAll("OVER DRAW TIME 0 0", a, b)
	expectAll("PLAYERS 1 1:Ann:0 2:Bob:0", a, b)
	a.send("ATK 2")
	a.send("DEAD")
	a.send("SNAP 0 0 0 board")
	clock.timer(0).fire() // firing again changes nothing
	a.send("BEGIN")
	expectAll("START 102 3 1 2", a, b) // nothing reached either player before this
}

func TestStaleTimerAfterBeginIsIgnored(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	clock.timer(0).fire()
	expectAll("OVER DRAW TIME 0 0", a, b)
	expectAll("PLAYERS 1 1:Ann:0 2:Bob:0", a, b)
	a.send("BEGIN")
	expectAll("START 102 3 1 2", a, b)
	expectAll("PLAYERS 1 1:Ann:1 2:Bob:1", a, b)
	clock.timer(0).fire() // the first match's timer, racing the new match
	a.send("SNAP probe")
	b.expect("SNAP 1 probe")
	clock.timer(1).fire()
	expectAll("OVER DRAW TIME 0 0", a, b)
}

// A duel that ends by LEFT stops its timer without a verdict, so a timer that fired just before the stop
// must not end the next match.
func TestStaleTimerAfterLeftAndBeginIsIgnored(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	c := joinRoom(t, srv, "AAAA", "Cat")
	expectAll("PLAYERS 1 1:Ann:1 2:Bob:1 3:Cat:0", a, b, c)
	b.conn.Close(websocket.StatusNormalClosure, "")
	a.expect("LEFT")
	expectAll("PLAYERS 1 1:Ann:0 3:Cat:0", a, c)
	a.send("BEGIN")
	expectAll("START 102 3 1 3", a, c)
	expectAll("PLAYERS 1 1:Ann:1 3:Cat:1", a, c)
	clock.timer(0).fire() // the first match's timer
	a.send("SNAP probe")
	c.expect("SNAP 1 probe") // an OVER would have reached c first
	a.flush()
	clock.timer(1).fire()
	expectAll("OVER DRAW TIME 0 0", a, c)
}

func TestLeavingStopsTheMatchTimer(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	a.conn.Close(websocket.StatusNormalClosure, "")
	b.expect("LEFT")
	if !clock.timer(0).isStopped() {
		t.Error("match timer still running after a player left")
	}
	clock.timer(0).fire() // the match is over; this must do nothing
	b.expect("PLAYERS 2 2:Bob:0")
	b.flush()
}

func TestBadHelloLeavesTheDefaultName(t *testing.T) {
	srv := newTestServer(t)
	a, b := dial(t, srv), dial(t, srv)
	a.send("HELLO two words")
	a.send("CREATE 3")
	a.expect("ROOM AAAA 1")
	a.expect("PLAYERS 1 1:Player:0")
	b.send("HELLO abcdefghijklm")
	b.send("JOIN AAAA")
	b.expect("ROOM AAAA 2")
	expectAll("PLAYERS 1 1:Player:0 2:Player2:0", a, b)
}

func TestHelloInsideARoomIsIgnored(t *testing.T) {
	srv := newTestServer(t)
	a, b := pair(t, srv)
	a.send("HELLO Zed") // dropped: a is in a room
	a.send("ATK 1")
	b.expect("ATK 1") // a's HELLO was handled before b leaves; each connection is read in order
	b.conn.Close(websocket.StatusNormalClosure, "")
	a.expect("LEFT")
	a.expect("PLAYERS 1 1:Ann:0")
	c := joinRoom(t, srv, "AAAA", "Cat")
	c.expect("PLAYERS 1 1:Ann:0 2:Cat:0")
}

// watch dials a lobby watcher and consumes its first (empty) list.
func watch(t *testing.T, srv *httptest.Server) *client {
	t.Helper()
	w := dial(t, srv)
	w.send("LIST")
	w.expect("ROOMS")
	return w
}

func TestPublicRoomsListPlayerCountUntilFull(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	createRoom(t, srv, "Ann", "1 PUB")
	w.expect("ROOMS AAAA:1:Ann:1")
	var last *client
	for n := 2; n <= maxRoomPlayers; n++ {
		last = joinRoom(t, srv, "AAAA", "Bob")
		if n < maxRoomPlayers {
			w.expect(fmt.Sprintf("ROOMS AAAA:1:Ann:%d", n))
		}
	}
	w.expect("ROOMS") // full rooms are not listed
	last.conn.Close(websocket.StatusNormalClosure, "")
	w.expect("ROOMS AAAA:1:Ann:15")
}

func TestPublicRoomsListTheCurrentHost(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	a := createRoom(t, srv, "Sam", "1 PUB")
	w.expect("ROOMS AAAA:1:Sam:1")
	joinRoom(t, srv, "AAAA", "Sam")
	w.expect("ROOMS AAAA:1:Sam:2")
	a.conn.Close(websocket.StatusNormalClosure, "")
	w.expect("ROOMS AAAA:1:Sam2:1")
}

func TestPrivateRoomsAreNotListed(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	a, b := dial(t, srv), dial(t, srv)
	a.send("CREATE 3")
	a.expect("ROOM AAAA 1")
	b.send("CREATE 5 PUB")
	b.expect("ROOM AAAB 1")
	w.expect("ROOMS AAAB:5:Player:1") // the private room sent nothing before this
}

func TestCreatingUnsubscribes(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	w.send("CREATE 1 PUB")
	w.expect("ROOM AAAA 1") // no ROOMS: w stopped watching
	w.expect("PLAYERS 1 1:Player:0")
	x := dial(t, srv)
	x.send("CREATE 2 PUB")
	x.expect("ROOM AAAB 1")
	y := dial(t, srv)
	y.send("JOIN AAAA")
	w.expect("PLAYERS 1 1:Player:0 2:Player2:0") // still nothing in between
}

func TestUnlistStopsUpdates(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	w.send("UNLIST")
	w.send("CREATE 99")
	w.expect("ERR bad start level") // UNLIST was handled; a bad level never reaches the watcher set
	a, b := dial(t, srv), dial(t, srv)
	a.send("CREATE 1 PUB")
	a.expect("ROOM AAAA 1")
	b.send("CREATE 1 PUB")
	b.expect("ROOM AAAB 1")
	w.send("LIST")
	w.expect("ROOMS AAAA:1:Player:1 AAAB:1:Player:1") // a push would have arrived first with one room
}

func TestListIsOldestFirstAndCapped(t *testing.T) {
	srv := newTestServer(t)
	for i := 0; i < maxListedRooms+1; i++ {
		c := dial(t, srv)
		c.send("CREATE 1 PUB")
		c.expect(fmt.Sprintf("ROOM AAA%c 1", 'A'+i))
	}
	w := dial(t, srv)
	w.send("LIST")
	var want strings.Builder
	want.WriteString("ROOMS")
	for i := 0; i < maxListedRooms; i++ {
		fmt.Fprintf(&want, " AAA%c:1:Player:1", 'A'+i)
	}
	w.expect(want.String())
}

func TestBadRoomOptionsAndFullServer(t *testing.T) {
	srv, _ := newConfiguredHub(t, func(h *Hub) { h.maxRooms = 1 })
	a, b := dial(t, srv), dial(t, srv)
	a.send("CREATE 3 OPEN")
	a.expect("ERR bad room options")
	a.send("CREATE 3")
	a.expect("ROOM AAAA 1")
	b.send("CREATE 3 PUB")
	b.expect("ERR server is full")
}

func TestLobbyVerbsInsideARoomAreNotRelayed(t *testing.T) {
	a, b := pair(t, newTestServer(t))
	a.send("LIST")
	a.send("UNLIST")
	a.send("ROOMS AAAA:1:Sam:1")
	a.send("ATK 1")
	b.expect("ATK 1")
}

func TestQuickOpensAPublicRoomWhenNoneIsOpen(t *testing.T) {
	srv := newTestServer(t)
	w := watch(t, srv)
	a := dial(t, srv)
	a.send("QUICK 4")
	a.expect("ROOM AAAA 1")
	w.expect("ROOMS AAAA:4:Player:1")
}

func TestQuickJoinsTheOldestOpenRoomAtItsLevel(t *testing.T) {
	srv := newTestServer(t)
	a := createRoom(t, srv, "Ann", "3 PUB")
	b := dial(t, srv)
	b.send("CREATE 5 PUB")
	b.expect("ROOM AAAB 1")
	c := dial(t, srv)
	c.send("HELLO Cat")
	c.send("QUICK 9")
	c.expect("ROOM AAAA 2")
	expectAll("PLAYERS 1 1:Ann:0 2:Cat:0", a, c)
	a.send("BEGIN")
	expectAll("START 101 3 1 2", a, c)
}

func TestQuickPrefersARoomWithNoMatch(t *testing.T) {
	srv := newTestServer(t)
	a := createRoom(t, srv, "Ann", "3 PUB")
	b := joinRoom(t, srv, "AAAA", "Bob")
	expectAll("PLAYERS 1 1:Ann:0 2:Bob:0", a, b)
	a.send("BEGIN")
	expectAll("START 101 3 1 2", a, b)
	c := dial(t, srv)
	c.send("HELLO Cat")
	c.send("CREATE 5 PUB")
	c.expect("ROOM AAAB 1")
	c.expect("PLAYERS 1 1:Cat:0")
	d := dial(t, srv)
	d.send("HELLO Dan")
	d.send("QUICK 9")
	d.expect("ROOM AAAB 2")
	expectAll("PLAYERS 1 1:Cat:0 2:Dan:0", c, d)
}

func TestQuickIgnoresPrivateRooms(t *testing.T) {
	srv := newTestServer(t)
	a, b := dial(t, srv), dial(t, srv)
	a.send("CREATE 3")
	a.expect("ROOM AAAA 1")
	b.send("QUICK 2")
	b.expect("ROOM AAAB 1")
}

func TestTwoQuicksPairUp(t *testing.T) {
	srv := newTestServer(t)
	a, b := dial(t, srv), dial(t, srv)
	a.send("QUICK 2")
	a.expect("ROOM AAAA 1")
	a.expect("PLAYERS 1 1:Player:0")
	b.send("QUICK 7")
	b.expect("ROOM AAAA 2")
	expectAll("PLAYERS 1 1:Player:0 2:Player2:0", a, b)
	a.send("BEGIN")
	expectAll("START 101 2 1 2", a, b)
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

func TestMessagesOutsideARoomAreDropped(t *testing.T) {
	c := dial(t, newTestServer(t))
	c.send("ATK 2")
	c.send("REMATCH")
	c.send("BEGIN")
	c.send("CREATE 1")
	c.expect("ROOM AAAA 1") // nothing was sent back for the first three
}

func TestBadStartLevelIsRejected(t *testing.T) {
	c := dial(t, newTestServer(t))
	c.send("CREATE 99")
	c.expect("ERR bad start level")
}

// royale puts the named players in room AAAA at level 1, in order with IDs 1, 2, ..., and has the host
// begin, consuming START and PLAYERS.
func royale(t *testing.T, srv *httptest.Server, names ...string) []*client {
	t.Helper()
	clients := []*client{createRoom(t, srv, names[0], "1")}
	roster := "PLAYERS 1 1:" + names[0] + ":0"
	for i, name := range names[1:] {
		clients = append(clients, joinRoom(t, srv, "AAAA", name))
		roster += fmt.Sprintf(" %d:%s:0", i+2, name)
		expectAll(roster, clients...)
	}
	clients[0].send("BEGIN")
	start := "START 101 1"
	for i := range names {
		start += fmt.Sprintf(" %d", i+1)
	}
	expectAll(start, clients...)
	expectAll(strings.ReplaceAll(roster, ":0", ":1"), clients...)
	return clients
}

func TestThreePlayersStartARoyaleWithoutTimer(t *testing.T) {
	srv, clock := newTestHub(t)
	royale(t, srv, "Ann", "Bob", "Cat") // START 101 1 1 2 3 reaches all three
	clock.mu.Lock()
	defer clock.mu.Unlock()
	if len(clock.timers) != 0 {
		t.Errorf("%d timers armed, want 0", len(clock.timers))
	}
}

func TestRoyaleAttackGoesToAPickedLivingOpponent(t *testing.T) {
	var mu sync.Mutex
	var seen []int
	srv, _ := newConfiguredHub(t, func(h *Hub) {
		h.pickTarget = func(n int) int {
			mu.Lock()
			defer mu.Unlock()
			seen = append(seen, n)
			return n - 1 // 1 while Bob and Cat are alive
		}
	})
	c := royale(t, srv, "Ann", "Bob", "Cat")
	a, b, cat := c[0], c[1], c[2]
	a.send("ATK 2")
	cat.expect("ATK 2")
	b.flush() // the ATK went to Cat alone
	b.send("DEAD")
	expectAll("OUT 2 3", a, b, cat)
	a.send("ATK 1")
	cat.expect("ATK 1")
	mu.Lock()
	defer mu.Unlock()
	if !slices.Equal(seen, []int{2, 1}) {
		t.Errorf("pickTarget saw %v, want [2 1]", seen)
	}
}

func TestRoyaleSnapReachesEveryOtherParticipant(t *testing.T) {
	c := royale(t, newTestServer(t), "Ann", "Bob", "Cat")
	c[1].send("SNAP 0 0 0 board")
	expectAll("SNAP 2 0 0 0 board", c[0], c[2])
	c[1].flush()
}

func TestRoyaleDeadGivesPlacesAndTheLastWins(t *testing.T) {
	c := royale(t, newTestServer(t), "Ann", "Bob", "Cat", "Dan")
	c[1].send("DEAD")
	expectAll("OUT 2 4", c...)
	c[3].send("DEAD")
	expectAll("OUT 4 3", c...)
	c[0].send("DEAD")
	expectAll("OUT 1 2", c...)
	expectAll("OUT 3 1", c...)
	expectAll("PLAYERS 1 1:Ann:0 2:Bob:0 3:Cat:0 4:Dan:0", c...)
}

func TestEliminatedPlayersCannotAttackOrDieAgain(t *testing.T) {
	c := royale(t, newTestServer(t), "Ann", "Bob", "Cat")
	c[1].send("DEAD")
	expectAll("OUT 2 3", c...)
	c[1].send("ATK 3")
	c[1].send("DEAD")
	c[1].flush()
	c[0].send("SNAP probe")
	c[2].expect("SNAP 1 probe") // an ATK or OUT would have reached Cat first
	c[1].expect("SNAP 1 probe")
}

// DEAD racing the end of a royale: the second DEAD arrives after OUT ... 1.
func TestDeadAfterRoyaleEndIsDropped(t *testing.T) {
	c := royale(t, newTestServer(t), "Ann", "Bob", "Cat")
	c[1].send("DEAD")
	expectAll("OUT 2 3", c...)
	c[0].send("DEAD")
	expectAll("OUT 1 2", c...)
	expectAll("OUT 3 1", c...)
	expectAll("PLAYERS 1 1:Ann:0 2:Bob:0 3:Cat:0", c...)
	c[2].send("DEAD")
	c[2].flush()
	c[0].send("BEGIN")
	expectAll("START 102 1 1 2 3", c...) // nothing reached anyone before this
}

func TestLeavesEndRoyaleAndMoveHost(t *testing.T) {
	c := royale(t, newTestServer(t), "Ann", "Bob", "Cat")
	c[0].conn.Close(websocket.StatusNormalClosure, "")
	expectAll("OUT 1 3", c[1], c[2])
	expectAll("PLAYERS 2 2:Bob:1 3:Cat:1", c[1], c[2])
	c[1].conn.Close(websocket.StatusNormalClosure, "")
	c[2].expect("OUT 2 2")
	c[2].expect("OUT 3 1")
	c[2].expect("PLAYERS 3 3:Cat:0")
}

func TestLateJoinerGetsNoMatchTraffic(t *testing.T) {
	srv, _ := newConfiguredHub(t, func(h *Hub) { h.pickTarget = func(n int) int { return n - 1 } })
	c := royale(t, srv, "Ann", "Bob", "Cat")
	d := joinRoom(t, srv, "AAAA", "Dan") // checks ROOM AAAA 4
	expectAll("PLAYERS 1 1:Ann:1 2:Bob:1 3:Cat:1 4:Dan:0", append(c, d)...)
	c[0].send("SNAP 0 0 0 board")
	expectAll("SNAP 1 0 0 0 board", c[1], c[2])
	c[0].send("ATK 1")
	c[2].expect("ATK 1") // the last living opponent is Cat, not Dan
	c[1].send("DEAD")
	expectAll("OUT 2 3", c...)
	d.flush() // Dan got nothing since PLAYERS
}

func TestServerOnlyVerbsFromClientsAreDropped(t *testing.T) {
	c := royale(t, newTestServer(t), "Ann", "Bob", "Cat")
	for _, forged := range []string{"PLAYERS 1 1:x:1", "OUT 1 1", "ROOM AAAA 1"} {
		c[0].send(forged)
	}
	c[0].send("SNAP probe")
	expectAll("SNAP 1 probe", c[1], c[2])
	c[0].flush()
}
