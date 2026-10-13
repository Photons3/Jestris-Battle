package main

import (
	"context"
	"fmt"
	"log"
	"math/rand/v2"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/coder/websocket"
)

const (
	maxMessageBytes    = 4096
	writeTimeout       = 2 * time.Second
	codeLetters        = "ABCDEFGHJKLMNPQRSTUVWXYZ" // no I or O, which look like 1 and 0
	codeLength         = 4
	minStartLevel      = 1
	maxStartLevel      = 15
	maxAttack          = 40              // the client's ATK range is 1 to kBoardHeight
	kosToWin           = 3               // kKosToWin in src/net/protocol.hpp
	countdown          = 3 * time.Second // must match kCountdownMs in src/app/duel.hpp
	defaultMatchLength = 2 * time.Minute
	defaultMatchGrace  = 500 * time.Millisecond
)

// Hub pairs browsers into two-player rooms, relays game messages between them and referees each match.
// It handles CREATE, JOIN, REMATCH, ATK and DEAD itself; other game messages go to the opponent unchanged,
// and the verbs only the server sends (ROOM, START, LEFT, ERR, KO, OVER) are dropped.
type Hub struct {
	mu          sync.Mutex
	rooms       map[string]*room
	newCode     func() string
	newSeed     func() uint32
	afterFunc   func(d time.Duration, f func()) stopper
	matchLength time.Duration
	matchGrace  time.Duration // covers clients starting their clocks a one-way latency after the server
}

// stopper is a match timer; *time.Timer satisfies it.
type stopper interface{ Stop() bool }

type room struct {
	code    string
	level   int
	players [2]*player
	rematch [2]bool
	// The current match, reset by every START.
	kos     [2]int
	sent    [2]int // lines sent: the total of each seat's accepted ATK n
	over    bool
	matchID uint64 // tells a stale timer from the current match's
	timer   stopper
}

type player struct {
	conn *websocket.Conn
	room *room // guarded by Hub.mu
}

// outgoing is a message to write after the hub lock is released.
type outgoing struct {
	to   *player
	text string
}

func NewHub() *Hub {
	return &Hub{
		rooms:       map[string]*room{},
		newCode:     randomCode,
		newSeed:     rand.Uint32,
		afterFunc:   func(d time.Duration, f func()) stopper { return time.AfterFunc(d, f) },
		matchLength: defaultMatchLength,
		matchGrace:  defaultMatchGrace,
	}
}

func randomCode() string {
	b := make([]byte, codeLength)
	for i := range b {
		b[i] = codeLetters[rand.IntN(len(codeLetters))]
	}
	return string(b)
}

func (h *Hub) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, nil)
	if err != nil {
		log.Printf("websocket accept: %v", err)
		return
	}
	defer conn.CloseNow()
	conn.SetReadLimit(maxMessageBytes)
	p := &player{conn: conn}
	defer h.leave(p)
	for {
		typ, data, err := conn.Read(context.Background())
		if err != nil {
			return
		}
		if typ == websocket.MessageText {
			h.deliver(h.route(p, string(data)))
		}
	}
}

func (h *Hub) route(p *player, text string) []outgoing {
	h.mu.Lock()
	defer h.mu.Unlock()
	verb, arg, _ := strings.Cut(text, " ")
	if p.room == nil {
		switch verb {
		case "CREATE":
			return h.create(p, arg)
		case "JOIN":
			return h.join(p, arg)
		}
		return nil // relay traffic that raced a LEFT; nothing to do
	}
	switch verb {
	case "CREATE", "JOIN":
		return []outgoing{{p, "ERR already in a room"}}
	case "REMATCH":
		return h.rematch(p)
	case "ATK":
		return p.room.attack(p, arg)
	case "DEAD":
		if text != "DEAD" {
			return nil
		}
		return p.room.knockOut(p)
	case "ROOM", "START", "LEFT", "ERR", "KO", "OVER":
		return nil // only the server sends these; never relay a forged one
	}
	if other := p.room.opponent(p); other != nil {
		return []outgoing{{other, text}}
	}
	return nil
}

func (h *Hub) create(p *player, arg string) []outgoing {
	level, err := strconv.Atoi(arg)
	if err != nil || level < minStartLevel || level > maxStartLevel {
		return []outgoing{{p, "ERR bad start level"}}
	}
	code := h.newCode()
	for h.rooms[code] != nil {
		code = h.newCode()
	}
	r := &room{code: code, level: level}
	r.players[0] = p
	p.room = r
	h.rooms[code] = r
	return []outgoing{{p, "ROOM " + code}}
}

