package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"tetris-server/sim"
)

// fakeSim is a scripted gameSim: it records every call that changes a game and returns the attacks and
// top-outs a test sets per handle.
type fakeSim struct {
	calls      []string
	handles    int
	attacks    map[int][]int // queued TakeAttack results, oldest first
	topOuts    map[int]int
	eliminated map[int]bool
	snaps      map[int]string // Snapshot results; "snap" when unset
	advanceErr error          // returned by every Advance once set
	closed     bool
}

func newFakeSim() *fakeSim {
	return &fakeSim{attacks: map[int][]int{}, topOuts: map[int]int{}, eliminated: map[int]bool{}, snaps: map[int]string{}}
}

func (f *fakeSim) record(format string, a ...any) {
	f.calls = append(f.calls, fmt.Sprintf(format, a...))
}

func (f *fakeSim) New(seed uint32, level, players, sdf int) (int, error) {
	f.record("New(%d,%d,%d,%d)", seed, level, players, sdf)
	f.handles++
	return f.handles - 1, nil
}
func (f *fakeSim) Free(h int) error        { f.record("Free(%d)", h); return nil }
func (f *fakeSim) Input(h, code int) error { f.record("Input(%d,%d)", h, code); return nil }
func (f *fakeSim) AcceptGarbage(h, id, lines int) error {
	f.record("AcceptGarbage(%d,%d,%d)", h, id, lines)
	return nil
}
func (f *fakeSim) Advance(h, ms int) error {
	f.record("Advance(%d,%d)", h, ms)
	return f.advanceErr
}
func (f *fakeSim) T(h int) (int, error)           { return 0, nil }
func (f *fakeSim) TopOuts(h int) (int, error)     { return f.topOuts[h], nil }
func (f *fakeSim) Eliminated(h int) (bool, error) { return f.eliminated[h], nil }
func (f *fakeSim) Hash(h int) (uint64, error)     { return 0, nil }
func (f *fakeSim) Close() error                   { f.closed = true; return nil }

func (f *fakeSim) Snapshot(h int) (string, error) {
	if s, ok := f.snaps[h]; ok {
		return s, nil
	}
	return "snap", nil
}

func (f *fakeSim) TakeAttack(h int) (int, error) {
	if len(f.attacks[h]) == 0 {
		return 0, nil
	}
	n := f.attacks[h][0]
	f.attacks[h] = f.attacks[h][1:]
	return n, nil
}

// queuedMsgs is the number of messages waiting in r's inbox.
func queuedMsgs(r *matchRunner) int {
	r.inMu.Lock()
	defer r.inMu.Unlock()
	return len(r.inbox)
}

// steps lists the recorded calls for handle h, without New.
func (f *fakeSim) steps(h int) []string {
	var out []string
	for _, c := range f.calls {
		if !strings.HasPrefix(c, "New(") && (strings.Contains(c, fmt.Sprintf("(%d,", h)) || c == fmt.Sprintf("Free(%d)", h)) {
			out = append(out, c)
		}
	}
	return out
}

// directTest is a hub driven directly: a scripted simulation per match, runners the test drains and ticks,
// a settable clock and fake match timers.
type directTest struct {
	h     *Hub
	ps    []*player
	sims  []*fakeSim
	clock *fakeClock
	now   time.Time
	wall0 time.Time
}

// directMatch puts the named players in room AAAA at level 1 with IDs 1, 2, ... and has the host begin,
// emptying their queues.
func directMatch(t *testing.T, names ...string) *directTest {
	t.Helper()
	return directMatchWith(t, func(*Hub) {}, names...)
}

// directMatchWith is directMatch with configure applied to the hub before the first message.
func directMatchWith(t *testing.T, configure func(*Hub), names ...string) *directTest {
	t.Helper()
	d := &directTest{clock: &fakeClock{}, now: time.Unix(1000, 0)}
	h := NewHub()
	h.newCode = func() string { return "AAAA" }
	seed := uint32(100)
	h.newSeed = func() uint32 { seed++; return seed }
	h.pickTarget = func(int) int { return 0 }
	h.afterFunc = d.clock.afterFunc
	h.manualRunners = true
	h.now = func() time.Time { return d.now }
	h.newSim = func() (gameSim, error) {
		f := newFakeSim()
		d.sims = append(d.sims, f)
		return f, nil
	}
	h.auth = authority{mode: "log", lead: 250 * time.Millisecond, lag: time.Second,
		garbageWindow: 750 * time.Millisecond, maxResyncs: 10}
	configure(h)
	d.h = h
	for _, name := range names {
		d.ps = append(d.ps, testPlayer(name))
	}
	h.receive(d.ps[0], "CREATE 1")
	for _, p := range d.ps[1:] {
		h.receive(p, "JOIN AAAA")
	}
	h.receive(d.ps[0], "BEGIN")
	for _, p := range d.ps {
		if got := queued(p); len(got) == 0 || !strings.HasPrefix(got[len(got)-1], "PLAYERS ") {
			t.Fatalf("setup queued %q", got)
		}
	}
	d.wall0 = d.now.Add(countdown)
	return d
}

// runner is the current match's runner.
func (d *directTest) runner() *matchRunner { return d.h.rooms["AAAA"].match.runner }

// at sets the clock to ms after the countdown ends.
func (d *directTest) at(ms int) { d.now = d.wall0.Add(time.Duration(ms) * time.Millisecond) }

// sim is the current match's scripted simulation.
func (d *directTest) sim() *fakeSim { return d.sims[len(d.sims)-1] }

func (d *directTest) expectQueues(t *testing.T, want ...[]string) {
	t.Helper()
	for i, p := range d.ps {
		if got := queued(p); !slices.Equal(got, want[i]) {
			t.Errorf("%s got %q, want %q", p.name, got, want[i])
		}
	}
}

// logBuffer collects log output; log writes from runner goroutines of other tests may land in it too.
type logBuffer struct {
	mu sync.Mutex
	b  strings.Builder
}

func (l *logBuffer) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.Write(p)
}

func (l *logBuffer) String() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.String()
}

func captureLog(t *testing.T) *logBuffer {
	t.Helper()
	buf := &logBuffer{}
	prev := log.Writer()
	log.SetOutput(buf)
	t.Cleanup(func() { log.SetOutput(prev) })
	return buf
}

