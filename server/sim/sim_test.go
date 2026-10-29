package sim

import (
	"bufio"
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func newMatch(tb testing.TB) *Match {
	tb.Helper()
	ctx := context.Background()
	e, err := NewEngine(ctx)
	if err != nil {
		tb.Fatal(err)
	}
	tb.Cleanup(func() { e.Close(ctx) })
	m, err := e.NewMatch(ctx)
	if err != nil {
		tb.Fatal(err)
	}
	tb.Cleanup(func() { m.Close() })
	return m
}

func TestFixturesReplay(t *testing.T) {
	files, err := filepath.Glob("testdata/*.txt")
	if err != nil || len(files) == 0 {
		t.Fatalf("no fixtures: %v", err)
	}
	for _, f := range files {
		t.Run(filepath.Base(f), func(t *testing.T) { replay(t, f) })
	}
}

func replay(t *testing.T, path string) {
	fh, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer fh.Close()
	m := newMatch(t)
	h := -1
	now, attackSum := 0, 0
	advance := func(to int) {
		if to > now {
			if err := m.Advance(h, to-now); err != nil {
				t.Fatalf("%s: advance: %v", path, err)
			}
			now = to
		}
	}
	sc := bufio.NewScanner(fh)
	for line := 1; sc.Scan(); line++ {
		f := strings.Fields(sc.Text())
		if len(f) == 0 {
			continue
		}
		where := fmt.Sprintf("%s:%d", path, line)
		switch f[0] {
		case "setup":
			seed, _ := strconv.ParseUint(f[1], 10, 32)
			lvl, _ := strconv.Atoi(f[2])
			pl, _ := strconv.Atoi(f[3])
			sdf, _ := strconv.Atoi(f[4])
			if h, err = m.New(uint32(seed), lvl, pl, sdf); err != nil {
				t.Fatalf("%s: new: %v", where, err)
			}
		case "ev":
			tt, _ := strconv.Atoi(f[1])
			advance(tt)
			code, id, lines, ok := ParseToken(f[2])
			if !ok {
				t.Fatalf("%s: bad token %q", where, f[2])
			}
			if code < 0 {
				err = m.AcceptGarbage(h, id, lines)
			} else {
				err = m.Input(h, code)
			}
			if err != nil {
				t.Fatalf("%s: %v", where, err)
			}
		case "check":
			tt, _ := strconv.Atoi(f[1])
			advance(tt)
			wantAtk, _ := strconv.Atoi(f[2])
			wantTop, _ := strconv.Atoi(f[3])
			wantHash, _ := strconv.ParseUint(f[4], 16, 64)
			gotT, err := m.T(h)
			if err != nil {
				t.Fatal(err)
			}
			a, _ := m.TakeAttack(h)
			attackSum += a
			top, _ := m.TopOuts(h)
			hash, err := m.Hash(h)
			if err != nil {
				t.Fatal(err)
			}
			if gotT != tt || attackSum != wantAtk || top != wantTop || hash != wantHash {
				t.Fatalf("%s: got t=%d attack=%d topOuts=%d hash=%016x, want t=%d attack=%d topOuts=%d hash=%016x",
					where, gotT, attackSum, top, hash, tt, wantAtk, wantTop, wantHash)
			}
		}
	}
}

func TestParseToken(t *testing.T) {
	codes := map[string]int{"L": CodeLeft, "R": CodeRight, "CW": CodeRotateCW, "CCW": CodeRotateCCW,
		"HD": CodeHardDrop, "H": CodeHold, "LW": CodeLeftWall, "RW": CodeRightWall, "S+": CodeSoftOn, "S-": CodeSoftOff}
	for tok, want := range codes {
		c, _, _, ok := ParseToken(tok)
		if !ok || c != want {
			t.Errorf("%s: got %d %v, want %d", tok, c, ok, want)
		}
	}
	if c, id, n, ok := ParseToken("G3x2"); !ok || c != -1 || id != 3 || n != 2 {
		t.Errorf("G3x2: got %d %d %d %v", c, id, n, ok)
	}
	if _, _, _, ok := ParseToken("G1x40"); !ok {
		t.Error("G1x40 should parse")
	}
	for _, bad := range []string{"X", "G0x1", "G1x41", "G1x0", "G+1x2", "G1x+2", "G-1x2", "G1x-2", "G4294967297x1", "G2147483648x1", "G1x041", "G1x2x3", "G1", "Gx", "G1x", ""} {
		if _, _, _, ok := ParseToken(bad); ok {
			t.Errorf("%q should be rejected", bad)
		}
	}
}

func TestParseTokenLeadingZerosMatchCpp(t *testing.T) {
	if c, id, n, ok := ParseToken("G01x02"); !ok || c != -1 || id != 1 || n != 2 {
		t.Errorf("G01x02: got %d %d %d %v", c, id, n, ok)
	}
	if _, id, _, ok := ParseToken("G2147483647x1"); !ok || id != 2147483647 {
		t.Errorf("max id: got %d %v", id, ok)
	}
}

func TestArgumentsOutOfRangeAreErrors(t *testing.T) {
	m := newMatch(t)
	h, err := m.New(1, 1, 2, 20)
	if err != nil {
		t.Fatal(err)
	}
	if err := m.Input(1<<32+h, CodeHardDrop); err == nil {
		t.Error("Input: expected range error")
	}
	if err := m.AcceptGarbage(h, 1<<32+1, 1); err == nil {
		t.Error("AcceptGarbage: expected range error")
	}
	if err := m.Advance(h, 1<<31); err == nil {
		t.Error("Advance: expected range error")
	}
	if _, err := m.New(1, 1<<32+1, 2, 20); err == nil {
		t.Error("New: expected range error")
	}
	if _, err := m.T(-1 << 40); err == nil {
		t.Error("T: expected range error")
	}
	if _, err := m.Hash(h); err != nil {
		t.Fatalf("match must not be poisoned: %v", err)
	}
}

func TestSnapshotTracksState(t *testing.T) {
	m := newMatch(t)
	h, err := m.New(5, 1, 2, 20)
	if err != nil {
		t.Fatal(err)
	}
	a, err := m.Snapshot(h)
	if err != nil || a == "" || strings.HasPrefix(a, "SNAP ") {
		t.Fatalf("snapshot %q err %v", a, err)
	}
	if b, _ := m.Snapshot(h); a != b {
		t.Fatal("snapshot not stable")
	}
	if err := m.Input(h, CodeHardDrop); err != nil {
		t.Fatal(err)
	}
	if err := m.Advance(h, 1); err != nil {
		t.Fatal(err)
	}
	if c, _ := m.Snapshot(h); c == a {
		t.Fatal("snapshot did not change")
	}
}

func TestStatsCountPiecesAndTime(t *testing.T) {
	m := newMatch(t)
	h, err := m.New(5, 1, 2, 20)
	if err != nil {
		t.Fatal(err)
	}
	if err := m.Input(h, CodeHardDrop); err != nil {
		t.Fatal(err)
	}
	if err := m.Advance(h, 250); err != nil {
		t.Fatal(err)
	}
	st, err := m.Stats(h)
	now, _ := m.T(h)
	if err != nil || st.Pieces != 1 || st.T != now || st.T <= 0 || st.Attack != 0 {
		t.Fatalf("stats %+v (t %d) err %v", st, now, err)
	}
}

func TestStatsOfABadHandleIsAnError(t *testing.T) {
	m := newMatch(t)
	if _, err := m.Stats(99); err == nil {
		t.Fatal("want an error")
	}
}

func TestTrapBecomesError(t *testing.T) {
	m := newMatch(t)
	if err := m.Advance(99, 1); err == nil {
		t.Fatal("expected trap error")
	}
	if _, err := m.New(1, 1, 2, 20); err == nil {
		t.Fatal("expected poisoned error")
	}
	if _, err := m.Hash(0); err == nil {
		t.Fatal("expected poisoned error")
	}
}

func TestTwoMatchesAreIndependent(t *testing.T) {
	ctx := context.Background()
	e, err := NewEngine(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer e.Close(ctx)
	a, _ := e.NewMatch(ctx)
	b, _ := e.NewMatch(ctx)
	defer a.Close()
	defer b.Close()
	ha, _ := a.New(3, 1, 2, 20)
	hb, _ := b.New(3, 1, 2, 20)
	before, _ := b.Hash(hb)
	if err := a.Input(ha, CodeHardDrop); err != nil {
		t.Fatal(err)
	}
	if err := a.Advance(ha, 5000); err != nil {
		t.Fatal(err)
	}
	if after, _ := b.Hash(hb); after != before {
		t.Fatal("advancing one match changed the other")
	}
}

// The module keeps no event log, so a flood of inputs does not grow its memory.
func TestInputsDoNotGrowMemory(t *testing.T) {
	m := newMatch(t)
	h, err := m.New(9, 1, 2, 20)
	if err != nil {
		t.Fatal(err)
	}
	before := m.mod.Memory().Size()
	for range 300000 {
		if err := m.Input(h, CodeLeft); err != nil {
			t.Fatal(err)
		}
	}
	if after := m.mod.Memory().Size(); after != before {
		t.Errorf("memory grew from %d to %d bytes", before, after)
	}
}

func TestFreeReleasesAHandle(t *testing.T) {
	m := newMatch(t)
	a, _ := m.New(1, 1, 2, 20)
	b, _ := m.New(2, 1, 2, 20)
	if err := m.Free(a); err != nil {
		t.Fatal(err)
	}
	if c, err := m.New(3, 1, 2, 7); err != nil || c != a {
		t.Errorf("New after Free = %d, %v; want the freed handle %d", c, err, a)
	}
	if _, err := m.Hash(b); err != nil {
		t.Errorf("the other game: %v", err)
	}
}

func BenchmarkTwoMinuteGame(b *testing.B) {
	m := newMatch(b)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		h, err := m.New(7, 1, 2, 20)
		if err != nil {
			b.Fatal(err)
		}
		for t := 0; t < 120000; t += 333 {
			if err := m.Input(h, CodeHardDrop); err != nil {
				b.Fatal(err)
			}
			if err := m.Advance(h, 333); err != nil {
				b.Fatal(err)
			}
		}
	}
}

func BenchmarkRoomMemory(b *testing.B) {
	for i := 0; i < b.N; i++ {
		m := newMatch(b)
		for j := 0; j < 16; j++ {
			h, err := m.New(uint32(j+1), 1, 2, 20)
			if err != nil {
				b.Fatal(err)
			}
			if err := m.Advance(h, 1000); err != nil {
				b.Fatal(err)
			}
		}
		pages := m.mod.Memory().Size() / 65536
		b.ReportMetric(float64(pages), "pages")
		b.ReportMetric(float64(m.mod.Memory().Size()), "bytes")
	}
}