func (h *Hub) join(p *player, code string) []outgoing {
	r := h.rooms[code]
	if r == nil {
		return []outgoing{{p, "ERR unknown room"}}
	}
	if r.players[1] != nil {
		return []outgoing{{p, "ERR room is full"}}
	}
	r.players[1] = p
	p.room = r
	return h.startMatch(r)
}

func (h *Hub) rematch(p *player) []outgoing {
	r := p.room
	if r.opponent(p) == nil || !r.over {
		return nil
	}
	r.rematch[r.seat(p)] = true
	if r.rematch[0] && r.rematch[1] {
		return h.startMatch(r)
	}
	return nil
}

func (h *Hub) leave(p *player) {
	h.mu.Lock()
	var out []outgoing
	if r := p.room; r != nil {
		if r.timer != nil {
			r.timer.Stop()
		}
		delete(h.rooms, r.code)
		if other := r.opponent(p); other != nil {
			other.room = nil
			out = append(out, outgoing{other, "LEFT"})
		}
		p.room = nil
	}
	h.mu.Unlock()
	h.deliver(out)
}

// deliver writes outside the lock; a failed write closes that connection, whose read loop then cleans up.
func (h *Hub) deliver(out []outgoing) {
	for _, o := range out {
		ctx, cancel := context.WithTimeout(context.Background(), writeTimeout)
		_ = o.to.conn.Write(ctx, websocket.MessageText, []byte(o.text))
		cancel()
	}
}

// startMatch resets the room's match state, arms the match timer and sends START to both players.
func (h *Hub) startMatch(r *room) []outgoing {
	if r.timer != nil {
		r.timer.Stop()
	}
	r.rematch = [2]bool{}
	r.kos, r.sent, r.over = [2]int{}, [2]int{}, false
	r.matchID++
	id := r.matchID
	r.timer = h.afterFunc(countdown+h.matchLength+h.matchGrace, func() { h.timeUp(r, id) })
	msg := fmt.Sprintf("START %d %d", h.newSeed(), r.level)
	return []outgoing{{r.players[0], msg}, {r.players[1], msg}}
}

// timeUp runs on the timer's goroutine and ends match id unless it is already over or replaced.
func (h *Hub) timeUp(r *room, id uint64) {
	h.mu.Lock()
	var out []outgoing
	if h.rooms[r.code] == r && r.matchID == id && !r.over {
		out = r.finish("TIME")
	}
	h.mu.Unlock()
	h.deliver(out)
}

func (r *room) playing() bool { return r.matchID > 0 && !r.over }

// attack counts a valid ATK toward the sender's lines sent and relays it in canonical form.
func (r *room) attack(p *player, arg string) []outgoing {
	n, err := strconv.Atoi(arg)
	if !r.playing() || err != nil || n < 1 || n > maxAttack {
		return nil
	}
	r.sent[r.seat(p)] += n
	return []outgoing{{r.opponent(p), fmt.Sprintf("ATK %d", n)}}
}

// knockOut scores a KO for p's opponent, tells both players, and ends the match at kosToWin.
func (r *room) knockOut(p *player) []outgoing {
	if !r.playing() {
		return nil
	}
	winner := 1 - r.seat(p)
	r.kos[winner]++
	out := make([]outgoing, 0, 4)
	for seat, q := range r.players {
		out = append(out, outgoing{q, fmt.Sprintf("KO %d %d", r.kos[seat], r.kos[1-seat])})
	}
	if r.kos[winner] >= kosToWin {
		out = append(out, r.finish("KO")...)
	}
	return out
}

// finish ends the match and sends each player the verdict from their own side.
func (r *room) finish(reason string) []outgoing {
	r.over = true
	if r.timer != nil {
		r.timer.Stop()
	}
	out := make([]outgoing, 0, 2)
	for seat, p := range r.players {
		msg := fmt.Sprintf("OVER %s %s %d %d", r.verdict(seat), reason, r.sent[seat], r.sent[1-seat])
		out = append(out, outgoing{p, msg})
	}
	return out
}

// verdict compares KOs, then lines sent, from seat's side.
func (r *room) verdict(seat int) string {
	mine, theirs := r.kos[seat], r.kos[1-seat]
	if mine == theirs {
		mine, theirs = r.sent[seat], r.sent[1-seat]
	}
	switch {
	case mine > theirs:
		return "WIN"
	case mine < theirs:
		return "LOSE"
	}
	return "DRAW"
}

func (r *room) opponent(p *player) *player {
	if r.players[0] == p {
		return r.players[1]
	}
	return r.players[0]
}

func (r *room) seat(p *player) int {
	if r.players[0] == p {
		return 0
	}
	return 1
}