func TestLogModeRelaysOldTrafficUnchanged(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	a, b := d.ps[0], d.ps[1]
	r := d.runner()
	d.h.receive(a, "ATK 41") // malformed: dropped as before
	d.h.receive(a, "ATK 2")
	d.expectQueues(t, nil, nil) // the GARB comes from the runner
	r.drain(d.h)
	d.expectQueues(t, nil, []string{"GARB 1 2"})
	d.h.receive(b, "SNAP 0 0 0 board")
	d.expectQueues(t, []string{"SNAP 2 0 0 0 board"}, nil)
	d.h.receive(b, "DEAD")
	d.expectQueues(t, []string{"KO 1 0"}, []string{"KO 0 1"})
	d.h.receive(b, "ATK 1")
	d.h.receive(a, "ATK 3")
	r.drain(d.h)
	d.expectQueues(t, []string{"GARB 1 1"}, []string{"GARB 2 3"})
	d.clock.timer(0).fire()
	d.expectQueues(t,
		[]string{"OVER WIN TIME 5 1", "PLAYERS 1 1:Ann:0 2:Bob:0"},
		[]string{"OVER LOSE TIME 1 5", "PLAYERS 1 1:Ann:0 2:Bob:0"})
	if got := r.players[a].claimedSent; got != 5 {
		t.Errorf("Ann claimedSent = %d, want 5", got)
	}
	if got := r.players[b].claimedDead; got != 1 {
		t.Errorf("Bob claimedDead = %d, want 1", got)
	}
}

func TestLogModeRoyaleGarbageGoesToThePickedTarget(t *testing.T) {
	d := directMatch(t, "Ann", "Bob", "Cat")
	r := d.runner()
	d.h.receive(d.ps[0], "ATK 2") // pickTarget 0 of [Bob Cat]
	d.h.receive(d.ps[2], "ATK 1") // pickTarget 0 of [Ann Bob]
	r.drain(d.h)
	d.expectQueues(t, []string{"GARB 1 1"}, []string{"GARB 1 2"}, nil)
	d.h.receive(d.ps[1], "DEAD")
	d.expectQueues(t, []string{"OUT 2 3"}, []string{"OUT 2 3"}, []string{"OUT 2 3"})
	d.h.receive(d.ps[0], "ATK 4") // Cat is the only living opponent
	r.drain(d.h)
	d.expectQueues(t, nil, nil, []string{"GARB 1 4"})
}

func TestINStepsSimToEachEventThenTEnd(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	d.at(1000)
	d.h.receive(d.ps[0], "IN 120 10:L 50:HD")
	d.h.receive(d.ps[1], "SDF 7")
	d.runner().drain(d.h)
	f := d.sim()
	want := []string{"New(101,1,2,20)", "Advance(0,10)", "Input(0,0)", "Advance(0,40)", "Input(0,4)", "Advance(0,70)"}
	if got := append([]string{f.calls[0]}, f.steps(0)...); !slices.Equal(got, want) {
		t.Errorf("calls %q, want %q", got, want)
	}
	if !slices.Contains(f.calls, "New(101,1,2,7)") {
		t.Errorf("SDF 7 did not create Bob's game: %q", f.calls)
	}
	ps := d.runner().players[d.ps[0]]
	if ps.t != 120 || ps.reportedEnd != 120 || ps.accepted != 2 || ps.canonical != nil { // log mode keeps no log
		t.Errorf("t=%d reportedEnd=%d accepted=%d canonical=%q", ps.t, ps.reportedEnd, ps.accepted, ps.canonical)
	}
	d.expectQueues(t, nil, nil)
}

func TestRunningAheadIsRejected(t *testing.T) {
	logs := captureLog(t)
	d := directMatch(t, "Ann", "Bob")
	d.at(1000)
	d.h.receive(d.ps[0], "IN 1300")
	d.runner().drain(d.h)
	for _, c := range d.sim().calls {
		if strings.HasPrefix(c, "Advance(") {
			t.Errorf("unexpected %s", c)
		}
	}
	if !strings.Contains(logs.String(), "would-resync:bad-in room=AAAA player=1 t=0") {
		t.Errorf("log %q lacks would-resync:bad-in", logs.String())
	}
	d.h.receive(d.ps[0], "IN 1250") // exactly wall + lead is allowed
	d.runner().drain(d.h)
	if got := d.sim().steps(0); !slices.Equal(got, []string{"Advance(0,1250)"}) {
		t.Errorf("steps %q", got)
	}
}

func TestHorizonForcesAdvanceAndDropsBackdated(t *testing.T) {
	logs := captureLog(t)
	d := directMatch(t, "Ann", "Bob")
	r := d.runner()
	d.at(3000)
	r.tick(d.h, d.now)
	f := d.sim()
	if got := f.steps(0); !slices.Equal(got, []string{"Advance(0,2000)"}) {
		t.Errorf("after tick, steps %q, want [Advance(0,2000)]", got)
	}
	ps := r.players[d.ps[0]]
	if ps.needResync || len(ps.wouldResync) != 0 {
		t.Errorf("a forced advance marked the player: %v", ps.wouldResync)
	}
	d.h.receive(d.ps[0], "IN 2100 1500:L")
	r.drain(d.h)
	if got := f.steps(0); !slices.Equal(got, []string{"Advance(0,2000)", "Advance(0,100)"}) {
		t.Errorf("steps %q, want the L dropped and an advance to 2100", got)
	}
	if ps.t != 2100 || ps.wouldResync["backdated"] != 1 || len(ps.canonical) != 0 {
		t.Errorf("t=%d wouldResync=%v canonical=%q", ps.t, ps.wouldResync, ps.canonical)
	}
	if !strings.Contains(logs.String(), "would-resync:backdated room=AAAA player=1 t=2000") {
		t.Errorf("log %q lacks would-resync:backdated", logs.String())
	}
}

