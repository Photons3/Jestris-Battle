package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"path/filepath"
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
