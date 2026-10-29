package main

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

// fakeSessions maps a cookie token straight to an account.
type fakeSessions map[string]Account

func (f fakeSessions) Session(_ context.Context, token string) (Account, error) {
	if a, ok := f[token]; ok {
		return a, nil
	}
	return Account{}, errNoSession
}

// dialAs is dial with the session cookie token.
func dialAs(t *testing.T, srv *httptest.Server, token string) *client {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	opts := &websocket.DialOptions{HTTPHeader: http.Header{"Cookie": {sessionCookie + "=" + token}}}
	conn, _, err := websocket.Dial(ctx, "ws"+strings.TrimPrefix(srv.URL, "http"), opts)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	t.Cleanup(func() { conn.CloseNow() })
	return &client{t: t, conn: conn}
}

// expectClosed reads until the connection fails.
func (c *client) expectClosed() {
	c.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	for {
		if _, _, err := c.conn.Read(ctx); err != nil {
			if ctx.Err() != nil {
				c.t.Fatalf("connection still open after 5s")
			}
			return
		}
	}
}

func newAccountHub(t *testing.T) (*httptest.Server, *Hub) {
	t.Helper()
	var hub *Hub
	srv, _ := newConfiguredHub(t, func(h *Hub) {
		h.sessions = fakeSessions{"tokS": {ID: 1, Username: "Sam"}}
		hub = h
	})
	return srv, hub
}

func TestAuthenticatedSocketUsesTheAccountNameAndIgnoresHello(t *testing.T) {
	srv, _ := newAccountHub(t)
	c := dialAs(t, srv, "tokS")
	c.send("HELLO Bob")
	c.send("CREATE 3")
	c.expect("ROOM AAAA 1")
	c.expect("PLAYERS 1 1:Sam:0")
}

func TestBadSessionCookieIsAGuest(t *testing.T) {
	srv, _ := newAccountHub(t)
	c := dialAs(t, srv, "nope")
	c.send("HELLO Bob")
	c.send("CREATE 3")
	c.expect("ROOM AAAA 1")
	c.expect("PLAYERS 1 1:Bob:0")
}

func TestSecondSocketForAnAccountClosesTheFirst(t *testing.T) {
	srv, _ := newAccountHub(t)
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
	srv, _ := newAccountHub(t)
	a, b := dial(t, srv), dial(t, srv)
	a.send("HELLO Sam")
	b.send("HELLO Sam")
	a.send("CREATE 3")
	a.expect("ROOM AAAA 1")
	a.expect("PLAYERS 1 1:Sam:0")
	b.send("CREATE 3")
	b.expect("ROOM AAAB 1")
	b.expect("PLAYERS 1 1:Sam:0")
	a.flush()
}

// directAccountHub is a hub driven without a simulation or a live match timer.
func directAccountHub(career careerRecorder) *Hub {
	h := NewHub()
	h.career = career
	h.newCode = func() string { return "AAAA" }
	h.newSeed = func() uint32 { return 101 }
	h.newSim = func() (gameSim, error) { return nil, errors.New("no engine") }
	h.afterFunc = (&fakeClock{}).afterFunc
	return h
}

func messageWithPrefix(msgs []string, prefix string) string {
	for _, m := range msgs {
		if strings.HasPrefix(m, prefix) {
			return m
		}
	}
	return ""
}