func TestGarbageWindow(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	a, b := d.ps[0], d.ps[1]
	r := d.runner()
	d.at(10000)
	d.h.receive(b, "IN 1000") // Bob's game is handle 0
	r.drain(d.h)
	ps := r.players[b]
	f := d.sim()

	d.h.receive(a, "ATK 2") // GARB 1 at t0 = 1000
	r.drain(d.h)
	d.h.receive(b, "IN 1200 1100:G1x5") // t0+100: accepted with the server's 2 lines
	r.drain(d.h)

	d.h.receive(a, "ATK 3") // GARB 2 at t0 = 1200
	r.drain(d.h)
	d.h.receive(b, "IN 2100 2000:G2x3") // t0+800: forced at t0+750 on the way, then unknown
	r.drain(d.h)
	if ps.wouldResync["garbage-late"] != 1 || ps.wouldResync["garbage-bad"] != 1 {
		t.Errorf("after t0+800, wouldResync=%v", ps.wouldResync)
	}

	r.tick(d.h, d.now)      // the horizon moves Bob to 9000, past his last tEnd
	d.h.receive(a, "ATK 1") // GARB 3 at t0 = 9000
	r.drain(d.h)
	d.h.receive(b, "IN 9100 8999:G3x1") // t0-1: dropped
	r.drain(d.h)
	if _, ok := ps.pending[3]; !ok || ps.wouldResync["backdated"] != 1 {
		t.Errorf("G3 at t0-1 was not dropped: pending=%v wouldResync=%v", ps.pending, ps.wouldResync)
	}
	d.h.receive(b, "IN 9200 9100:G3x1 9150:G9x1") // G3 at t0+100 is accepted; id 9 is unknown
	r.drain(d.h)

	want := []string{"Advance(0,1000)", "Advance(0,100)", "AcceptGarbage(0,1,2)", "Advance(0,100)",
		"Advance(0,750)", "AcceptGarbage(0,2,3)", "Advance(0,50)", "Advance(0,100)",
		"Advance(0,6900)", "Advance(0,100)", "AcceptGarbage(0,3,1)", "Advance(0,50)", "Advance(0,50)"}
	if got := f.steps(0); !slices.Equal(got, want) {
		t.Errorf("steps\n %q\nwant\n %q", got, want)
	}
	if ps.accepted != 2 || ps.canonical != nil { // G1 and G3; G2 was forced. Log mode keeps no log.
		t.Errorf("accepted %d, canonical %q", ps.accepted, ps.canonical)
	}
	if ps.wouldResync["garbage-bad"] != 2 || len(ps.pending) != 0 {
		t.Errorf("wouldResync=%v pending=%v", ps.wouldResync, ps.pending)
	}
	d.expectQueues(t, nil, []string{"GARB 1 2", "GARB 2 3", "GARB 3 1"})
}

// A G stamped exactly t0+window is still in time.
func TestGarbageWindowEdgeIsAccepted(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	r := d.runner()
	d.at(10000)
	d.h.receive(d.ps[0], "ATK 2") // GARB 1 at t0 = 0
	r.drain(d.h)
	d.h.receive(d.ps[1], "IN 800 750:G1x2")
	r.drain(d.h)
	if got, want := d.sim().steps(0), []string{"Advance(0,750)", "AcceptGarbage(0,1,2)", "Advance(0,50)"}; !slices.Equal(got, want) {
		t.Errorf("steps %q, want %q", got, want)
	}
	if ps := r.players[d.ps[1]]; len(ps.wouldResync) != 0 {
		t.Errorf("wouldResync=%v", ps.wouldResync)
	}
}

func TestBadINShapes(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	r := d.runner()
	d.at(10000)
	a := d.ps[0]
	d.h.receive(a, "IN 500")
	r.drain(d.h)
	tooMany := "IN 600" + strings.Repeat(" 550:L", maxInputEvents+1)
	bad := []string{
		tooMany,
		"IN 600 560:L 550:R", // decreasing t
		"IN 600 650:L",       // beyond tEnd
		"IN 400",             // tEnd below the previous tEnd
		"IN 600 499:L",       // before the previous tEnd
		"IN", "IN x", "IN 600 550:Q", "IN 600 550:L ", "IN -600", "IN 600 +550:L", "IN 600 550",
	}
	for _, text := range bad {
		d.h.receive(a, text)
	}
	r.drain(d.h)
	ps := r.players[a]
	if got := d.sim().steps(0); !slices.Equal(got, []string{"Advance(0,500)"}) {
		t.Errorf("steps %q: a bad IN was not dropped whole", got)
	}
	if ps.wouldResync["bad-in"] != len(bad) || ps.reportedEnd != 500 || !ps.sawINSinceMark {
		t.Errorf("wouldResync=%v reportedEnd=%d sawINSinceMark=%v", ps.wouldResync, ps.reportedEnd, ps.sawINSinceMark)
	}
	ok := "IN 600" + strings.Repeat(" 550:L", maxInputEvents)
	d.h.receive(a, ok)
	r.drain(d.h)
	if ps.reportedEnd != 600 || ps.accepted != maxInputEvents {
		t.Errorf("64 entries: reportedEnd=%d, %d entries", ps.reportedEnd, ps.accepted)
	}
}

func TestBadINShapesIncludeBadSDF(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	r := d.runner()
	for _, text := range []string{"SDF 4", "SDF 41", "SDF", "SDF x", "SDF 10 10"} {
		d.h.receive(d.ps[0], text)
	}
	d.h.receive(d.ps[1], "SDF 0")
	d.h.receive(d.ps[1], "SDF 5") // a second SDF
	r.drain(d.h)
	if got := r.players[d.ps[0]].wouldResync["bad-in"]; got != 5 {
		t.Errorf("Ann bad-in = %d, want 5", got)
	}
	if got := r.players[d.ps[1]].wouldResync["bad-in"]; got != 1 {
		t.Errorf("Bob bad-in = %d, want 1", got)
	}
	if got := d.sim().calls; !slices.Equal(got, []string{"New(101,1,2,0)"}) {
		t.Errorf("calls %q", got)
	}
}

// Rule 4's ceiling: an IN that would take the player past tEnd/4 + 256 accepted entries is dropped whole.
func TestINEntryCeiling(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	r := d.runner()
	a := d.ps[0]
	d.at(10000)
	full := "IN 0" + strings.Repeat(" 0:L", maxInputEvents)
	for range 4 {
		d.h.receive(a, full) // 256 entries at tEnd 0: exactly the ceiling
	}
	d.h.receive(a, "IN 3 3:L") // 257 > 3/4 + 256
	d.h.receive(a, "IN 4 4:L") // 257 = 4/4 + 256
	r.drain(d.h)
	ps := r.players[a]
	if ps.accepted != 257 || ps.wouldResync["bad-in"] != 1 || ps.reportedEnd != 4 {
		t.Errorf("accepted=%d wouldResync=%v reportedEnd=%d", ps.accepted, ps.wouldResync, ps.reportedEnd)
	}
	if ps.canonical != nil {
		t.Errorf("log mode kept a canonical log of %d entries", len(ps.canonical))
	}
}

// Enforce mode keeps the canonical log for RESYNC: accepted entries, and forced garbage at the window's end.
func TestEnforceKeepsTheCanonicalLog(t *testing.T) {
	d := directMatchWith(t, enforce, "Ann", "Bob")
	r := d.runner()
	d.at(2000)
	d.sim().attacks[0] = []int{4}
	d.h.receive(d.ps[0], "IN 100") // Ann's game is handle 0; GARB 1 to Bob at t0 = 0
	r.drain(d.h)
	d.h.receive(d.ps[1], "IN 1000 900:L")
	r.drain(d.h)
	if got, want := r.players[d.ps[1]].canonical, []string{"750:G1x4", "900:L"}; !slices.Equal(got, want) {
		t.Errorf("canonical %q, want %q", got, want)
	}
	queued(d.ps[1])
}

