package main

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
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

// pair puts two fresh clients in room AAAA and consumes the first START.
func pair(t *testing.T, srv *httptest.Server) (a, b *client) {
	t.Helper()
	a, b = dial(t, srv), dial(t, srv)
	a.send("CREATE 3")
	a.expect("ROOM AAAA")
	b.send("JOIN AAAA")
	a.expect("OPP Player")
	b.expect("OPP Player")
	a.expect("START 101 3")
	b.expect("START 101 3")
	return a, b
}

func TestCreateAndJoinStartBothPlayers(t *testing.T) {
	pair(t, newTestServer(t))
}

func TestGameMessagesAreRelayed(t *testing.T) {
	a, b := pair(t, newTestServer(t))
	a.send("ATK 2")
	b.expect("ATK 2")
	b.send("SNAP 0 0 0 board")
	a.expect("SNAP 0 0 0 board")
}

func TestServerOnlyVerbsAreNotRelayed(t *testing.T) {
	a, b := pair(t, newTestServer(t))
	for _, forged := range []string{"ROOM ZZZZ", "START 1 1", "LEFT", "ERR boom", "KO 3 0", "OVER WIN KO 0 0", "OPP Zed"} {
		a.send(forged)
	}
	a.send("ATK 1")
	b.expect("ATK 1")
}

func TestJoinErrors(t *testing.T) {
	srv := newTestServer(t)
	c := dial(t, srv)
	c.send("JOIN ZZZZ")
	c.expect("ERR unknown room")
	pair(t, srv)
	d := dial(t, srv)
	d.send("JOIN AAAA")
	d.expect("ERR room is full")
}

func TestBadStartLevelIsRejected(t *testing.T) {
	c := dial(t, newTestServer(t))
	c.send("CREATE 99")
	c.expect("ERR bad start level")
}

func TestMessagesOutsideARoomAreDropped(t *testing.T) {
	c := dial(t, newTestServer(t))
	c.send("ATK 2")
	c.send("REMATCH")
	c.send("CREATE 1")
	c.expect("ROOM AAAA") // nothing was sent back for the first two
}

func TestDisconnectTellsOpponent(t *testing.T) {
	a, b := pair(t, newTestServer(t))
	a.conn.Close(websocket.StatusNormalClosure, "")
	b.expect("LEFT")
}

func TestRematchNeedsBothPlayers(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	clock.timer(0).fire()
	a.expect("OVER DRAW TIME 0 0")
	b.expect("OVER DRAW TIME 0 0")
	a.send("REMATCH")
	b.send("SNAP probe") // proves nothing was sent to a after a's REMATCH alone
	a.expect("SNAP probe")
	b.send("REMATCH")
	a.expect("START 102 3")
	b.expect("START 102 3")
}

func TestWebFilesServesOnlyTheAllowlist(t *testing.T) {
	dir := t.TempDir()
	for _, name := range []string{"index.html", "index.js", "index.wasm", "CMakeCache.txt"} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Mkdir(filepath.Join(dir, "_deps"), 0o755); err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(webFiles(dir))
	t.Cleanup(srv.Close)

	for _, path := range []string{"/", "/index.html", "/index.js", "/index.wasm"} {
		resp, err := http.Get(srv.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Errorf("GET %s: status %d, want 200", path, resp.StatusCode)
		}
		if path == "/index.wasm" && resp.Header.Get("Content-Type") != "application/wasm" {
			t.Errorf("GET %s: Content-Type %q, want application/wasm", path, resp.Header.Get("Content-Type"))
		}
	}
	for _, path := range []string{"/CMakeCache.txt", "/_deps/", "/_deps"} {
		resp, err := http.Get(srv.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusNotFound {
			t.Errorf("GET %s: status %d, want 404", path, resp.StatusCode)
		}
	}
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
	a.expect("OVER DRAW TIME 0 0")
	b.expect("OVER DRAW TIME 0 0")
	a.send("ATK 2")
	a.send("DEAD")
	clock.timer(0).fire() // firing again changes nothing
	a.send("SNAP probe")
	b.expect("SNAP probe")
}

func TestRematchBeforeOverIsIgnored(t *testing.T) {
	a, b := pair(t, newTestServer(t))
	a.send("REMATCH")
	a.send("SNAP from a")
	b.expect("SNAP from a") // a's REMATCH has been handled
	b.send("REMATCH")
	b.send("SNAP from b")
	a.expect("SNAP from b") // a START would have reached a before this
}

func TestStaleTimerAfterRematchIsIgnored(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	clock.timer(0).fire()
	a.expect("OVER DRAW TIME 0 0")
	b.expect("OVER DRAW TIME 0 0")
	a.send("REMATCH")
	b.send("REMATCH")
	a.expect("START 102 3")
	b.expect("START 102 3")
	clock.timer(0).fire() // the first match's timer, racing the rematch
	a.send("SNAP probe")
	b.expect("SNAP probe")
	clock.timer(1).fire()
	a.expect("OVER DRAW TIME 0 0")
	b.expect("OVER DRAW TIME 0 0")
}

func TestLeavingStopsTheMatchTimer(t *testing.T) {
	srv, clock := newTestHub(t)
	a, b := pair(t, srv)
	a.conn.Close(websocket.StatusNormalClosure, "")
	b.expect("LEFT")
	if !clock.timer(0).isStopped() {
		t.Error("match timer still running after a player left")
	}
	clock.timer(0).fire() // the room is gone; this must do nothing
}

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
