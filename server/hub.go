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
	maxRoomPlayers     = 16       // kMaxRoomPlayers in src/net/protocol.hpp
	defaultMaxRooms    = 500
	sendQueueLength    = 1024 // per player; a full 16-player room relays about 150 SNAPs a second to each
	maxInputEvents     = 64   // kMaxInputEvents in src/net/protocol.hpp
)

// Hub gathers browsers into rooms of up to maxRoomPlayers, starts a match when the host sends BEGIN, relays
// game messages between the match's participants and referees each match: a duel for 2, a royale for 3 or
// more. It handles HELLO, CREATE, JOIN, LIST, UNLIST, QUICK, BEGIN, ATK, DEAD and SNAP itself (with
// -authority enforce it drops ATK, DEAD and SNAP), and hands a participant's IN and SDF to the match's runner
// (authority.go), which re-simulates every game and, in enforce mode, decides attacks and top-outs; anything
// else inside a room, including the verbs only the server sends (ROOM, PLAYERS, START, KO, OVER, OUT, LEFT,
// ERR, ROOMS), is dropped. Lobby watchers get the full ROOMS list whenever a listed room appears,
// disappears, or changes its player count or host.
//
// Messages are queued to each recipient while the hub lock is held, so every player gets them in the order
// the hub produced them; a writer goroutine per connection writes them outside the lock.
type Hub struct {
	mu          sync.Mutex
	rooms       map[string]*room
	watchers    map[*player]bool // players outside a room who asked for the lobby list
	nextSeq     uint64           // orders rooms oldest first
	maxRooms    int
	newCode     func() string
	newSeed     func() uint32
	pickTarget  func(n int) int // a royale ATK goes to living[pickTarget(len(living))]
	afterFunc   func(d time.Duration, f func()) stopper
	matchLength time.Duration
	matchGrace  time.Duration // covers clients starting their clocks a one-way latency after the server
	auth        authority
	newSim      func() (gameSim, error) // the game core for one match's runner
	now         func() time.Time
	// manualRunners leaves each runner without its goroutine, for tests that drain and tick it themselves.
	manualRunners bool
	// originPatterns are extra page hosts allowed to open /ws (e.g. "game.example.com"), for a
	// reverse proxy that does not pass the Host header through. The page's own host is always allowed.
	originPatterns []string
}

// stopper is a match timer; *time.Timer satisfies it.
type stopper interface{ Stop() bool }

// room and everything it points to is guarded by Hub.mu.
type room struct {
	code    string
	level   int
	public  bool      // listed in the lobby while it has a free place
	seq     uint64    // creation order, for the oldest-first list
	players []*player // join order
	hostID  int
	match   *match // nil between matches
	matchID uint64 // counts BEGINs; tells a stale timer from the current match's
	timer   stopper
}

type player struct {
	conn     *websocket.Conn
	room     *room         // guarded by Hub.mu
	name     string        // from HELLO, guarded by Hub.mu
	id       int           // in the room, 1 to maxRoomPlayers
	roomName string        // name made unique in the room
	queue    chan string   // outbound messages in production order, filled under Hub.mu
	kick     chan struct{} // closed when queue overflows; the writer then closes the connection
	kicked   bool          // guarded by Hub.mu
}

func newPlayer(conn *websocket.Conn) *player {
	return &player{
		conn:  conn,
		name:  defaultName,
		queue: make(chan string, sendQueueLength),
		kick:  make(chan struct{}),
	}
}

// match is one match in a room: its participants are the room's players at BEGIN.
type match struct {
	royale  bool
	players []*player // participants, in START order
	kos     map[*player]int
	sent    map[*player]int // lines sent: the total of each participant's accepted ATK n
	alive   map[*player]bool
	runner  *matchRunner // nil without a simulation, and once the match has ended
}