// An SDF that arrives after the horizon started the game with the default still applies while no entry has
// changed the game: the game is rebuilt with it and advanced back to the server's t.
func TestLateSDFRebuildsTheGame(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	r := d.runner()
	a, b := d.ps[0], d.ps[1]
	d.h.receive(d.ps[0], "ATK 2") // GARB 1 to Bob at t0 = 0
	r.drain(d.h)
	d.at(3000)
	r.tick(d.h, d.now) // Ann's game is handle 0, Bob's handle 1, both at 2000; Bob's G1 forced at 750
	f := d.sim()
	d.h.receive(a, "SDF 7")
	d.h.receive(b, "SDF 7") // forced garbage changed Bob's game: too late
	r.drain(d.h)
	psA, psB := r.players[a], r.players[b]
	if got, want := f.steps(0), []string{"Advance(0,2000)", "Free(0)"}; !slices.Equal(got, want) {
		t.Errorf("handle 0 steps %q, want %q", got, want)
	}
	if !slices.Contains(f.calls, "New(101,1,2,7)") || psA.h != 2 || psA.t != 2000 || len(psA.wouldResync) != 0 {
		t.Errorf("calls %q h=%d t=%d wouldResync=%v", f.calls, psA.h, psA.t, psA.wouldResync)
	}
	if got, want := f.steps(2), []string{"Advance(2,2000)"}; !slices.Equal(got, want) {
		t.Errorf("rebuilt game steps %q, want %q", got, want)
	}
	if psB.wouldResync["bad-in"] != 1 || psB.h != 1 {
		t.Errorf("Bob wouldResync=%v h=%d", psB.wouldResync, psB.h)
	}
	d.h.receive(a, "SDF 9") // a second SDF
	d.h.receive(a, "IN 2100 2050:L")
	r.drain(d.h)
	if got, want := f.steps(2), []string{"Advance(2,2000)", "Advance(2,50)", "Input(2,0)", "Advance(2,50)"}; !slices.Equal(got, want) {
		t.Errorf("rebuilt game steps %q, want %q", got, want)
	}
	if psA.wouldResync["bad-in"] != 1 {
		t.Errorf("Ann wouldResync=%v", psA.wouldResync)
	}
	d.h.receive(a, "SDF 7") // after an accepted entry, too
	r.drain(d.h)
	if psA.wouldResync["bad-in"] != 2 {
		t.Errorf("Ann wouldResync=%v", psA.wouldResync)
	}
}

func TestSummaryLoggedAtMatchEnd(t *testing.T) {
	logs := captureLog(t)
	d := directMatch(t, "Ann", "Bob")
	a := d.ps[0]
	r := d.runner()
	f := d.sim()
	d.at(1000)
	f.attacks[0] = []int{3}
	d.h.receive(a, "IN 100 50:HD")
	d.h.receive(a, "ATK 2")
	d.h.receive(a, "DEAD")
	r.drain(d.h)
	f.topOuts[0] = 1
	d.h.receive(a, "IN 200")
	d.h.receive(a, "IN 5000") // running ahead
	r.drain(d.h)
	d.clock.timer(0).fire()
	if d.h.rooms["AAAA"].match != nil {
		t.Fatal("the match did not end")
	}
	r.finish()
	for _, want := range []string{
		"authority room=AAAA player=1 claimedSent=2 simAttack=3 claimedDead=1 simTopOuts=1 wouldResync=map[bad-in:1]",
		"authority room=AAAA player=2 claimedSent=0 simAttack=0 claimedDead=0 simTopOuts=0 wouldResync=map[]",
	} {
		if !strings.Contains(logs.String(), want) {
			t.Errorf("log %q lacks %q", logs.String(), want)
		}
	}
	if !f.closed {
		t.Error("finish did not close the simulation")
	}
}

func TestRunnerOutputsForEndedMatchAreDropped(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	a, b := d.ps[0], d.ps[1]
	old := d.runner()
	d.at(1000)
	d.h.receive(a, "ATK 2")
	d.h.receive(b, "IN 100 50:L")
	d.clock.timer(0).fire()
	select {
	case <-old.stop:
	default:
		t.Error("ending the match did not stop its runner")
	}
	d.h.receive(a, "BEGIN")
	for _, p := range d.ps {
		if got := queued(p); len(got) == 0 || got[len(got)-1] != "PLAYERS 1 1:Ann:1 2:Bob:1" {
			t.Fatalf("%s queued %q", p.name, got)
		}
	}
	if d.runner() == old || d.runner() == nil {
		t.Fatal("the new match has no runner of its own")
	}
	old.drain(d.h)
	d.expectQueues(t, nil, nil)
	if !slices.Contains(d.sims[0].calls, "Input(0,0)") {
		t.Errorf("the old runner did not simulate the queued IN: %q", d.sims[0].calls)
	}
}

func TestRunnerChannelFullMarksBadIN(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	r := d.runner()
	d.at(10000)
	for i := range runnerQueuePerPlayer + 3 {
		d.h.receive(d.ps[0], fmt.Sprintf("IN %d", i))
	}
	if got := r.players[d.ps[0]].dropped.Load(); got != 3 {
		t.Errorf("dropped %d, want 3", got)
	}
	r.drain(d.h)
	if got := r.players[d.ps[0]].wouldResync["bad-in"]; got != 1 {
		t.Errorf("bad-in = %d, want 1", got)
	}
}

// One player's flood fills only that player's queue: another player's IN and every claimed ATK still arrive.
func TestOneFloodDoesNotDropOthers(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	r := d.runner()
	a, b := d.ps[0], d.ps[1]
	d.at(10000)
	for i := range 3 * runnerQueuePerPlayer {
		d.h.receive(a, fmt.Sprintf("IN %d", i))
	}
	d.h.receive(a, "ATK 2")
	d.h.receive(b, "IN 100 50:L")
	d.h.receive(b, "ATK 3")
	if got := r.players[a].dropped.Load(); got != 2*runnerQueuePerPlayer {
		t.Errorf("Ann dropped %d, want %d", got, 2*runnerQueuePerPlayer)
	}
	r.drain(d.h)
	psA, psB := r.players[a], r.players[b]
	if psA.wouldResync["bad-in"] != 1 || psA.reportedEnd != runnerQueuePerPlayer-1 {
		t.Errorf("Ann wouldResync=%v reportedEnd=%d", psA.wouldResync, psA.reportedEnd)
	}
	if len(psB.wouldResync) != 0 || psB.reportedEnd != 100 || psB.accepted != 1 {
		t.Errorf("Bob wouldResync=%v reportedEnd=%d accepted=%d", psB.wouldResync, psB.reportedEnd, psB.accepted)
	}
	if psA.claimedSent != 2 || psB.claimedSent != 3 {
		t.Errorf("claimedSent Ann=%d Bob=%d", psA.claimedSent, psB.claimedSent)
	}
	d.expectQueues(t, []string{"GARB 1 3"}, []string{"GARB 1 2"})
	// The cap is per message waiting: once the runner has handled them, Ann may queue again.
	d.h.receive(a, "IN 5000")
	if got := r.players[a].dropped.Load(); got != 0 {
		t.Errorf("after the drain, dropped %d", got)
	}
}

