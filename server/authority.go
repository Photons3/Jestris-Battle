package main

import (
	"cmp"
	"context"
	"fmt"
	"log"
	"math"
	"slices"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"tetris-server/sim"
)

const (
	defaultSdf           = 20 // kDefaultSoftDropFactor
	minSdf               = 5  // kMinSdf
	maxSdf               = 40 // kMaxSdf
	runnerTick           = 100 * time.Millisecond
	runnerQueuePerPlayer = 64  // IN and SDF messages waiting per participant; more are dropped
	entryAllowanceBase   = 256 // rule 4: a player's accepted entries stay within tEnd/4 + this
	resyncInterval       = 100 * time.Millisecond
	resyncLimitWindow    = 10 * time.Second // -max-resyncs counts the RESYNCs sent within this
	defaultLead          = 250 * time.Millisecond
	defaultLag           = time.Second
	defaultGarbageWindow = 750 * time.Millisecond
	defaultMaxResyncs    = 10
)

// gameSim is the game core the runner simulates with; *sim.Match satisfies it.
type gameSim interface {
	New(seed uint32, level, players, sdf int) (int, error)
	Free(h int) error
	Input(h, code int) error
	AcceptGarbage(h, id, lines int) error
	Advance(h, ms int) error
	T(h int) (int, error)
	TakeAttack(h int) (int, error)
	TopOuts(h int) (int, error)
	Eliminated(h int) (bool, error)
	Hash(h int) (uint64, error)
	Snapshot(h int) (string, error)
	Close() error
}

// authority holds the server's -authority, -lead, -lag, -garbage-window and -max-resyncs settings.
type authority struct {
	mode                     string // "log" | "enforce"
	lead, lag, garbageWindow time.Duration
	maxResyncs               int // per 10 s
}

func defaultAuthority() authority {
	return authority{mode: "log", lead: defaultLead, lag: defaultLag, garbageWindow: defaultGarbageWindow,
		maxResyncs: defaultMaxResyncs}
}

var (
	engineOnce sync.Once
	engine     *sim.Engine
	engineErr  error
)

// newSharedSim instantiates the game core for one match from the process-wide engine, which compiles the
// module on first use.
func newSharedSim() (gameSim, error) {
	engineOnce.Do(func() { engine, engineErr = sim.NewEngine(context.Background()) })
	if engineErr != nil {
		return nil, engineErr
	}
	m, err := engine.NewMatch(context.Background())
	if err != nil {
		return nil, err
	}
	return m, nil
}

type msgKind int

const (
	msgIN            msgKind = iota
	msgSDF                   // the text is the whole message
	msgClaimedAttack         // log mode: route() relayed an ATK of lines to to; the runner sends GARB
	msgClaimedDead
	msgLeft
)

// runnerMsg is something route() or leave() hands the runner of from's match.
type runnerMsg struct {
	from      *player
	kind      msgKind
	text      string
	to        *player
	id, lines int
	seq       int // msgIN: the IN's number among from's INs this match, from 1
}

// garbageWindow is a GARB the player has not acknowledged yet: lines sent when its server-side t was t0.
type garbageWindow struct{ lines, t0 int }

// playerSim is one participant's simulated game; only the runner touches it, except dropped.
type playerSim struct {
	p              *player
	id             int // p's ID in the room at BEGIN
	h              int
	started        bool // the sim game exists
	t              int  // server-side match time
	reportedEnd    int  // the last accepted IN's tEnd
	accepted       int  // the player's entries accepted so far; rule 4 caps it at tEnd/4 + 256
	changed        bool // an entry, accepted or forced, has changed the game since it was created
	sdfSeen        bool
	pending        map[int]garbageWindow
	nextGarbageID  int
	canonical      []string // enforce only: accepted and forced entries, as t:token
	needResync     bool
	sawINSinceMark bool
	resyncTimes    []time.Time // RESYNCs sent within the last resyncLimitWindow
	simAttack      int
	simTopOuts     int
	claimedSent    int
	claimedDead    int
	eliminated     bool
	wouldResync    map[string]int
	lastSnap       string
	left           bool
	dropped        atomic.Int32 // messages route() could not queue; counted under Hub.mu, read by the runner
	waiting        atomic.Int32 // p's IN and SDF messages in the runner's inbox
	ins            int          // INs route() has seen from p this match; only touched under Hub.mu
	droppedIN      atomic.Int64 // the number of the last IN route() could not queue
	consumedINs    int          // INs the runner has consumed (handled, rejected or dropped); RESYNC's count
}

