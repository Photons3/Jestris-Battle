package main

import (
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/coder/websocket"
)

func TestMuxServesTheAPIBesideThePage(t *testing.T) {
	hub := NewHub()
	page := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { io.WriteString(w, "page") })
	srv := httptest.NewServer(newMux(page, hub, newAccountAPI(openTestStore(t), hub)))
	t.Cleanup(srv.Close)

	if status, _ := call(t, srv.Client(), "GET", srv.URL+"/api/me", ""); status != http.StatusUnauthorized {
		t.Fatalf("GET /api/me status %d, want 401", status)
	}
	if status, body := call(t, srv.Client(), "GET", srv.URL+"/", ""); status != http.StatusOK || body != "page" {
		t.Fatalf("GET / = %d %q, want 200 \"page\"", status, body)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, "ws"+srv.URL[len("http"):]+"/ws", nil)
	if err != nil {
		t.Fatalf("dial /ws: %v", err)
	}
	t.Cleanup(func() { conn.CloseNow() })
	c := &client{t: t, conn: conn}
	c.send("LIST")
	c.expect("ROOMS")
}