// outgoing is a message for the queue of player to.
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
		pickTarget:  rand.IntN,
		afterFunc:   func(d time.Duration, f func()) stopper { return time.AfterFunc(d, f) },
		matchLength: defaultMatchLength,
		matchGrace:  defaultMatchGrace,
		auth:        defaultAuthority(),
		newSim:      newSharedSim,
		now:         time.Now,
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
	p := newPlayer(conn)
	go p.write(ctx)
	defer h.leave(p)
	for {
		typ, data, err := conn.Read(ctx)
		if err != nil {
			return
		}
		if typ == websocket.MessageText {
			h.receive(p, string(data))
		}
	}
}

// write sends p's queued messages until ctx ends, each with the write timeout. A failed write or a kick
// closes the connection, whose read loop then leaves.
func (p *player) write(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		case <-p.kick:
			p.conn.CloseNow()
			return
		case text := <-p.queue:
			writeCtx, cancel := context.WithTimeout(ctx, writeTimeout)
			err := p.conn.Write(writeCtx, websocket.MessageText, []byte(text))
			cancel()
			if err != nil {
				p.conn.CloseNow()
				return
			}
		}
	}
}

// receive handles one message from p and queues what it causes, all under the lock.
func (h *Hub) receive(p *player, text string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	enqueue(h.route(p, text))
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

// route handles one message from p; the caller holds the lock.
func (h *Hub) route(p *player, text string) []outgoing {
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
		case "QUICK":
			return h.quick(p, arg)
		}
		return nil // a lobby or idle connection: match verbs and anything unknown mean nothing here
	}
	if h.auth.mode == "enforce" && (verb == "ATK" || verb == "DEAD" || verb == "SNAP") {
		return nil // the simulation decides attacks, top-outs and snapshots
	}
	switch verb {
	case "CREATE", "JOIN", "QUICK":
		return []outgoing{{p, "ERR already in a room"}}
	case "BEGIN":
		if text != "BEGIN" {
			return nil
		}
		return h.begin(p)
	case "ATK":
		return h.attack(p, arg)
	case "DEAD":
		if text != "DEAD" {
			return nil
		}
		if m := p.room.match; m != nil && m.runner != nil && m.alive[p] {
			m.runner.push(runnerMsg{from: p, kind: msgClaimedDead})
		}
		return p.room.knockOut(p)
	case "SNAP":
		return p.room.relaySnapshot(p, arg)
	case "IN", "SDF":
		if m := p.room.match; m != nil && m.runner != nil && m.has(p) {
			kind := msgIN
			if verb == "SDF" {
				kind = msgSDF
			}
			m.runner.push(runnerMsg{from: p, kind: kind, text: text})
		}
		return nil
	}
	return nil // lobby verbs, server-only verbs (never relay a forged one) and unknown verbs
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
	r := &room{code: code, level: level, public: public, seq: h.nextSeq}
	h.rooms[code] = r
	return h.enter(p, r)
}

func (h *Hub) join(p *player, code string) []outgoing {
	delete(h.watchers, p) // a refused join unsubscribes too; the client asks for the list again
	r := h.rooms[code]
	if r == nil {
		return []outgoing{{p, "ERR unknown room"}}
	}
	if len(r.players) >= maxRoomPlayers {
		return []outgoing{{p, "ERR room is full"}}
	}
	return h.enter(p, r)
}

// enter adds p to r with the lowest free ID and a room-unique name. p gets ROOM, the room gets PLAYERS, and
// the lobby hears of a public room's new player count. A player who joins mid-match waits for the next one.
func (h *Hub) enter(p *player, r *room) []outgoing {
	p.id = 1
	for r.player(p.id) != nil {
		p.id++
	}
	p.roomName = uniqueName(p.name, func(s string) bool {
		return slices.ContainsFunc(r.players, func(q *player) bool { return q.roomName == s })
	})
	if len(r.players) == 0 {
		r.hostID = p.id
	}
	r.players = append(r.players, p)
	p.room = r
	out := []outgoing{{p, fmt.Sprintf("ROOM %s %d", r.code, p.id)}}
	out = append(out, r.broadcast(r.rosterMessage())...)
	if r.public {
		out = append(out, h.lobbyUpdate()...)
	}
	return out
}