type effectKind int

const (
	effGarbage effectKind = iota // GARB id lines to to
	effAttack                    // the simulation of from sent lines
	effTopOut                    // the simulation of from topped out
	effEliminated
	effSnap     // SNAP <from's id> text
	effResync   // text to from
	effKick     // from is over -max-resyncs
	effSimError // the simulation failed: the match ends for everyone with LEFT
)

// runnerEffect is an output the runner applies under Hub.mu.
type runnerEffect struct {
	kind      effectKind
	from, to  *player
	id, lines int
	text      string
}

// matchRunner re-simulates every participant of one match from its IN messages, on its own goroutine.
// It owns sim for the match's lifetime and touches hub, room and player state only under Hub.mu.
type matchRunner struct {
	match   *match
	room    *room
	id      uint64 // the room's matchID for this match
	seed    uint32
	level   int
	auth    authority
	now     func() time.Time
	sim     gameSim
	players map[*player]*playerSim // fixed at begin; route() reads it under Hub.mu
	inMu    sync.Mutex
	inbox   []runnerMsg   // under inMu: what push() handed over, oldest first
	notify  chan struct{} // 1-buffered: the inbox has something
	stop    chan struct{}
	wall0   time.Time
	effects []runnerEffect
	failed  bool // the simulation returned an error; nothing more is simulated
}

// newRunner creates the runner for match m in room r, which begin() has just started; the caller holds the
// lock. It returns nil, and the match runs without simulation, when the game core is unavailable.
func (h *Hub) newRunner(r *room, m *match, seed uint32) *matchRunner {
	s, err := h.newSim()
	if err != nil {
		if h.auth.mode == "enforce" { // begin() then ends the match: player 0 is no player's game
			log.Printf("sim error room=%s seed=%d player=%d: %v", r.code, seed, 0, err)
		} else {
			log.Printf("authority room=%s seed=%d: no simulation: %v", r.code, seed, err)
		}
		return nil
	}
	run := &matchRunner{
		match:   m,
		room:    r,
		id:      r.matchID,
		seed:    seed,
		level:   r.level,
		auth:    h.auth,
		now:     h.now,
		sim:     s,
		players: map[*player]*playerSim{},
		notify:  make(chan struct{}, 1),
		stop:    make(chan struct{}),
		wall0:   h.now().Add(countdown),
	}
	for _, q := range m.players {
		run.players[q] = &playerSim{p: q, id: q.id, pending: map[int]garbageWindow{}, nextGarbageID: 1,
			wouldResync: map[string]int{}}
	}
	if !h.manualRunners {
		go run.loop(h)
	}
	return run
}

// push hands msg to the runner without blocking; the caller holds the lock. An IN or SDF from a player who
// already has runnerQueuePerPlayer of them waiting is dropped, and the runner treats that as a bad message from
// msg.from on its next drain; one player's flood never drops another's messages. Control messages (a claimed
// ATK or DEAD, a player leaving) are never dropped.
func (r *matchRunner) push(msg runnerMsg) {
	ps := r.players[msg.from]
	if msg.kind == msgIN {
		ps.ins++
		msg.seq = ps.ins
	}
	if msg.kind == msgIN || msg.kind == msgSDF {
		if ps.waiting.Load() >= runnerQueuePerPlayer {
			ps.dropped.Add(1)
			if msg.kind == msgIN {
				ps.droppedIN.Store(int64(msg.seq))
			}
			return
		}
		ps.waiting.Add(1)
	}
	r.inMu.Lock()
	r.inbox = append(r.inbox, msg)
	r.inMu.Unlock()
	select {
	case r.notify <- struct{}{}:
	default:
	}
}