// Kick account 1 while it shares a room with a guest. The new socket is the only seat BEGIN
// gives that account, and the finished match records one delta. The kicked socket's later leave is a no-op.
func TestKickedAccountHasOneSeatAndOneCareerDelta(t *testing.T) {
	fc := newFakeCareer(t)
	h := directAccountHub(fc)
	sam := Account{ID: 1, Username: "Sam"}
	old := testPlayer("temp")
	h.attach(old, sam)
	bob := testPlayer("Bob")
	h.receive(old, "CREATE 1")
	h.receive(bob, "JOIN AAAA")
	queued(old)
	queued(bob)

	neu := testPlayer("temp")
	h.attach(neu, sam)
	if !old.kicked || old.room != nil {
		t.Fatalf("kicked=%v room=%v, want the old socket out of the room", old.kicked, old.room != nil)
	}
	if h.accounts[1] != neu {
		t.Fatal("account 1 is not the new socket")
	}
	h.receive(neu, "JOIN AAAA")
	if neu.room == nil || !slices.Contains(neu.room.players, neu) || slices.Contains(neu.room.players, old) {
		t.Fatalf("room players after the new socket joined: %+v", neu.room)
	}

	h.receive(bob, "BEGIN")
	m := h.rooms["AAAA"].match
	if m == nil {
		t.Fatal("BEGIN did not start")
	}
	seats := 0
	for _, q := range m.players {
		if q == old {
			t.Fatal("BEGIN seated the kicked socket")
		}
		if q.accountID == 1 {
			seats++
		}
	}
	if seats != 1 || !slices.Contains(m.players, neu) {
		t.Fatalf("match seats %+v, want one seat and it is the new socket", m.players)
	}
	start := messageWithPrefix(queued(bob), "START ")
	fields := strings.Fields(start)
	if len(fields) != 5 { // START seed level id id
		t.Fatalf("START %q, want two seats", start)
	}

	for range 3 {
		h.receive(bob, "DEAD")
	}
	fc.expect(t, map[int64]Career{1: {DuelWins: 1, KosDealt: 3}})

	queued(bob)
	h.leave(old)
	if h.accounts[1] != neu || neu.room == nil || h.rooms["AAAA"] == nil {
		t.Fatal("the kicked socket's leave touched the room")
	}
	if got := queued(bob); len(got) != 0 {
		t.Fatalf("late leave sent %q", got)
	}
}

func TestBeginSkipsAKickedPlayer(t *testing.T) {
	h := directAccountHub(nil)
	a, b, c := testPlayer("Ann"), testPlayer("Bob"), testPlayer("Cat")
	h.receive(a, "CREATE 1")
	h.receive(b, "JOIN AAAA")
	h.receive(c, "JOIN AAAA")
	c.disconnect() // still listed until its read loop leaves
	h.receive(a, "BEGIN")
	m := h.rooms["AAAA"].match
	if m == nil || len(m.players) != 2 || slices.Contains(m.players, c) {
		t.Fatalf("match players %v, want Ann and Bob only", m)
	}
}

func TestKickDuringADuelSendsLeftAndALaterLeaveIsQuiet(t *testing.T) {
	h := directAccountHub(nil)
	sam := Account{ID: 1, Username: "Sam"}
	old := testPlayer("temp")
	h.attach(old, sam)
	bob := testPlayer("Bob")
	h.receive(old, "CREATE 1")
	h.receive(bob, "JOIN AAAA")
	h.receive(old, "BEGIN")
	queued(old)
	queued(bob)

	neu := testPlayer("temp")
	h.attach(neu, sam)
	if h.rooms["AAAA"].match != nil {
		t.Fatal("the duel was still running")
	}
	if bob.room == nil || h.rooms["AAAA"].hostID != bob.id {
		t.Fatalf("host %d, Bob id %d", h.rooms["AAAA"].hostID, bob.id)
	}
	got := queued(bob)
	if !slices.Contains(got, "LEFT") {
		t.Fatalf("Bob got %q, want LEFT", got)
	}
	h.leave(old)
	if got := queued(bob); len(got) != 0 {
		t.Fatalf("late leave sent %q", got)
	}
	if h.accounts[1] != neu {
		t.Fatal("late leave unregistered the new socket")
	}
}