// A leaving player's message is never dropped, however full the queues are.
func TestFloodDoesNotDropLeft(t *testing.T) {
	d := directMatch(t, "Ann", "Bob", "Cat")
	r := d.runner()
	d.at(10000)
	for _, p := range d.ps {
		for i := range 2 * runnerQueuePerPlayer {
			d.h.receive(p, fmt.Sprintf("IN %d", i))
		}
	}
	d.h.leave(d.ps[2])
	r.drain(d.h)
	if !r.players[d.ps[2]].left {
		t.Error("Cat's leaving was dropped")
	}
}

func TestWarmEngine(t *testing.T) {
	failing := func() (gameSim, error) { return nil, errors.New("no engine") }
	if err := warmEngine("enforce", failing); err == nil {
		t.Error("enforce mode started without a game core")
	}
	if err := warmEngine("log", failing); err != nil {
		t.Errorf("log mode: %v", err)
	}
	f := newFakeSim()
	if err := warmEngine("enforce", func() (gameSim, error) { return f, nil }); err != nil || !f.closed {
		t.Errorf("err=%v closed=%v", err, f.closed)
	}
}

func TestRunnerIgnoresNonParticipants(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	r := d.runner()
	d.at(1000)
	cat := testPlayer("Cat")
	d.h.receive(cat, "JOIN AAAA")
	queued(cat)
	d.h.receive(cat, "IN 100 50:L")
	d.h.receive(cat, "SDF 10")
	if n := queuedMsgs(r); n != 0 {
		t.Errorf("%d messages queued for the runner from a non-participant", n)
	}
}

func TestRunnerFailureFallsBackToATK(t *testing.T) {
	srv, _ := newConfiguredHub(t, func(h *Hub) {
		h.newSim = func() (gameSim, error) { return nil, fmt.Errorf("no engine") }
	})
	a, b := pair(t, srv)
	a.send("IN 100 50:L")
	a.send("SDF 10")
	a.send("ATK 2")
	b.expect("ATK 2")
	a.flush()
}

func TestRunnerLeftStopsTheHorizon(t *testing.T) {
	d := directMatch(t, "Ann", "Bob", "Cat")
	r := d.runner()
	d.h.leave(d.ps[2])
	d.at(3000)
	r.drain(d.h)
	r.tick(d.h, d.now)
	f := d.sim()
	if got := f.steps(2); len(got) != 0 {
		t.Errorf("Cat left but was advanced: %q", got)
	}
	if got := f.steps(0); !slices.Equal(got, []string{"Advance(0,2000)"}) {
		t.Errorf("Ann steps %q", got)
	}
}

// The production simulation, driven by hand: an honest IN stream moves the game without a disagreement.
func TestINWithTheRealSimulation(t *testing.T) {
	d := directMatchWith(t, func(h *Hub) { h.newSim = newSharedSim }, "Ann", "Bob")
	r := d.runner()
	if _, fake := r.sim.(*fakeSim); fake {
		t.Fatal("the match did not get the real simulation")
	}
	d.at(2000)
	d.h.receive(d.ps[0], "SDF 10")
	d.h.receive(d.ps[0], "IN 500 100:L 200:HD 300:CW")
	d.h.receive(d.ps[0], "IN 1600 1000:HD")
	r.drain(d.h)
	ps := r.players[d.ps[0]]
	if r.failed || ps.t != 1600 || len(ps.wouldResync) != 0 {
		t.Errorf("failed=%v t=%d wouldResync=%v", r.failed, ps.t, ps.wouldResync)
	}
	if got, err := r.sim.T(ps.h); err != nil || got != 1600 {
		t.Errorf("sim t = %d, %v; want 1600", got, err)
	}
	r.finish()
}

// enforce configures a direct hub for -authority enforce.
func enforce(h *Hub) { h.auth.mode = "enforce" }

// withPrefix keeps the messages that start with prefix.
func withPrefix(msgs []string, prefix string) []string {
	return slices.DeleteFunc(msgs, func(m string) bool { return !strings.HasPrefix(m, prefix) })
}

func TestEnforceIgnoresForgedAttackAndDead(t *testing.T) {
	d := directMatchWith(t, enforce, "Ann", "Bob")
	r := d.runner()
	d.h.receive(d.ps[0], "ATK 40")
	d.h.receive(d.ps[1], "DEAD")
	d.h.receive(d.ps[1], "SNAP 0 0 0 board")
	r.drain(d.h)
	d.expectQueues(t, nil, nil)
	m := d.h.rooms["AAAA"].match
	if m == nil || m.sent[d.ps[0]] != 0 || m.kos[d.ps[0]] != 0 || queuedMsgs(r) != 0 {
		t.Errorf("a forged message changed the match: %+v", m)
	}

	d = directMatchWith(t, enforce, "Ann", "Bob", "Cat")
	d.h.receive(d.ps[0], "ATK 40")
	d.h.receive(d.ps[1], "DEAD")
	d.runner().drain(d.h)
	d.expectQueues(t, nil, nil, nil)
}

func TestEnforceRoutesSimAttackAsGarb(t *testing.T) {
	d := directMatchWith(t, enforce, "Ann", "Bob")
	r := d.runner()
	d.at(1000)
	d.sim().attacks[0] = []int{4}
	d.h.receive(d.ps[0], "IN 100") // Ann's game is handle 0
	r.drain(d.h)
	d.expectQueues(t, nil, []string{"GARB 1 4"})
	if got := d.h.rooms["AAAA"].match.sent[d.ps[0]]; got != 4 {
		t.Errorf("sent = %d, want 4", got)
	}
	if w, ok := r.players[d.ps[1]].pending[1]; !ok || w.lines != 4 {
		t.Errorf("Bob's pending %v", r.players[d.ps[1]].pending)
	}

	d = directMatchWith(t, func(h *Hub) {
		enforce(h)
		h.pickTarget = func(n int) int { return n - 1 }
	}, "Ann", "Bob", "Cat")
	d.at(1000)
	d.sim().attacks[0] = []int{4}
	d.h.receive(d.ps[0], "IN 100")
	d.runner().drain(d.h)
	d.expectQueues(t, nil, nil, []string{"GARB 1 4"})
}