// take empties the inbox.
func (r *matchRunner) take() []runnerMsg {
	r.inMu.Lock()
	defer r.inMu.Unlock()
	msgs := r.inbox
	r.inbox = nil
	return msgs
}

// loop runs the match until endMatch closes stop: messages as they come, the horizon and snapshots every
// tick. Messages still queued at the stop are counted for the summary.
func (r *matchRunner) loop(h *Hub) {
	ticker := time.NewTicker(runnerTick)
	defer ticker.Stop()
	for {
		select {
		case <-r.stop:
			r.drain(h)
			r.finish()
			return
		case <-r.notify:
			r.drain(h)
		case <-ticker.C:
			r.tick(h, r.now())
		}
	}
}

// drain handles every queued runnerMsg, then applies the effects. A dropped IN counts as consumed: every IN
// numbered below it was queued before it, so this drain handles them before any RESYNC is built.
func (r *matchRunner) drain(h *Hub) {
	for _, q := range r.match.players {
		ps := r.players[q]
		if ps.dropped.Swap(0) > 0 {
			r.mark(ps, "bad-in")
		}
		ps.consumedINs = max(ps.consumedINs, int(ps.droppedIN.Load()))
	}
	for msgs := r.take(); len(msgs) > 0; msgs = r.take() {
		for _, msg := range msgs {
			r.handle(msg)
		}
	}
	r.apply(h)
}

func (r *matchRunner) handle(msg runnerMsg) {
	ps := r.players[msg.from]
	if msg.kind == msgIN || msg.kind == msgSDF {
		ps.waiting.Add(-1)
	}
	switch msg.kind {
	case msgIN:
		ps.consumedINs = max(ps.consumedINs, msg.seq)
		if !ps.left && !(ps.eliminated && r.auth.mode == "enforce") { // an enforced elimination is final
			r.handleIN(ps, msg.text)
		}
	case msgSDF:
		r.handleSDF(ps, msg.text)
	case msgClaimedAttack:
		ps.claimedSent += msg.lines
		target := r.players[msg.to]
		id := target.nextGarbageID
		target.nextGarbageID++
		target.pending[id] = garbageWindow{lines: msg.lines, t0: target.t}
		r.effects = append(r.effects, runnerEffect{kind: effGarbage, from: msg.from, to: msg.to, id: id, lines: msg.lines})
	case msgClaimedDead:
		ps.claimedDead++
	case msgLeft:
		ps.left = true
	}
}

// tick moves every live player's game to the horizon, and in enforce mode produces changed snapshots and
// due RESYNCs, then applies the effects.
func (r *matchRunner) tick(h *Hub, now time.Time) {
	horizon := r.wallMs(now) - int(r.auth.lag/time.Millisecond)
	for _, q := range r.match.players {
		ps := r.players[q]
		if ps.left || ps.eliminated || r.failed {
			continue
		}
		if ps.t < horizon && r.ensureGame(ps, defaultSdf) {
			r.advanceTo(ps, horizon) // forced: the client replays the same stall, so this marks nothing
		}
		if r.auth.mode != "enforce" {
			continue
		}
		if ps.started && !r.failed {
			snap, err := r.sim.Snapshot(ps.h)
			if err != nil {
				r.fail(ps, err)
				continue
			}
			if snap != ps.lastSnap {
				ps.lastSnap = snap
				r.effects = append(r.effects, runnerEffect{kind: effSnap, from: q, text: snap})
			}
		}
		due := len(ps.resyncTimes) == 0 || now.Sub(ps.resyncTimes[len(ps.resyncTimes)-1]) >= resyncInterval
		if ps.needResync && ps.sawINSinceMark && due {
			text := "RESYNC " + strconv.Itoa(ps.t) + " " + strconv.Itoa(ps.consumedINs)
			if len(ps.canonical) > 0 {
				text += " " + strings.Join(ps.canonical, " ")
			}
			r.effects = append(r.effects, runnerEffect{kind: effResync, from: q, text: text})
			ps.needResync, ps.sawINSinceMark = false, false
			ps.resyncTimes = append(slices.DeleteFunc(ps.resyncTimes, func(at time.Time) bool {
				return now.Sub(at) >= resyncLimitWindow
			}), now)
			if len(ps.resyncTimes) > r.auth.maxResyncs {
				r.effects = append(r.effects, runnerEffect{kind: effKick, from: q})
			}
		}
	}
	r.apply(h)
}

