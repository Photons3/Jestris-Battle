package main

import (
	"context"
	"log"
	"sync"
)

// careerQueueLength bounds the deltas waiting for the database; a full queue drops the new one.
const careerQueueLength = 256

// careerRecorder takes the career delta of one finished match for one account. It runs under Hub.mu, so it
// must not block.
type careerRecorder interface {
	Record(userID int64, d Career)
}

// careerBase is p's delta before the verdict or place: the lines it sent, and, while the match has a runner
// that has advanced p's game, the simulation counters and the game time as ms played.
func (m *match) careerBase(p *player) Career {
	c := Career{Sent: m.sent[p]}
	if m.runner == nil {
		return c
	}
	if st, ok := m.runner.statsOf(p); ok {
		c.Pieces, c.Attack, c.GarbageCleared, c.Cancelled = st.Pieces, st.Attack, st.GarbageCleared, st.Cancelled
		c.Tetrises, c.TSpins, c.MaxCombo, c.MsPlayed = st.Tetrises, st.TSpins, st.MaxCombo, st.T
	}
	return c
}

// claimAccount is true the first time this match records p's account. A second seat with the same
// accountID, which a kicked socket must not leave behind, does not write another delta.
func (m *match) claimAccount(p *player) bool {
	if m.career == nil || p.accountID == 0 || m.recorded[p.accountID] {
		return false
	}
	if m.recorded == nil {
		m.recorded = map[int64]bool{}
	}
	m.recorded[p.accountID] = true
	return true
}

// recordDuel records each account participant's verdict and KOs. It runs before endMatch drops the runner.
func (m *match) recordDuel() {
	for _, p := range m.players {
		if !m.claimAccount(p) {
			continue
		}
		c := m.careerBase(p)
		c.KosDealt, c.KosTaken = m.kos[p], m.kos[m.opponent(p)]
		switch m.verdict(p) {
		case "WIN":
			c.DuelWins = 1
		case "LOSE":
			c.DuelLosses = 1
		default:
			c.DuelDraws = 1
		}
		m.career.Record(p.accountID, c)
	}
}

// recordPlace records p's royale place, 1 for the winner. It runs before endMatch drops the runner.
func (m *match) recordPlace(p *player, place int) {
	if !m.claimAccount(p) {
		return
	}
	c := m.careerBase(p)
	c.RoyaleMatches, c.RoyalePlaceSum = 1, place
	if place == 1 {
		c.RoyaleWins = 1
	}
	m.career.Record(p.accountID, c)
}

type careerEntry struct {
	userID int64
	d      Career
}

// careerWriter adds the recorded deltas to the database on one goroutine.
type careerWriter struct {
	store  *Store
	queue  chan careerEntry
	done   chan struct{}
	mu     sync.Mutex // guards closed against a Record racing Close
	closed bool
}

func newCareerWriter(store *Store) *careerWriter {
	w := &careerWriter{store: store, queue: make(chan careerEntry, careerQueueLength), done: make(chan struct{})}
	go w.run()
	return w
}

func (w *careerWriter) run() {
	defer close(w.done)
	for e := range w.queue {
		if err := w.store.AddCareer(context.Background(), e.userID, e.d); err != nil {
			log.Printf("career write user=%d: %v", e.userID, err)
		}
	}
}

// Record queues d without blocking; a full queue logs and drops it.
func (w *careerWriter) Record(userID int64, d Career) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.closed {
		return
	}
	select {
	case w.queue <- careerEntry{userID, d}:
	default:
		log.Printf("career queue full: dropped user=%d", userID)
	}
}

// Close writes what is queued, then returns.
func (w *careerWriter) Close() {
	w.mu.Lock()
	if !w.closed {
		w.closed = true
		close(w.queue)
	}
	w.mu.Unlock()
	<-w.done
}
