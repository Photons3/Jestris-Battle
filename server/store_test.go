package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

var ctx = context.Background()

func openTestStore(t *testing.T) *Store {
	t.Helper()
	s, err := OpenStore(filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	return s
}

func TestOpenEnablesWAL(t *testing.T) {
	s := openTestStore(t)
	var mode string
	if err := s.db.QueryRow("PRAGMA journal_mode").Scan(&mode); err != nil || mode != "wal" {
		t.Fatalf("journal_mode %q err %v", mode, err)
	}
}

func TestRegisterRejectsADuplicateRegardlessOfCase(t *testing.T) {
	s := openTestStore(t)
	if _, _, err := s.Register(ctx, "Sam", "password1"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := s.Register(ctx, "sAM", "password2"); !errors.Is(err, errNameTaken) {
		t.Fatalf("err %v, want errNameTaken", err)
	}
}

func TestRegisterRejectsBadNames(t *testing.T) {
	s := openTestStore(t)
	for _, name := range []string{"", "thirteenchars", "a b", "a:b"} {
		if _, _, err := s.Register(ctx, name, "password1"); !errors.Is(err, errBadName) {
			t.Errorf("Register(%q) err %v, want errBadName", name, err)
		}
	}
}

func TestPasswordLengthCountsBytes(t *testing.T) {
	s := openTestStore(t)
	cases := []struct {
		name, password string
		want           error
	}{
		{"u1", "1234567", errBadPassword},
		{"u2", strings.Repeat("é", 4), nil},
		{"u3", strings.Repeat("x", 72), nil},
		{"u4", strings.Repeat("x", 73), errBadPassword},
	}
	for _, c := range cases {
		_, _, err := s.Register(ctx, c.name, c.password)
		if !errors.Is(err, c.want) {
			t.Errorf("Register(%q, %d bytes) err %v, want %v", c.name, len(c.password), err, c.want)
		}
	}
}

func TestLoginIgnoresCaseAndKeepsTheRegisteredName(t *testing.T) {
	s := openTestStore(t)
	reg, _, err := s.Register(ctx, "Sam", "password1")
	if err != nil {
		t.Fatal(err)
	}
	acc, token, err := s.Login(ctx, "sam", "password1")
	if err != nil {
		t.Fatal(err)
	}
	if acc.Username != "Sam" || acc.ID != reg.ID {
		t.Fatalf("account %+v, want %+v", acc, reg)
	}
	got, err := s.Session(ctx, token)
	if err != nil || got != acc {
		t.Fatalf("Session = %+v, %v; want %+v", got, err, acc)
	}
}

func TestLoginMismatchAndUnknownUserAreTheSameError(t *testing.T) {
	s := openTestStore(t)
	if _, _, err := s.Register(ctx, "Sam", "password1"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := s.Login(ctx, "Sam", "wrong-pass"); !errors.Is(err, errWrongLogin) {
		t.Errorf("wrong password err %v, want errWrongLogin", err)
	}
	if _, _, err := s.Login(ctx, "Nobody", "password1"); !errors.Is(err, errWrongLogin) {
		t.Errorf("unknown user err %v, want errWrongLogin", err)
	}
}

func TestTokensAreHashedAtRest(t *testing.T) {
	s := openTestStore(t)
	_, token, err := s.Register(ctx, "Sam", "password1")
	if err != nil {
		t.Fatal(err)
	}
	if len(token) != 64 {
		t.Fatalf("token length %d, want 64", len(token))
	}
	raw, err := hex.DecodeString(token)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(raw)
	want := hex.EncodeToString(sum[:])
	rows, err := s.db.Query("SELECT token_hash FROM sessions")
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var hashes []string
	for rows.Next() {
		var h string
		if err := rows.Scan(&h); err != nil {
			t.Fatal(err)
		}
		hashes = append(hashes, h)
	}
	if len(hashes) != 1 || hashes[0] != want || hashes[0] == token {
		t.Fatalf("token_hash rows %v, want only %q (and not the token)", hashes, want)
	}
}

func TestLogoutEndsTheSession(t *testing.T) {
	s := openTestStore(t)
	_, token, err := s.Register(ctx, "Sam", "password1")
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Logout(ctx, token); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Session(ctx, token); !errors.Is(err, errNoSession) {
		t.Errorf("after logout err %v, want errNoSession", err)
	}
	for _, bad := range []string{"zz", ""} {
		if _, err := s.Session(ctx, bad); !errors.Is(err, errNoSession) {
			t.Errorf("Session(%q) err %v, want errNoSession", bad, err)
		}
	}
}

func TestChangePasswordKeepsThisSessionAndEndsOthers(t *testing.T) {
	s := openTestStore(t)
	acc, a, err := s.Register(ctx, "Sam", "password1")
	if err != nil {
		t.Fatal(err)
	}
	_, b, err := s.Login(ctx, "Sam", "password1")
	if err != nil {
		t.Fatal(err)
	}
	if err := s.ChangePassword(ctx, acc.ID, a, "wrong-pass", "newpass12"); !errors.Is(err, errWrongLogin) {
		t.Errorf("wrong current err %v, want errWrongLogin", err)
	}
	if err := s.ChangePassword(ctx, acc.ID, a, "password1", "short"); !errors.Is(err, errBadPassword) {
		t.Errorf("short new err %v, want errBadPassword", err)
	}
	if err := s.ChangePassword(ctx, acc.ID, a, "password1", "newpass12"); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Session(ctx, a); err != nil {
		t.Errorf("kept session err %v", err)
	}
	if _, err := s.Session(ctx, b); !errors.Is(err, errNoSession) {
		t.Errorf("other session err %v, want errNoSession", err)
	}
	if _, _, err := s.Login(ctx, "Sam", "password1"); !errors.Is(err, errWrongLogin) {
		t.Errorf("old password err %v, want errWrongLogin", err)
	}
	if _, _, err := s.Login(ctx, "Sam", "newpass12"); err != nil {
		t.Errorf("new password err %v", err)
	}
}

func registerForTest(t *testing.T, s *Store, name string) Account {
	t.Helper()
	acc, _, err := s.Register(ctx, name, "password1")
	if err != nil {
		t.Fatal(err)
	}
	return acc
}

func wantBest(t *testing.T, label string, got *int, want int) {
	t.Helper()
	if got == nil || *got != want {
		t.Errorf("%s = %v, want %d", label, got, want)
	}
}

func TestSettingsRoundTripVerbatim(t *testing.T) {
	s := openTestStore(t)
	acc := registerForTest(t, s, "Sam")
	p, err := s.Profile(ctx, acc.ID)
	if err != nil {
		t.Fatal(err)
	}
	if p.Username != "Sam" || p.Settings != "" || p.Bests != (Bests{}) {
		t.Fatalf("new profile %+v", p)
	}
	body := "version=1\nname=Other\nsprint=5\n"
	if err := s.SaveSettings(ctx, acc.ID, body); err != nil {
		t.Fatal(err)
	}
	p, err = s.Profile(ctx, acc.ID)
	if err != nil {
		t.Fatal(err)
	}
	if p.Settings != body {
		t.Errorf("Settings %q, want %q", p.Settings, body)
	}
	if p.Bests != (Bests{}) {
		t.Errorf("Bests %+v, want all nil", p.Bests)
	}
}

func TestSprintAndCheeseKeepTheFasterTime(t *testing.T) {
	for _, mode := range []string{"sprint", "cheese"} {
		t.Run(mode, func(t *testing.T) {
			s := openTestStore(t)
			acc := registerForTest(t, s, "Sam")
			for _, c := range []struct{ submit, want int }{{60000, 60000}, {61000, 60000}, {60000, 60000}, {59000, 59000}} {
				b, err := s.SubmitBest(ctx, acc.ID, mode, c.submit)
				if err != nil {
					t.Fatal(err)
				}
				got, other1, other2 := b.SprintMs, b.CheeseMs, b.BlitzScore
				if mode == "cheese" {
					got, other1 = b.CheeseMs, b.SprintMs
				}
				wantBest(t, mode+" after "+strconv.Itoa(c.submit), got, c.want)
				if other1 != nil || other2 != nil {
					t.Errorf("other bests %v %v, want nil", other1, other2)
				}
			}
		})
	}
}

func TestBlitzKeepsTheHigherScore(t *testing.T) {
	s := openTestStore(t)
	acc := registerForTest(t, s, "Sam")
	zero := registerForTest(t, s, "Zed")
	for _, c := range []struct{ submit, want int }{{5000, 5000}, {4000, 5000}} {
		b, err := s.SubmitBest(ctx, acc.ID, "blitz", c.submit)
		if err != nil {
			t.Fatal(err)
		}
		wantBest(t, "blitz after "+strconv.Itoa(c.submit), b.BlitzScore, c.want)
		if b.SprintMs != nil || b.CheeseMs != nil {
			t.Errorf("other bests %v %v, want nil", b.SprintMs, b.CheeseMs)
		}
	}
	b, err := s.SubmitBest(ctx, zero.ID, "blitz", 0)
	if err != nil {
		t.Fatal(err)
	}
	wantBest(t, "blitz 0 on a fresh account", b.BlitzScore, 0)
}

func TestSubmitBestRejectsBadInput(t *testing.T) {
	s := openTestStore(t)
	acc := registerForTest(t, s, "Sam")
	for _, c := range []struct {
		mode   string
		result int
	}{{"marathon", 1}, {"zen", 1}, {"", 1}, {"sprint", -1}} {
		if _, err := s.SubmitBest(ctx, acc.ID, c.mode, c.result); !errors.Is(err, errBadMode) {
			t.Errorf("SubmitBest(%q, %d) err %v, want errBadMode", c.mode, c.result, err)
		}
	}
}

func TestAddCareerSumsAndKeepsTheMaxCombo(t *testing.T) {
	s := openTestStore(t)
	acc := registerForTest(t, s, "Sam")
	if err := s.AddCareer(ctx, acc.ID, Career{DuelWins: 1, Pieces: 100, MaxCombo: 5, MsPlayed: 60000}); err != nil {
		t.Fatal(err)
	}
	if err := s.AddCareer(ctx, acc.ID, Career{DuelLosses: 1, Pieces: 50, MaxCombo: 3, MsPlayed: 30000}); err != nil {
		t.Fatal(err)
	}
	p, err := s.Profile(ctx, acc.ID)
	if err != nil {
		t.Fatal(err)
	}
	want := Career{DuelWins: 1, DuelLosses: 1, Pieces: 150, MaxCombo: 5, MsPlayed: 90000}
	if p.Career != want {
		t.Errorf("Career %+v, want %+v", p.Career, want)
	}
}

func TestCareerViewRates(t *testing.T) {
	c := Career{Pieces: 150, Attack: 90, GarbageCleared: 30, MsPlayed: 60000, RoyaleMatches: 4, RoyalePlaceSum: 10}
	v := c.view()
	if v.PPS != 2.5 || v.APM != 90 || v.VS != 200 {
		t.Errorf("rates pps %v apm %v vs %v, want 2.5 90 200", v.PPS, v.APM, v.VS)
	}
	if v.AvgPlace == nil || *v.AvgPlace != 2.5 {
		t.Errorf("AvgPlace %v, want 2.5", v.AvgPlace)
	}
	z := Career{}.view()
	if z.PPS != 0 || z.APM != 0 || z.VS != 0 || z.AvgPlace != nil {
		t.Errorf("zero view %+v, want zero rates and nil AvgPlace", z)
	}
}

func TestReopenedStoreKeepsSessionsAndData(t *testing.T) {
	path := filepath.Join(t.TempDir(), "reopen.db")
	s, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	acc, token, err := s.Register(ctx, "Sam", "password1")
	if err != nil {
		t.Fatal(err)
	}
	body := "version=1\nmute=1\n"
	if err := s.SaveSettings(ctx, acc.ID, body); err != nil {
		t.Fatal(err)
	}
	if _, err := s.SubmitBest(ctx, acc.ID, "sprint", 61000); err != nil {
		t.Fatal(err)
	}
	delta := Career{DuelWins: 2, Pieces: 100, MaxCombo: 4, MsPlayed: 60000}
	if err := s.AddCareer(ctx, acc.ID, delta); err != nil {
		t.Fatal(err)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}

	s, err = OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	got, err := s.Session(ctx, token)
	if err != nil || got != acc {
		t.Fatalf("Session = %+v, %v, want %+v", got, err, acc)
	}
	p, err := s.Profile(ctx, acc.ID)
	if err != nil {
		t.Fatal(err)
	}
	if p.Settings != body {
		t.Errorf("Settings %q, want %q", p.Settings, body)
	}
	wantBest(t, "sprint", p.Bests.SprintMs, 61000)
	if p.Career != delta {
		t.Errorf("Career %+v, want %+v", p.Career, delta)
	}
}