// The client decodes at most 40 lines per GARB, so a bigger simulated attack goes out as several batches.
func TestEnforceSplitsGarbageAt40Lines(t *testing.T) {
	d := directMatchWith(t, enforce, "Ann", "Bob")
	r := d.runner()
	d.at(1000)
	d.sim().attacks[0] = []int{85}
	d.h.receive(d.ps[0], "IN 100")
	r.drain(d.h)
	d.expectQueues(t, nil, []string{"GARB 1 40", "GARB 2 40", "GARB 3 5"})
	pending := r.players[d.ps[1]].pending
	if len(pending) != 3 || pending[1].lines != 40 || pending[3].lines != 5 {
		t.Errorf("Bob's pending %v", pending)
	}
	if got := d.h.rooms["AAAA"].match.sent[d.ps[0]]; got != 85 {
		t.Errorf("sent = %d, want 85", got)
	}
}

// Ticks skip an eliminated player, so the board it died with goes out once at the elimination.
func TestEnforceSendsTheFinalSnapshotAtElimination(t *testing.T) {
	d := directMatchWith(t, enforce, "Ann", "Bob", "Cat")
	r := d.runner()
	f := d.sim()
	for _, p := range d.ps {
		d.h.receive(p, "SDF 20") // handles 0, 1, 2 in order
	}
	r.drain(d.h)
	d.at(100)
	r.tick(d.h, d.now)
	for _, p := range d.ps {
		queued(p) // the first SNAPs
	}
	f.snaps[1] = "topped"
	f.topOuts[1], f.eliminated[1] = 1, true
	d.h.receive(d.ps[1], "IN 150")
	r.drain(d.h)
	d.expectQueues(t, []string{"SNAP 2 topped", "OUT 2 3"}, []string{"OUT 2 3"}, []string{"SNAP 2 topped", "OUT 2 3"})
	for ms := 200; ms <= 1000; ms += 100 {
		d.at(ms)
		r.tick(d.h, d.now)
	}
	if got := withPrefix(queued(d.ps[0]), "SNAP 2"); len(got) != 0 {
		t.Errorf("Bob's snapshot again: %q", got)
	}
	// Unchanged since the last tick: nothing more.
	f.topOuts[2], f.eliminated[2] = 1, true
	d.h.receive(d.ps[2], "IN 1000")
	r.drain(d.h)
	if got := withPrefix(queued(d.ps[0]), "SNAP 3"); len(got) != 0 {
		t.Errorf("an unchanged final snapshot went out: %q", got)
	}
}

func TestEnforceSimTopOutScoresKOAndEliminates(t *testing.T) {
	d := directMatchWith(t, enforce, "Ann", "Bob")
	r := d.runner()
	f := d.sim()
	d.at(1000)
	f.attacks[0] = []int{2}
	f.topOuts[0] = 1
	d.h.receive(d.ps[0], "IN 100")
	r.drain(d.h)
	d.expectQueues(t, []string{"KO 0 1"}, []string{"GARB 1 2", "KO 1 0"})
	f.topOuts[0] = 3 // two more at once: the second wins the duel
	d.h.receive(d.ps[0], "IN 200")
	r.drain(d.h)
	d.expectQueues(t,
		[]string{"KO 0 2", "KO 0 3", "OVER LOSE KO 2 0", "PLAYERS 1 1:Ann:0 2:Bob:0"},
		[]string{"KO 2 0", "KO 3 0", "OVER WIN KO 0 2", "PLAYERS 1 1:Ann:0 2:Bob:0"})

	d = directMatchWith(t, enforce, "Ann", "Bob", "Cat")
	r = d.runner()
	f = d.sim()
	d.at(1000)
	for _, p := range d.ps {
		d.h.receive(p, "SDF 20") // handles 0, 1, 2 in order
	}
	f.topOuts[1], f.eliminated[1] = 1, true
	d.h.receive(d.ps[1], "IN 100")
	r.drain(d.h)
	// Bob's board was never sent, so his final snapshot goes out with his elimination.
	d.expectQueues(t, []string{"SNAP 2 snap", "OUT 2 3"}, []string{"OUT 2 3"}, []string{"SNAP 2 snap", "OUT 2 3"})
	if !r.players[d.ps[1]].eliminated {
		t.Error("Bob's game is not marked eliminated")
	}
	steps := f.steps(1)
	d.h.receive(d.ps[1], "IN 300 200:L")
	d.at(5000)
	r.drain(d.h)
	r.tick(d.h, d.now)
	if got := f.steps(1); !slices.Equal(got, steps) {
		t.Errorf("Bob's game moved after his elimination: %q", got[len(steps):])
	}
	for _, p := range d.ps {
		queued(p) // the tick's SNAPs
	}
	f.topOuts[2], f.eliminated[2] = 1, true
	d.h.receive(d.ps[2], "IN 100")
	r.drain(d.h)
	end := []string{"OUT 3 2", "OUT 1 1", "PLAYERS 1 1:Ann:0 2:Bob:0 3:Cat:0"}
	d.expectQueues(t, end, end, end)
	if d.h.rooms["AAAA"].match != nil {
		t.Error("the royale did not end")
	}
}

func TestEnforceSnapshotsFromSimOnlyWhenChanged(t *testing.T) {
	d := directMatchWith(t, enforce, "Ann", "Bob")
	r := d.runner()
	d.h.receive(d.ps[0], "SDF 20") // handle 0
	d.h.receive(d.ps[1], "SDF 20") // handle 1
	r.drain(d.h)
	d.at(100)
	r.tick(d.h, d.now)
	d.at(200)
	r.tick(d.h, d.now)
	d.expectQueues(t, []string{"SNAP 2 snap"}, []string{"SNAP 1 snap"})
	d.sim().snaps[0] = "changed"
	d.at(300)
	r.tick(d.h, d.now)
	d.expectQueues(t, nil, []string{"SNAP 1 changed"})
}

func TestResyncWaitsForNextINAndRateLimits(t *testing.T) {
	d := directMatchWith(t, enforce, "Ann", "Bob")
	r := d.runner()
	a := d.ps[0]
	// resyncs takes Ann's queue and keeps the RESYNCs; the ticks also send SNAPs.
	resyncs := func() []string { return withPrefix(queued(a), "RESYNC") }
	d.at(1000)
	d.h.receive(a, "IN 500 100:L")
	d.h.receive(a, "SDF 10") // too late: a mark
	r.drain(d.h)
	r.tick(d.h, d.now)
	if got := resyncs(); len(got) != 0 {
		t.Errorf("RESYNC before the next IN: %q", got)
	}
	d.at(1050)
	d.h.receive(a, "IN 600 550:R")
	r.drain(d.h)
	d.at(1100)
	r.tick(d.h, d.now)
	if got, want := resyncs(), []string{"RESYNC 600 2 100:L 550:R"}; !slices.Equal(got, want) {
		t.Errorf("got %q, want %q", got, want)
	}

	d.at(1150)
	d.h.receive(a, "IN x") // two marks, each with its IN, within 100 ms
	d.h.receive(a, "IN y")
	r.drain(d.h)
	r.tick(d.h, d.now) // 50 ms after the last RESYNC: not yet
	if got := resyncs(); len(got) != 0 {
		t.Errorf("RESYNC within 100 ms: %q", got)
	}
	d.at(1200)
	r.tick(d.h, d.now)
	d.at(1250)
	r.tick(d.h, d.now)
	if got, want := resyncs(), []string{"RESYNC 600 4 100:L 550:R"}; !slices.Equal(got, want) {
		t.Errorf("got %q, want %q", got, want)
	}
	queued(d.ps[1])
}

