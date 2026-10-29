package main

import (
	"context"
	"net/http"
	"net/http/httptest"
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