// uniqueName returns base, or else base with the lowest free suffix 2, 3, ..., shortening base so the result
// stays within maxNameLength.
func uniqueName(base string, taken func(string) bool) string {
	if !taken(base) {
		return base
	}
	for n := 2; ; n++ {
		suffix := strconv.Itoa(n)
		name := base[:min(len(base), maxNameLength-len(suffix))] + suffix
		if !taken(name) {
			return name
		}
	}
}

// quick joins the oldest listed room with no match running, else the oldest listed room, at that room's
// level, or opens a public room at level arg.
func (h *Hub) quick(p *player, arg string) []outgoing {
	level, err := strconv.Atoi(arg)
	if err != nil || level < minStartLevel || level > maxStartLevel {
		return []outgoing{{p, "ERR bad start level"}}
	}
	open := h.openPublic()
	if len(open) == 0 {
		return h.open(p, level, true)
	}
	if i := slices.IndexFunc(open, func(r *room) bool { return r.match == nil }); i >= 0 {
		return h.join(p, open[i].code)
	}
	return h.join(p, open[0].code)
}

// openPublic lists the rooms a lobby can join, oldest first.
func (h *Hub) openPublic() []*room {
	var open []*room
	for _, r := range h.rooms {
		if r.public && len(r.players) < maxRoomPlayers {
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
		fmt.Fprintf(&b, " %s:%d:%s:%d", r.code, r.level, r.player(r.hostID).roomName, len(r.players))
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

// leave takes p out of its room. A duel p was playing ends with LEFT to the other participant; in a royale a
// living p is eliminated as if by DEAD, but gets nothing. The longest present player becomes host if p was,
// and an empty room is deleted.
func (h *Hub) leave(p *player) {
	h.mu.Lock()
	var out []outgoing
	delete(h.watchers, p)
	if r := p.room; r != nil {
		if m := r.match; m != nil && m.runner != nil && m.has(p) {
			m.runner.push(runnerMsg{from: p, kind: msgLeft})
		}
		r.players = slices.DeleteFunc(r.players, func(q *player) bool { return q == p })
		p.room = nil
		if m := r.match; m != nil && !m.royale && m.has(p) {
			r.endMatch()
			out = append(out, outgoing{m.opponent(p), "LEFT"})
		}
		if m := r.match; m != nil && m.royale && m.alive[p] {
			out = append(out, r.eliminate(p)...)
		}
		if len(r.players) == 0 {
			r.endMatch()
			delete(h.rooms, r.code)
		} else {
			if r.hostID == p.id {
				r.hostID = r.players[0].id
			}
			out = append(out, r.broadcast(r.rosterMessage())...)
		}
		if r.public {
			out = append(out, h.lobbyUpdate()...)
		}
	}
	enqueue(out)
	h.mu.Unlock()
}

// enqueue adds each message to its recipient's queue without blocking; the caller holds the lock. A
// recipient whose queue is full is kicked: its writer closes the connection and its read loop leaves.
func enqueue(out []outgoing) {
	for _, o := range out {
		select {
		case o.to.queue <- o.text:
		default:
			o.to.disconnect()
		}
	}
}

// disconnect kicks p once: its writer closes the connection and its read loop leaves. The caller holds the lock.
func (p *player) disconnect() {
	if !p.kicked {
		p.kicked = true
		close(p.kick)
	}
}

// begin starts a match for everyone in p's room if p is the host, there are 2 or more players and no match
// is running. Each participant gets the same START, then the room gets PLAYERS. A duel arms the match timer.
// In enforce mode a match whose simulation cannot start ends at once, as after a simulation error.
func (h *Hub) begin(p *player) []outgoing {
	r := p.room
	if p.id != r.hostID || len(r.players) < 2 || r.match != nil {
		return nil
	}
	m := &match{
		royale:  len(r.players) > 2,
		players: slices.Clone(r.players),
		kos:     map[*player]int{},
		sent:    map[*player]int{},
		alive:   map[*player]bool{},
	}
	seed := h.newSeed()
	var b strings.Builder
	fmt.Fprintf(&b, "START %d %d", seed, r.level)
	for _, q := range m.players {
		m.alive[q] = true
		fmt.Fprintf(&b, " %d", q.id)
	}
	r.match = m
	r.matchID++
	m.runner = h.newRunner(r, m, seed)
	if !m.royale {
		id := r.matchID
		r.timer = h.afterFunc(countdown+h.matchLength+h.matchGrace, func() { h.timeUp(r, id) })
	}
	out := make([]outgoing, 0, 2*len(r.players))
	for _, q := range m.players {
		out = append(out, outgoing{q, b.String()})
	}
	out = append(out, r.broadcast(r.rosterMessage())...)
	if m.runner == nil && h.auth.mode == "enforce" {
		// a failed simulation: never fall back to trusting clients
		out = append(out, m.tellConnected(r, "LEFT")...)
		r.endMatch()
		out = append(out, r.broadcast(r.rosterMessage())...)
	}
	return out
}

// timeUp runs on the timer's goroutine and ends match id unless it is already over or replaced.
func (h *Hub) timeUp(r *room, id uint64) {
	h.mu.Lock()
	var out []outgoing
	if h.rooms[r.code] == r && r.matchID == id && r.match != nil {
		out = r.finish("TIME")
	}
	enqueue(out)
	h.mu.Unlock()
}

// player returns the player with ID id, or nil.
func (r *room) player(id int) *player {
	for _, q := range r.players {
		if q.id == id {
			return q
		}
	}
	return nil
}

// rosterMessage is "PLAYERS <hostID> id:name:m ..." in join order, where m is 1 for a match participant.
func (r *room) rosterMessage() string {
	var b strings.Builder
	fmt.Fprintf(&b, "PLAYERS %d", r.hostID)
	for _, q := range r.players {
		inMatch := 0
		if r.match != nil && r.match.has(q) {
			inMatch = 1
		}
		fmt.Fprintf(&b, " %d:%s:%d", q.id, q.roomName, inMatch)
	}
	return b.String()
}

// broadcast sends text to everyone in the room, in join order.
func (r *room) broadcast(text string) []outgoing {
	out := make([]outgoing, 0, len(r.players))
	for _, q := range r.players {
		out = append(out, outgoing{q, text})
	}
	return out
}

// endMatch returns the room to between matches and stops the match's runner without waiting for it.
func (r *room) endMatch() {
	if m := r.match; m != nil && m.runner != nil {
		close(m.runner.stop)
		m.runner = nil
	}
	r.match = nil
	if r.timer != nil {
		r.timer.Stop()
	}
}

// relaySnapshot sends a participant's SNAP to every other participant, tagged with the sender's ID.
func (r *room) relaySnapshot(p *player, arg string) []outgoing {
	m := r.match
	if m == nil || !m.has(p) || arg == "" {
		return nil
	}
	msg := "SNAP " + strconv.Itoa(p.id) + " " + arg
	out := make([]outgoing, 0, len(m.players)-1)
	for _, q := range m.players {
		if q != p && q.room == r {
			out = append(out, outgoing{q, msg})
		}
	}
	return out
}

// duelist reports whether p is playing in the room's running duel.
func (r *room) duelist(p *player) bool {
	return r.match != nil && !r.match.royale && r.match.has(p)
}

// attack counts a valid ATK from a living participant toward its lines sent and routes it: in a duel to the
// opponent, in a royale to a living opponent that pickTarget chooses. With a runner the target gets
// GARB <id> <n> from it; without one, ATK n in canonical form.
func (h *Hub) attack(p *player, arg string) []outgoing {
	n, ok := h.validateAttack(p, arg)
	if !ok {
		return nil
	}
	to := h.routeGarbage(p, n)
	if r := p.room.match.runner; r != nil {
		r.push(runnerMsg{from: p, kind: msgClaimedAttack, to: to, lines: n})
		return nil
	}
	return []outgoing{{to, fmt.Sprintf("ATK %d", n)}}
}

// validateAttack parses ATK's argument; ok only for 1 to maxAttack lines from a living participant.
func (h *Hub) validateAttack(p *player, arg string) (n int, ok bool) {
	m := p.room.match
	n, err := strconv.Atoi(arg)
	if m == nil || !m.alive[p] || err != nil || n < 1 || n > maxAttack {
		return 0, false
	}
	return n, true
}

// routeGarbage counts n lines sent by living participant p and picks who receives them.
func (h *Hub) routeGarbage(p *player, n int) *player {
	m := p.room.match
	m.sent[p] += n
	if !m.royale {
		return m.opponent(p)
	}
	living := slices.DeleteFunc(slices.Clone(m.players), func(q *player) bool { return q == p || !m.alive[q] })
	return living[h.pickTarget(len(living))]
}

// knockOut handles DEAD: a KO in a duel, an elimination in a royale.
func (r *room) knockOut(p *player) []outgoing { return r.topOut(p) }

// topOut scores participant p's top-out: a KO in a duel; in a royale an elimination of p if it is still
// alive, with PLAYERS to the room when that ends the match.
func (r *room) topOut(p *player) []outgoing {
	if m := r.match; m != nil && m.royale && m.alive[p] {
		out := r.eliminate(p)
		if r.match == nil {
			out = append(out, r.broadcast(r.rosterMessage())...)
		}
		return out
	}
	if !r.duelist(p) {
		return nil
	}
	return r.scoreKO(p)
}

// eliminate puts living royale participant p out in place n, the number alive before it, telling every
// participant still in the room. When one player is left it wins with OUT <id> 1 and the match ends; the
// caller then broadcasts PLAYERS.
func (r *room) eliminate(p *player) []outgoing {
	m := r.match
	place := 0
	for _, q := range m.players {
		if m.alive[q] {
			place++
		}
	}
	m.alive[p] = false
	out := m.tellConnected(r, fmt.Sprintf("OUT %d %d", p.id, place))
	if place == 2 {
		i := slices.IndexFunc(m.players, func(q *player) bool { return m.alive[q] })
		out = append(out, m.tellConnected(r, fmt.Sprintf("OUT %d 1", m.players[i].id))...)
		r.endMatch()
	}
	return out
}

// tellConnected sends text to every participant still in room r, in START order.
func (m *match) tellConnected(r *room, text string) []outgoing {
	out := make([]outgoing, 0, len(m.players))
	for _, q := range m.players {
		if q.room == r {
			out = append(out, outgoing{q, text})
		}
	}
	return out
}

// scoreKO scores a KO for duelist p's opponent, tells both players, and ends the match at kosToWin.
func (r *room) scoreKO(p *player) []outgoing {
	m := r.match
	winner := m.opponent(p)
	m.kos[winner]++
	out := make([]outgoing, 0, 4)
	for _, q := range m.players {
		out = append(out, outgoing{q, fmt.Sprintf("KO %d %d", m.kos[q], m.kos[m.opponent(q)])})
	}
	if m.kos[winner] >= kosToWin {
		out = append(out, r.finish("KO")...)
	}
	return out
}

// finish ends the duel, sends each participant the verdict from their own side, then sends the room PLAYERS.
func (r *room) finish(reason string) []outgoing {
	m := r.match
	r.endMatch()
	out := make([]outgoing, 0, 2+len(r.players))
	for _, p := range m.players {
		other := m.opponent(p)
		msg := fmt.Sprintf("OVER %s %s %d %d", m.verdict(p), reason, m.sent[p], m.sent[other])
		out = append(out, outgoing{p, msg})
	}
	return append(out, r.broadcast(r.rosterMessage())...)
}

// verdict compares KOs, then lines sent, from p's side of the duel.
func (m *match) verdict(p *player) string {
	other := m.opponent(p)
	mine, theirs := m.kos[p], m.kos[other]
	if mine == theirs {
		mine, theirs = m.sent[p], m.sent[other]
	}
	switch {
	case mine > theirs:
		return "WIN"
	case mine < theirs:
		return "LOSE"
	}
	return "DRAW"
}

func (m *match) has(p *player) bool { return slices.Contains(m.players, p) }

// opponent is the other participant of a duel.
func (m *match) opponent(p *player) *player {
	if m.players[0] == p {
		return m.players[1]
	}
	return m.players[0]
}