// apply sends the effects in one locked section, unless the match has ended or been replaced since; an effect
// that ends the match drops the rest. Log mode only compares the simulation's attacks and top-outs; it sends
// the GARBs of the claimed attacks. Enforce mode routes the simulation's attacks as GARBs and scores its
// top-outs.
func (r *matchRunner) apply(h *Hub) {
	if len(r.effects) == 0 {
		return
	}
	effects := r.effects
	r.effects = nil
	h.mu.Lock()
	defer h.mu.Unlock()
	var out []outgoing
	for _, e := range effects {
		if r.room.match != r.match || r.room.matchID != r.id {
			break
		}
		enforce := r.auth.mode == "enforce"
		switch e.kind {
		case effAttack:
			if enforce && e.from.room == r.room && r.match.alive[e.from] {
				to := h.routeGarbage(e.from, e.lines)
				target := r.players[to]
				id := target.nextGarbageID
				target.nextGarbageID++
				target.pending[id] = garbageWindow{lines: e.lines, t0: target.t}
				if to.room == r.room {
					out = append(out, outgoing{to, fmt.Sprintf("GARB %d %d", id, e.lines)})
				}
			}
		case effTopOut:
			if enforce {
				out = append(out, r.room.topOut(e.from)...)
			}
		case effKick:
			if e.from.room == r.room {
				e.from.disconnect()
			}
		case effSimError:
			out = append(out, r.match.tellConnected(r.room, "LEFT")...)
			r.room.endMatch()
			out = append(out, r.room.broadcast(r.room.rosterMessage())...)
		case effGarbage:
			if e.to.room == r.room {
				out = append(out, outgoing{e.to, fmt.Sprintf("GARB %d %d", e.id, e.lines)})
			}
		case effSnap:
			msg := "SNAP " + strconv.Itoa(r.players[e.from].id) + " " + e.text
			for _, q := range r.match.players {
				if q != e.from && q.room == r.room {
					out = append(out, outgoing{q, msg})
				}
			}
		case effResync:
			if e.from.room == r.room {
				out = append(out, outgoing{e.from, e.text})
			}
		}
	}
	enqueue(out)
}

// finish logs each participant's claimed and simulated totals and closes the simulation.
func (r *matchRunner) finish() {
	for _, q := range r.match.players {
		ps := r.players[q]
		log.Printf("authority room=%s player=%d claimedSent=%d simAttack=%d claimedDead=%d simTopOuts=%d wouldResync=%v",
			r.room.code, ps.id, ps.claimedSent, ps.simAttack, ps.claimedDead, ps.simTopOuts, ps.wouldResync)
	}
	if err := r.sim.Close(); err != nil {
		log.Printf("authority room=%s seed=%d: close: %v", r.room.code, r.seed, err)
	}
}

// wallMs is match time on the server's clock: ms since the countdown ended, negative before.
func (r *matchRunner) wallMs(now time.Time) int { return int(now.Sub(r.wall0) / time.Millisecond) }

// mark records a broken timing or shape rule: the player needs a RESYNC once its next IN arrives.
func (r *matchRunner) mark(ps *playerSim, rule string) {
	ps.needResync = true
	ps.sawINSinceMark = false
	ps.wouldResync[rule]++
	if r.auth.mode == "log" && ps.wouldResync[rule] == 1 {
		log.Printf("would-resync:%s room=%s player=%d t=%d", rule, r.room.code, ps.id, ps.t)
	}
}

// fail logs the first simulation error of the match and stops simulating it. In enforce mode it also ends
// the match: the server never falls back to trusting clients.
func (r *matchRunner) fail(ps *playerSim, err error) {
	if r.failed {
		return
	}
	r.failed = true
	if r.auth.mode != "enforce" {
		log.Printf("authority room=%s seed=%d player=%d err=%v", r.room.code, r.seed, ps.id, err)
		return
	}
	log.Printf("sim error room=%s seed=%d player=%d: %v", r.room.code, r.seed, ps.id, err)
	r.effects = append(r.effects, runnerEffect{kind: effSimError, from: ps.p})
}