func TestKickDuringARoyaleEliminatesAndMovesHost(t *testing.T) {
	h := directAccountHub(nil)
	sam := Account{ID: 1, Username: "Sam"}
	old := testPlayer("temp")
	h.attach(old, sam)
	bob, cat := testPlayer("Bob"), testPlayer("Cat")
	h.receive(old, "CREATE 1")
	h.receive(bob, "JOIN AAAA")
	h.receive(cat, "JOIN AAAA")
	h.receive(old, "BEGIN")
	queued(old)
	queued(bob)
	queued(cat)

	h.attach(testPlayer("temp"), sam)
	if h.rooms["AAAA"].hostID != bob.id {
		t.Fatalf("host %d, want Bob %d", h.rooms["AAAA"].hostID, bob.id)
	}
	if h.rooms["AAAA"].match == nil || h.rooms["AAAA"].match.alive[old] {
		t.Fatal("kicked player is still alive in the royale")
	}
	for _, p := range []*player{bob, cat} {
		got := queued(p)
		if !slices.Contains(got, "OUT 1 3") {
			t.Fatalf("%s got %q, want OUT 1 3", p.name, got)
		}
	}
}

func TestKickOfTheOnlyOccupantDeletesTheRoom(t *testing.T) {
	h := directAccountHub(nil)
	old := testPlayer("temp")
	h.attach(old, Account{ID: 1, Username: "Sam"})
	h.receive(old, "CREATE 1")
	h.attach(testPlayer("temp"), Account{ID: 1, Username: "Sam"})
	if h.rooms["AAAA"] != nil || old.room != nil {
		t.Fatal("the empty room is still there")
	}
	h.leave(old) // the read loop's leave must not panic or recreate it
	if h.rooms["AAAA"] != nil {
		t.Fatal("late leave recreated the room")
	}
}

func TestAccountKeepsItsNameAndRenamesTheGuest(t *testing.T) {
	h := directAccountHub(nil)
	guest := testPlayer("Sam")
	h.receive(guest, "CREATE 1")
	queued(guest)
	acc := testPlayer("nope")
	h.attach(acc, Account{ID: 1, Username: "Sam"})
	h.receive(acc, "JOIN AAAA")
	if acc.roomName != "Sam" || guest.roomName != "Sam2" {
		t.Fatalf("names account %q guest %q, want Sam and Sam2", acc.roomName, guest.roomName)
	}
	want := "PLAYERS 1 1:Sam2:0 2:Sam:0"
	if got := queued(guest); !slices.Contains(got, want) {
		t.Fatalf("guest got %q, want a roster including %q", got, want)
	}
	if got := queued(acc); !slices.Contains(got, want) {
		t.Fatalf("account got %q, want %q", got, want)
	}
}

func TestGuestJoiningAnAccountGetsASuffix(t *testing.T) {
	h := directAccountHub(nil)
	acc := testPlayer("nope")
	h.attach(acc, Account{ID: 1, Username: "Sam"})
	h.receive(acc, "CREATE 1")
	guest := testPlayer("Sam")
	h.receive(guest, "JOIN AAAA")
	if acc.roomName != "Sam" || guest.roomName != "Sam2" {
		t.Fatalf("names account %q guest %q, want Sam and Sam2", acc.roomName, guest.roomName)
	}
}

func TestTwoAccountsWithTheSameNameSuffixTheJoiner(t *testing.T) {
	h := directAccountHub(nil)
	first := testPlayer("nope")
	h.attach(first, Account{ID: 1, Username: "Sam"})
	h.receive(first, "CREATE 1")
	second := testPlayer("nope")
	h.attach(second, Account{ID: 2, Username: "Sam"})
	h.receive(second, "JOIN AAAA")
	if first.roomName != "Sam" || second.roomName != "Sam2" {
		t.Fatalf("names %q %q, want Sam and Sam2", first.roomName, second.roomName)
	}
}

func TestDisconnectAccountClosesThatSocket(t *testing.T) {
	srv, h := newAccountHub(t)
	c := dialAs(t, srv, "tokS")
	c.send("LIST")
	c.expect("ROOMS")
	h.DisconnectAccount(2) // nobody is connected as account 2
	c.send("LIST")
	c.expect("ROOMS")
	h.DisconnectAccount(1)
	c.expectClosed()
}