// RESYNC's count covers every IN the runner consumed: handled, rejected whole or dropped from a full queue, but
// not one still queued, nor SDF.
func TestResyncCountsConsumedINs(t *testing.T) {
	d := directMatchWith(t, enforce, "Ann", "Bob")
	r := d.runner()
	a := d.ps[0]
	d.at(500)
	d.h.receive(a, "SDF 20")
	d.h.receive(a, "IN 100 50:L")
	d.h.receive(a, "IN 90") // rejected: tEnd went back
	r.drain(d.h)
	d.at(600)
	r.tick(d.h, d.now)
	if got, want := withPrefix(queued(a), "RESYNC"), []string{"RESYNC 100 2 50:L"}; !slices.Equal(got, want) {
		t.Errorf("got %q, want %q", got, want)
	}
	sent := 2
	for r.players[a].dropped.Load() == 0 { // a flood until the queue drops one
		sent++
		d.h.receive(a, fmt.Sprintf("IN %d", 100+sent))
	}
	r.drain(d.h)
	d.at(700)
	r.tick(d.h, d.now)
	if got, want := withPrefix(queued(a), "RESYNC"), []string{fmt.Sprintf("RESYNC %d %d 50:L", 100+sent-1, sent)}; !slices.Equal(got, want) {
		t.Errorf("got %q, want %q", got, want)
	}
	queued(d.ps[1])
}

func TestHiddenTabIsNotKicked(t *testing.T) {
	d := directMatchWith(t, enforce, "Ann", "Bob")
	r := d.runner()
	a, b := d.ps[0], d.ps[1]
	d.h.receive(a, "SDF 20") // handle 0
	d.h.receive(b, "SDF 20") // handle 1
	r.drain(d.h)
	f := d.sim()
	for range 300 {
		f.attacks[0] = append(f.attacks[0], 1)
	}
	var bob []string
	for ms := 100; ms <= 30000; ms += 100 {
		d.at(ms)
		d.h.receive(a, fmt.Sprintf("IN %d", ms))
		r.drain(d.h)
		r.tick(d.h, d.now)
		queued(a)
		bob = append(bob, queued(b)...)
	}
	if got := withPrefix(slices.Clone(bob), "RESYNC"); len(got) != 0 {
		t.Fatalf("a hidden tab got %d RESYNCs", len(got))
	}
	if got := withPrefix(slices.Clone(bob), "GARB"); len(got) != 300 {
		t.Fatalf("%d GARBs, want 300", len(got))
	}
	d.h.receive(b, "IN 30000 100:G1x1 200:G2x1 300:G3x1")
	r.drain(d.h)
	bob = nil
	for ms := 30100; ms <= 32000; ms += 100 {
		d.at(ms)
		r.tick(d.h, d.now)
		bob = append(bob, queued(b)...)
	}
	if got := withPrefix(bob, "RESYNC"); len(got) < 1 || len(got) > 2 {
		t.Errorf("%d RESYNCs after waking, want 1 or 2", len(got))
	}
	if b.kicked {
		t.Error("the hidden tab was kicked")
	}
}

// A hidden tab as the real client runs it: one frame a second, each sending one IN for the whole second, with
// the acks of the GARBs that reached it since its last frame stamped at that frame's start (the previous IN's
// tEnd). Ann attacks every 300 ms. Bob's INs reach the server latency ms after his frame. At the default -lag
// 1s Bob is never kicked; the test reports the would-resync marks and RESYNCs he collects.
func TestHiddenTabAt1HzUnderGarbageIsNotKicked(t *testing.T) {
	for _, latency := range []int{40, 150} {
		t.Run(fmt.Sprintf("latency %dms", latency), func(t *testing.T) {
			d := directMatchWith(t, enforce, "Ann", "Bob")
			r := d.runner()
			a, b := d.ps[0], d.ps[1]
			d.h.receive(a, "SDF 20") // handle 0
			d.h.receive(b, "SDF 20") // handle 1
			r.drain(d.h)
			f := d.sim()
			type garb struct{ id, lines, arrives int }
			var inbound []garb // GARBs on their way to Bob
			var arrived []garb // GARBs Bob's client has, not acked yet
			sendAt, sendText := -1, ""
			lastEnd, resyncs, garbs := 0, 0, 0
			for ms := 10; ms <= 60000; ms += 10 {
				d.at(ms)
				if ms%100 == 0 { // Ann: an honest 10 Hz stream
					if ms%300 == 0 {
						f.attacks[0] = append(f.attacks[0], 1)
					}
					d.h.receive(a, fmt.Sprintf("IN %d", ms))
					r.drain(d.h)
				}
				if ms == sendAt {
					d.h.receive(b, sendText)
					r.drain(d.h)
				}
				if ms%100 == 0 {
					r.tick(d.h, d.now)
				}
				queued(a)
				for _, m := range queued(b) {
					var g garb
					if n, _ := fmt.Sscanf(m, "GARB %d %d", &g.id, &g.lines); n == 2 {
						g.arrives = ms + latency
						inbound = append(inbound, g)
						garbs++
					}
					if strings.HasPrefix(m, "RESYNC ") {
						resyncs++
					}
				}
				for len(inbound) > 0 && inbound[0].arrives <= ms {
					arrived = append(arrived, inbound[0])
					inbound = inbound[1:]
				}
				if ms%1000 == 0 { // Bob's frame: the GARBs are logged at the frame's start, then a second is replayed
					in := fmt.Sprintf("IN %d", ms)
					for _, g := range arrived {
						in += fmt.Sprintf(" %d:G%dx%d", lastEnd, g.id, g.lines)
					}
					arrived = nil
					lastEnd = ms
					sendAt, sendText = ms+latency, in
				}
				if b.kicked {
					t.Fatalf("kicked at %d ms after %d RESYNCs", ms, resyncs)
				}
			}
			ps := r.players[b]
			t.Logf("latency %d ms: %d GARBs, %d RESYNCs in 60 s, would-resync marks %v", latency, garbs, resyncs, ps.wouldResync)
		})
	}
}