// ensureGame creates ps's game with soft-drop factor sdf unless it exists; false once the simulation failed.
func (r *matchRunner) ensureGame(ps *playerSim, sdf int) bool {
	if ps.started {
		return !r.failed
	}
	if r.failed {
		return false
	}
	h, err := r.sim.New(r.seed, r.level, len(r.match.players), sdf)
	if err != nil {
		r.fail(ps, err)
		return false
	}
	ps.h, ps.started = h, true
	return true
}

// inEntry is one "<t>:<token>" of an IN.
type inEntry struct {
	t                 int
	token             string
	code, gid, glines int
}

// parseIN decodes "IN <tEnd> [<t>:<token> ...]" as the client's decoder does: digit-only numbers, at most
// maxInputEvents entries, t never decreasing and at most tEnd.
func parseIN(text string) (tEnd int, entries []inEntry, ok bool) {
	parts := strings.Split(text, " ")
	if len(parts) < 2 || parts[0] != "IN" || len(parts)-2 > maxInputEvents {
		return 0, nil, false
	}
	tEnd, ok = parseCount(parts[1])
	if !ok {
		return 0, nil, false
	}
	last := 0
	for _, part := range parts[2:] {
		tText, token, found := strings.Cut(part, ":")
		t, ok := parseCount(tText)
		if !found || !ok || t < last || t > tEnd {
			return 0, nil, false
		}
		code, gid, glines, ok := sim.ParseToken(token)
		if !ok {
			return 0, nil, false
		}
		last = t
		entries = append(entries, inEntry{t: t, token: token, code: code, gid: gid, glines: glines})
	}
	return tEnd, entries, true
}

// parseCount reads a non-negative int32 written in ASCII digits only.
func parseCount(s string) (int, bool) {
	if s == "" {
		return 0, false
	}
	n := 0
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, false
		}
		n = n*10 + int(s[i]-'0')
		if n > math.MaxInt32 {
			return 0, false
		}
	}
	return n, true
}

// handleSDF creates ps's game with its soft-drop factor. Only one SDF is allowed, before any entry has changed
// the game: one that arrives after the horizon started the game with the default rebuilds it.
func (r *matchRunner) handleSDF(ps *playerSim, text string) {
	arg, found := strings.CutPrefix(text, "SDF ")
	n, ok := parseCount(arg)
	if !found || !ok || (n != 0 && (n < minSdf || n > maxSdf)) || ps.sdfSeen || ps.changed {
		r.mark(ps, "bad-in")
		return
	}
	ps.sdfSeen = true
	if ps.started {
		r.rebuildGame(ps, n)
		return
	}
	r.ensureGame(ps, n)
}

// rebuildGame replaces ps's game, which no entry has changed, with one at soft-drop factor sdf advanced to the
// same t. Soft drop does nothing before an S+, so the new game is the one the client has been playing. Free
// releases the old game's handle.
func (r *matchRunner) rebuildGame(ps *playerSim, sdf int) {
	if r.failed {
		return
	}
	if err := r.sim.Free(ps.h); err != nil {
		r.fail(ps, err)
		return
	}
	h, err := r.sim.New(r.seed, r.level, len(r.match.players), sdf)
	if err != nil {
		r.fail(ps, err)
		return
	}
	ps.h = h
	if ps.t > 0 {
		if err := r.sim.Advance(h, ps.t); err != nil {
			r.fail(ps, err)
		}
	}
}

