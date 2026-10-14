package main

import (
	"cmp"
	"context"
	"fmt"
	"log"
	"math/rand/v2"
	"net/http"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/coder/websocket"
)

const (
	maxMessageBytes    = 4096
	writeTimeout       = 2 * time.Second
	pingInterval       = 25 * time.Second           // keeps reverse proxies (nginx: 60s idle) from dropping a waiting player
	codeLetters        = "ABCDEFGHJKLMNPQRSTUVWXYZ" // no I or O, which look like 1 and 0
	codeLength         = 4
	minStartLevel      = 1
	maxStartLevel      = 15
	maxAttack          = 40              // the client's ATK range is 1 to kBoardHeight
	kosToWin           = 3               // kKosToWin in src/net/protocol.hpp
	countdown          = 3 * time.Second // must match kCountdownMs in src/app/duel.hpp
	defaultMatchLength = 2 * time.Minute
	defaultMatchGrace  = 500 * time.Millisecond
	maxNameLength      = 12       // kMaxNameLength in src/net/protocol.hpp
	defaultName        = "Player" // for clients that send no valid HELLO
	maxListedRooms     = 20       // kMaxListedRooms in src/net/protocol.hpp
	defaultMaxRooms    = 500
)

// Hub pairs browsers into two-player rooms, relays game messages between them and referees each match.
// It handles HELLO, CREATE, JOIN, LIST, UNLIST, QUICK, REMATCH, ATK and DEAD itself; other game messages go
// to the opponent unchanged, and the verbs only the server sends (ROOM, START, LEFT, ERR, KO, OVER, OPP, ROOMS)
// are dropped. Lobby watchers get the full ROOMS list whenever the set of open public rooms changes.
type Hub struct {
	mu          sync.Mutex
	rooms       map[string]*room
	watchers    map[*player]bool // players outside a room who asked for the lobby list
	nextSeq     uint64           // orders rooms oldest first
	maxRooms    int
	newCode     func() string
	newSeed     func() uint32
	afterFunc   func(d time.Duration, f func()) stopper
	matchLength time.Duration
	matchGrace  time.Duration // covers clients starting their clocks a one-way latency after the server
	// originPatterns are extra page hosts allowed to open /ws (e.g. "game.example.com"), for a
	// reverse proxy that does not pass the Host header through. The page's own host is always allowed.
	originPatterns []string
}

// stopper is a match timer; *time.Timer satisfies it.
type stopper interface{ Stop() bool }

type room struct {
	code    string
	level   int
	public  bool   // listed in the lobby while it has a free seat
	host    string // the creator's name when the room opened
	seq     uint64 // creation order, for the oldest-first list
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
	room *room  // guarded by Hub.mu
	name string // from HELLO, guarded by Hub.mu
}

// outgoing is a message to write after the hub lock is released.
type outgoing struct {
	to   *player
	text string
}

func NewHub() *Hub {
	return &Hub{
		rooms:       map[string]*room{},
		watchers:    map[*player]bool{},
		maxRooms:    defaultMaxRooms,
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
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{OriginPatterns: h.originPatterns})
	if err != nil {
		log.Printf("websocket accept: %v", err)
		return
	}
	defer conn.CloseNow()
	conn.SetReadLimit(maxMessageBytes)
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()
	go keepAlive(ctx, conn)
	p := &player{conn: conn, name: defaultName}
	defer h.leave(p)
	for {
		typ, data, err := conn.Read(ctx)
		if err != nil {
			return
		}
		if typ == websocket.MessageText {
			h.deliver(h.route(p, string(data)))
		}
	}
}

// keepAlive pings until ctx ends. A ping that gets no pong closes the connection.
func keepAlive(ctx context.Context, conn *websocket.Conn) {
	ticker := time.NewTicker(pingInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			pingCtx, cancel := context.WithTimeout(ctx, pingInterval)
			err := conn.Ping(pingCtx)
			cancel()
			if err != nil {
				conn.CloseNow()
				return
			}
		}
	}
}

func (h *Hub) route(p *player, text string) []outgoing {
	h.mu.Lock()
	defer h.mu.Unlock()
	verb, arg, _ := strings.Cut(text, " ")
	if verb == "HELLO" {
		if p.room == nil && validName(arg) {
			p.name = arg
		}
		return nil
	}
	if p.room == nil {
		switch verb {
		case "CREATE":
			return h.create(p, arg)
		case "JOIN":
			return h.join(p, arg)
		case "LIST":
			if arg != "" {
				return nil
			}
			h.watchers[p] = true
			return []outgoing{{p, h.roomsMessage()}}
		case "UNLIST":
			delete(h.watchers, p)
			return nil
		}
		return nil // relay traffic that raced a LEFT; nothing to do
	}
	switch verb {
	case "CREATE", "JOIN":
		return []outgoing{{p, "ERR already in a room"}}
	case "LIST", "UNLIST":
		return nil
	case "REMATCH":
		return h.rematch(p)
	case "ATK":
		return p.room.attack(p, arg)
	case "DEAD":
		if text != "DEAD" {
			return nil
		}
		return p.room.knockOut(p)
	case "ROOM", "START", "LEFT", "ERR", "KO", "OVER", "OPP", "ROOMS":
		return nil // only the server sends these; never relay a forged one
	}
	if other := p.room.opponent(p); other != nil {
		return []outgoing{{other, text}}
	}
	return nil
}

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

func (h *Hub) join(p *player, code string) []outgoing {
	delete(h.watchers, p) // a refused join unsubscribes too; the client asks for the list again
	r := h.rooms[code]
	if r == nil {
		return []outgoing{{p, "ERR unknown room"}}
	}
	if r.players[1] != nil {
		return []outgoing{{p, "ERR room is full"}}
	}
	r.players[1] = p
	p.room = r
	out := []outgoing{{r.players[0], "OPP " + p.name}, {p, "OPP " + r.players[0].name}}
	out = append(out, h.startMatch(r)...)
	if r.public {
		out = append(out, h.lobbyUpdate()...)
	}
	return out
}

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
	delete(h.watchers, p)
	if r := p.room; r != nil {
		if r.timer != nil {
			r.timer.Stop()
		}
		wasListed := r.public && r.players[1] == nil
		delete(h.rooms, r.code)
		if other := r.opponent(p); other != nil {
			other.room = nil
			out = append(out, outgoing{other, "LEFT"})
		}
		p.room = nil
		if wasListed {
			out = append(out, h.lobbyUpdate()...)
		}
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