func TestAbuserIsKicked(t *testing.T) {
	d := directMatchWith(t, enforce, "Ann", "Bob")
	r := d.runner()
	a := d.ps[0]
	var resyncs int
	for i := range 11 {
		if a.kicked {
			t.Fatalf("kicked after %d bad INs", i)
		}
		d.at(1000 + 150*i)
		d.h.receive(a, "IN x")
		r.drain(d.h)
		r.tick(d.h, d.now)
		resyncs += len(withPrefix(queued(a), "RESYNC"))
	}
	if resyncs != 11 || !a.kicked {
		t.Errorf("%d RESYNCs, kicked=%v; want 11 and a kick", resyncs, a.kicked)
	}
	select {
	case <-a.kick:
	default:
		t.Error("the kick channel is open")
	}
	if d.ps[1].kicked {
		t.Error("the opponent was kicked")
	}
}

func TestSimErrorEndsMatch(t *testing.T) {
	logs := captureLog(t)
	d := directMatchWith(t, enforce, "Ann", "Bob")
	r := d.runner()
	d.sim().advanceErr = errors.New("boom")
	d.at(1000)
	d.h.receive(d.ps[0], "IN 100 50:L")
	r.drain(d.h)
	end := []string{"LEFT", "PLAYERS 1 1:Ann:0 2:Bob:0"}
	d.expectQueues(t, end, end)
	if d.h.rooms["AAAA"].match != nil || !d.clock.timer(0).isStopped() {
		t.Error("the match did not end")
	}
	if !strings.Contains(logs.String(), "sim error room=AAAA seed=101 player=1: boom") {
		t.Errorf("log %q lacks the sim error", logs.String())
	}
	d.h.receive(d.ps[0], "ATK 4") // no fallback to the client's word
	d.expectQueues(t, nil, nil)

	d = directMatchWith(t, enforce, "Ann", "Bob", "Cat")
	d.sim().advanceErr = errors.New("boom")
	d.at(3000)
	d.runner().tick(d.h, d.now)
	end = []string{"LEFT", "PLAYERS 1 1:Ann:0 2:Bob:0 3:Cat:0"}
	d.expectQueues(t, end, end, end)
}

func TestLeaverIsNotEliminatedTwice(t *testing.T) {
	d := directMatchWith(t, enforce, "Ann", "Bob", "Cat")
	r := d.runner()
	for _, p := range d.ps {
		d.h.receive(p, "SDF 20") // handles 0, 1, 2 in order
	}
	r.drain(d.h)
	f := d.sim()
	f.topOuts[2], f.eliminated[2] = 1, true // what Cat's game would do if it were left running
	d.h.leave(d.ps[2])
	want := []string{"OUT 3 3", "PLAYERS 1 1:Ann:1 2:Bob:1"}
	d.expectQueues(t, want, want, nil)
	var outs []string
	for ms := 100; ms <= 5000; ms += 100 {
		d.at(ms)
		r.drain(d.h)
		r.tick(d.h, d.now)
		for _, p := range d.ps {
			outs = append(outs, withPrefix(queued(p), "OUT")...)
		}
	}
	if len(outs) != 0 {
		t.Errorf("after leaving: %q", outs)
	}
	if got := f.steps(2); len(got) != 0 {
		t.Errorf("the leaver's game was advanced: %q", got)
	}
}

// The production simulation decides a duel KO from IN alone: Ann stacks hard drops until she tops out.
func TestRealSimDuelKO(t *testing.T) {
	e, err := sim.NewEngine(context.Background())
	if err != nil {
		t.Skipf("no game core: %v", err)
	}
	e.Close(context.Background())
	d := directMatchWith(t, func(h *Hub) {
		enforce(h)
		h.newSim = newSharedSim
	}, "Ann", "Bob")
	r := d.runner()
	a, b := d.ps[0], d.ps[1]
	d.h.receive(a, "SDF 20")
	var annKO, bobKO []string
	t0 := 20
	for batch := 0; batch < 50 && len(annKO) == 0; batch++ {
		var in strings.Builder
		for range 10 {
			fmt.Fprintf(&in, " %d:HD", t0)
			t0 += 20
		}
		d.at(t0)
		d.h.receive(a, fmt.Sprintf("IN %d%s", t0, in.String()))
		r.drain(d.h)
		annKO = append(annKO, withPrefix(queued(a), "KO")...)
		bobKO = append(bobKO, withPrefix(queued(b), "KO")...)
	}
	if r.failed {
		t.Fatal("the simulation failed")
	}
	if len(annKO) == 0 || annKO[0] != "KO 0 1" || len(bobKO) == 0 || bobKO[0] != "KO 1 0" {
		t.Errorf("Ann got %q, Bob got %q; want KO 0 1 and KO 1 0", annKO, bobKO)
	}
	r.finish()
}

func TestEnforceSimStartFailureEndsMatch(t *testing.T) {
	for _, names := range [][]string{{"Ann", "Bob"}, {"Ann", "Bob", "Cat"}} {
		logs := captureLog(t)
		h := NewHub()
		h.newCode = func() string { return "AAAA" }
		h.newSeed = func() uint32 { return 101 }
		clock := &fakeClock{}
		h.afterFunc = clock.afterFunc
		h.auth.mode = "enforce"
		h.newSim = func() (gameSim, error) { return nil, errors.New("no engine") }
		var ps []*player
		for _, name := range names {
			ps = append(ps, testPlayer(name))
		}
		h.receive(ps[0], "CREATE 1")
		for _, p := range ps[1:] {
			h.receive(p, "JOIN AAAA")
		}
		for _, p := range ps {
			queued(p)
		}
		h.receive(ps[0], "BEGIN")
		start, during, after := "START 101 1", "PLAYERS 1", "PLAYERS 1"
		for i, p := range ps {
			start += fmt.Sprintf(" %d", i+1)
			during += fmt.Sprintf(" %d:%s:1", i+1, p.name)
			after += fmt.Sprintf(" %d:%s:0", i+1, p.name)
		}
		want := []string{start, during, "LEFT", after}
		for _, p := range ps {
			if got := queued(p); !slices.Equal(got, want) {
				t.Errorf("%s got %q, want %q", p.name, got, want)
			}
		}
		if h.rooms["AAAA"].match != nil {
			t.Error("the match did not end")
		}
		if len(names) == 2 && !clock.timer(0).isStopped() {
			t.Error("the duel timer is still armed")
		}
		if !strings.Contains(logs.String(), "sim error room=AAAA seed=101 player=0: no engine") {
			t.Errorf("log %q lacks the sim error", logs.String())
		}
		h.receive(ps[0], "ATK 4") // no fallback to the client's word
		h.receive(ps[1], "DEAD")
		for _, p := range ps {
			if got := queued(p); len(got) != 0 {
				t.Errorf("%s got %q after the match ended", p.name, got)
			}
		}
	}
}