// handleIN applies an IN: the timing and shape rules of the spec's section 3, then each entry at its t.
func (r *matchRunner) handleIN(ps *playerSim, text string) {
	if r.failed {
		return
	}
	tEnd, entries, ok := parseIN(text)
	lead := int(r.auth.lead / time.Millisecond)
	if ok && (tEnd < ps.reportedEnd || tEnd > r.wallMs(r.now())+lead ||
		len(entries) > 0 && entries[0].t < ps.reportedEnd || ps.accepted+len(entries) > tEnd/4+entryAllowanceBase) {
		ok = false
	}
	if !ok {
		r.mark(ps, "bad-in")
		ps.sawINSinceMark = true // still this player's next IN, so its RESYNC goes out on the next tick
		return
	}
	if !r.ensureGame(ps, defaultSdf) {
		return
	}
	for _, e := range entries {
		if e.t < ps.t {
			r.mark(ps, "backdated")
			continue
		}
		r.advanceTo(ps, e.t)
		if r.failed {
			return
		}
		var err error
		if e.code < 0 {
			w, found := ps.pending[e.gid]
			if !found || e.t < w.t0 {
				r.mark(ps, "garbage-bad")
				continue
			}
			delete(ps.pending, e.gid)
			err = r.sim.AcceptGarbage(ps.h, e.gid, w.lines)
			r.record(ps, fmt.Sprintf("%d:G%dx%d", e.t, e.gid, w.lines))
		} else {
			err = r.sim.Input(ps.h, e.code)
			r.record(ps, fmt.Sprintf("%d:%s", e.t, e.token))
		}
		ps.accepted++
		if err != nil {
			r.fail(ps, err)
			return
		}
	}
	r.advanceTo(ps, tEnd)
	ps.reportedEnd = tEnd
	ps.sawINSinceMark = true
}

// advanceTo steps ps's game to target, first applying each garbage batch whose window closes before target
// at the window's end, then collects the simulation's attack and top-outs.
func (r *matchRunner) advanceTo(ps *playerSim, target int) {
	window := int(r.auth.garbageWindow / time.Millisecond)
	type due struct{ id, lines, t0 int }
	var late []due
	for id, w := range ps.pending {
		if w.t0+window < target {
			late = append(late, due{id, w.lines, w.t0})
		}
	}
	slices.SortFunc(late, func(a, b due) int { return cmp.Or(cmp.Compare(a.t0, b.t0), cmp.Compare(a.id, b.id)) })
	for _, g := range late {
		at := max(g.t0+window, ps.t)
		if !r.step(ps, at) {
			return
		}
		delete(ps.pending, g.id)
		if err := r.sim.AcceptGarbage(ps.h, g.id, g.lines); err != nil {
			r.fail(ps, err)
			return
		}
		r.record(ps, fmt.Sprintf("%d:G%dx%d", at, g.id, g.lines))
		r.mark(ps, "garbage-late")
	}
	if !r.step(ps, target) {
		return
	}
	attack, err := r.sim.TakeAttack(ps.h)
	if err != nil {
		r.fail(ps, err)
		return
	}
	if attack > 0 {
		ps.simAttack += attack
		r.effects = append(r.effects, runnerEffect{kind: effAttack, from: ps.p, lines: attack})
	}
	topOuts, err := r.sim.TopOuts(ps.h)
	if err != nil {
		r.fail(ps, err)
		return
	}
	for ; ps.simTopOuts < topOuts; ps.simTopOuts++ {
		r.effects = append(r.effects, runnerEffect{kind: effTopOut, from: ps.p})
	}
	eliminated, err := r.sim.Eliminated(ps.h)
	if err != nil {
		r.fail(ps, err)
		return
	}
	if eliminated && !ps.eliminated {
		ps.eliminated = true
		r.effects = append(r.effects, runnerEffect{kind: effEliminated, from: ps.p})
	}
}

// record notes an entry applied to ps's game; enforce mode keeps it in the canonical log for RESYNC.
func (r *matchRunner) record(ps *playerSim, entry string) {
	ps.changed = true
	if r.auth.mode == "enforce" {
		ps.canonical = append(ps.canonical, entry)
	}
}

// step advances ps's game to t if it is behind; false once the simulation failed.
func (r *matchRunner) step(ps *playerSim, t int) bool {
	if t > ps.t {
		if err := r.sim.Advance(ps.h, t-ps.t); err != nil {
			r.fail(ps, err)
			return false
		}
		ps.t = t
	}
	return true
}
