package main

import (
	"fmt"
	"log"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeSim is a scripted gameSim: it records every call that changes a game and returns the attacks and
// top-outs a test sets per handle.
type fakeSim struct {
	calls   []string
	handles int
	attacks map[int][]int // queued TakeAttack results, oldest first
	topOuts map[int]int
	closed  bool
}

func newFakeSim() *fakeSim { return &fakeSim{attacks: map[int][]int{}, topOuts: map[int]int{}} }

func (f *fakeSim) record(format string, a ...any) {
	f.calls = append(f.calls, fmt.Sprintf(format, a...))
}

func (f *fakeSim) New(seed uint32, level, players, sdf int) (int, error) {
	f.record("New(%d,%d,%d,%d)", seed, level, players, sdf)
	f.handles++
	return f.handles - 1, nil
}
func (f *fakeSim) Input(h, code int) error { f.record("Input(%d,%d)", h, code); return nil }
func (f *fakeSim) AcceptGarbage(h, id, lines int) error {
	f.record("AcceptGarbage(%d,%d,%d)", h, id, lines)
	return nil
}
func (f *fakeSim) Advance(h, ms int) error        { f.record("Advance(%d,%d)", h, ms); return nil }
func (f *fakeSim) T(h int) (int, error)           { return 0, nil }
func (f *fakeSim) TopOuts(h int) (int, error)     { return f.topOuts[h], nil }
func (f *fakeSim) Eliminated(h int) (bool, error) { return false, nil }
func (f *fakeSim) Hash(h int) (uint64, error)     { return 0, nil }
func (f *fakeSim) Snapshot(h int) (string, error) { return "snap", nil }
func (f *fakeSim) Close() error                   { f.closed = true; return nil }

func (f *fakeSim) TakeAttack(h int) (int, error) {
	if len(f.attacks[h]) == 0 {
		return 0, nil
	}
	n := f.attacks[h][0]
	f.attacks[h] = f.attacks[h][1:]
	return n, nil
}

// steps lists the recorded calls for handle h, without New.
func (f *fakeSim) steps(h int) []string {
	var out []string
	for _, c := range f.calls {
		if !strings.HasPrefix(c, "New(") && strings.Contains(c, fmt.Sprintf("(%d,", h)) {
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
	if ps.t != 120 || ps.reportedEnd != 120 || !slices.Equal(ps.canonical, []string{"10:L", "50:HD"}) {
		t.Errorf("t=%d reportedEnd=%d canonical=%q", ps.t, ps.reportedEnd, ps.canonical)
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
	if want := []string{"1100:G1x2", "1950:G2x3", "9100:G3x1"}; !slices.Equal(ps.canonical, want) {
		t.Errorf("canonical %q, want %q", ps.canonical, want)
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
	if ps.reportedEnd != 600 || len(ps.canonical) != maxInputEvents {
		t.Errorf("64 entries: reportedEnd=%d, %d entries", ps.reportedEnd, len(ps.canonical))
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
	for i := range cap(r.in) + 3 {
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

func TestRunnerIgnoresNonParticipants(t *testing.T) {
	d := directMatch(t, "Ann", "Bob")
	r := d.runner()
	d.at(1000)
	cat := testPlayer("Cat")
	d.h.receive(cat, "JOIN AAAA")
	queued(cat)
	d.h.receive(cat, "IN 100 50:L")
	d.h.receive(cat, "SDF 10")
	if len(r.in) != 0 {
		t.Errorf("%d messages queued for the runner from a non-participant", len(r.in))
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
