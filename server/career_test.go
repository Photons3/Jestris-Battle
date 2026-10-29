package main

import (
	"errors"
	"sync"
	"testing"

	"tetris-server/sim"
)

// fakeCareer collects the deltas the hub records, and fails the test when one account is recorded twice.
type fakeCareer struct {
	t   *testing.T
	mu  sync.Mutex
	got map[int64]Career
}

func newFakeCareer(t *testing.T) *fakeCareer {
	return &fakeCareer{t: t, got: map[int64]Career{}}
}

func (f *fakeCareer) Record(userID int64, d Career) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if _, dup := f.got[userID]; dup {
		f.t.Errorf("account %d recorded twice", userID)
	}
	f.got[userID] = d
}

func (f *fakeCareer) expect(t *testing.T, want map[int64]Career) {
	t.Helper()
	f.mu.Lock()
	defer f.mu.Unlock()
	if len(f.got) != len(want) {
		t.Errorf("recorded %+v, want %+v", f.got, want)
		return
	}
	for id, w := range want {
		if g, ok := f.got[id]; !ok || g != w {
			t.Errorf("account %d recorded %+v (present %v), want %+v", id, g, ok, w)
		}
	}
}

// careerMatch is directMatchWith recording into a fakeCareer; accounts[i] is player i's account, 0 for a guest.
func careerMatch(t *testing.T, accounts []int64, names ...string) (*directTest, *fakeCareer) {
	t.Helper()
	fc := newFakeCareer(t)
	d := directMatchWith(t, func(h *Hub) { h.career = fc }, names...)
	for i, id := range accounts {
		d.ps[i].accountID = id
	}
	return d, fc
}

func TestDuelRecordsBothAccounts(t *testing.T) {
	d, fc := careerMatch(t, []int64{1, 2}, "Ann", "Bob")
	d.sim().stats[0] = sim.Stats{Pieces: 40, Attack: 12, MaxCombo: 3, T: 60000}
	d.sim().stats[1] = sim.Stats{Pieces: 30, T: 60000}
	d.at(2000)
	d.runner().tick(d.h, d.now)
	d.h.receive(d.ps[0], "ATK 2")
	d.runner().drain(d.h)
	for range 3 {
		d.h.receive(d.ps[1], "DEAD")
	}
	fc.expect(t, map[int64]Career{
		1: {DuelWins: 1, KosDealt: 3, Sent: 2, Pieces: 40, Attack: 12, MaxCombo: 3, MsPlayed: 60000},
		2: {DuelLosses: 1, KosTaken: 3, Pieces: 30, MsPlayed: 60000},
	})
}

func TestDuelRecordsOneDeltaWhenBothSeatsShareAnAccount(t *testing.T) {
	d, fc := careerMatch(t, []int64{1, 1}, "Ann", "Bob")
	d.clock.timer(0).fire()
	fc.expect(t, map[int64]Career{1: {DuelDraws: 1}})
}

func TestDuelDrawRecordsADrawForBoth(t *testing.T) {
	d, fc := careerMatch(t, []int64{1, 2}, "Ann", "Bob")
	d.clock.timer(0).fire()
	fc.expect(t, map[int64]Career{1: {DuelDraws: 1}, 2: {DuelDraws: 1}})
}

func TestDuelGuestSeatRecordsNothingForTheGuest(t *testing.T) {
	d, fc := careerMatch(t, []int64{1, 0}, "Ann", "Bob")
	d.clock.timer(0).fire()
	fc.expect(t, map[int64]Career{1: {DuelDraws: 1}})
}

func TestDuelLeftRecordsNothing(t *testing.T) {
	d, fc := careerMatch(t, []int64{1, 2}, "Ann", "Bob")
	d.h.leave(d.ps[1])
	fc.expect(t, map[int64]Career{})
}

func TestRoyaleRecordsEachAccountsPlace(t *testing.T) {
	d, fc := careerMatch(t, []int64{1, 2, 3, 0}, "Ann", "Bob", "Cat", "Dan")
	d.h.receive(d.ps[1], "DEAD") // Bob out in place 4
	d.h.receive(d.ps[3], "DEAD") // Dan out in place 3
	d.h.leave(d.ps[2])           // Cat out in place 2: Ann wins
	fc.expect(t, map[int64]Career{
		1: {RoyaleMatches: 1, RoyaleWins: 1, RoyalePlaceSum: 1},
		2: {RoyaleMatches: 1, RoyalePlaceSum: 4},
		3: {RoyaleMatches: 1, RoyalePlaceSum: 2},
	})
}

func TestCareerWithoutASimulationKeepsTheVerdict(t *testing.T) {
	fc := newFakeCareer(t)
	d := directMatchWith(t, func(h *Hub) {
		h.career = fc
		h.newSim = func() (gameSim, error) { return nil, errors.New("no engine") }
	}, "Ann", "Bob")
	d.ps[0].accountID, d.ps[1].accountID = 1, 2
	if d.h.rooms["AAAA"].match.runner != nil {
		t.Fatal("the match has a runner")
	}
	d.h.receive(d.ps[0], "ATK 3")
	for range 3 {
		d.h.receive(d.ps[1], "DEAD")
	}
	fc.expect(t, map[int64]Career{
		1: {DuelWins: 1, KosDealt: 3, Sent: 3},
		2: {DuelLosses: 1, KosTaken: 3},
	})
}

func TestCareerWriterAddsToTheStore(t *testing.T) {
	s := openTestStore(t)
	acc, _, err := s.Register(ctx, "Sam", "password1")
	if err != nil {
		t.Fatal(err)
	}
	w := newCareerWriter(s)
	w.Record(acc.ID, Career{DuelWins: 1, MaxCombo: 4})
	w.Record(acc.ID, Career{DuelWins: 1, MaxCombo: 2})
	w.Close()
	p, err := s.Profile(ctx, acc.ID)
	if err != nil {
		t.Fatal(err)
	}
	if p.Career.DuelWins != 2 || p.Career.MaxCombo != 4 {
		t.Fatalf("career %+v, want 2 wins and max combo 4", p.Career)
	}
}
