package main

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"time"

	"golang.org/x/crypto/bcrypt"
	"modernc.org/sqlite"
	sqlite3 "modernc.org/sqlite/lib"
)

const (
	minPasswordBytes = 8
	maxPasswordBytes = 72 // bcrypt's own limit
)

var (
	errBadName     = errors.New("bad username")
	errNameTaken   = errors.New("username is taken")
	errBadPassword = errors.New("bad password")
	errWrongLogin  = errors.New("wrong username or password")
	errNoSession   = errors.New("no session")
)

// dummyHash lets Login spend the same bcrypt time on an unknown user as on a wrong password.
var dummyHash, _ = bcrypt.GenerateFromPassword([]byte("jestris-dummy-password"), bcrypt.DefaultCost)

const schema = `
CREATE TABLE IF NOT EXISTS users (
  id            INTEGER PRIMARY KEY,
  username      TEXT NOT NULL COLLATE NOCASE UNIQUE,
  password_hash TEXT NOT NULL,
  created_at    TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS sessions (
  token_hash TEXT PRIMARY KEY,
  user_id    INTEGER NOT NULL REFERENCES users(id),
  created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS settings (
  user_id INTEGER PRIMARY KEY REFERENCES users(id),
  body    TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS solo_bests (
  user_id     INTEGER PRIMARY KEY REFERENCES users(id),
  sprint_ms   INTEGER,
  cheese_ms   INTEGER,
  blitz_score INTEGER
);

CREATE TABLE IF NOT EXISTS career (
  user_id          INTEGER PRIMARY KEY REFERENCES users(id),
  duel_wins        INTEGER NOT NULL DEFAULT 0,
  duel_losses      INTEGER NOT NULL DEFAULT 0,
  duel_draws       INTEGER NOT NULL DEFAULT 0,
  kos_dealt        INTEGER NOT NULL DEFAULT 0,
  kos_taken        INTEGER NOT NULL DEFAULT 0,
  royale_matches   INTEGER NOT NULL DEFAULT 0,
  royale_wins      INTEGER NOT NULL DEFAULT 0,
  royale_place_sum INTEGER NOT NULL DEFAULT 0,
  pieces           INTEGER NOT NULL DEFAULT 0,
  attack           INTEGER NOT NULL DEFAULT 0,
  sent             INTEGER NOT NULL DEFAULT 0,
  garbage_cleared  INTEGER NOT NULL DEFAULT 0,
  cancelled        INTEGER NOT NULL DEFAULT 0,
  tetrises         INTEGER NOT NULL DEFAULT 0,
  tspins           INTEGER NOT NULL DEFAULT 0,
  max_combo        INTEGER NOT NULL DEFAULT 0,
  ms_played        INTEGER NOT NULL DEFAULT 0
);
`

// Store is the SQLite database of accounts and what they own.
type Store struct {
	db  *sql.DB
	now func() time.Time
}

// Account is a registered player. Username is as registered, whatever case the login used.
type Account struct {
	ID       int64
	Username string
}

// OpenStore opens (creating if missing) the database at path, in WAL mode, with all tables present.
func OpenStore(path string) (*Store, error) {
	db, err := sql.Open("sqlite", "file:"+path+"?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)&_pragma=foreign_keys(1)")
	if err != nil {
		return nil, err
	}
	// One connection, so writers never see SQLITE_BUSY.
	db.SetMaxOpenConns(1)
	if _, err := db.Exec(schema); err != nil {
		db.Close()
		return nil, err
	}
	return &Store{db: db, now: time.Now}, nil
}

func (s *Store) Close() error { return s.db.Close() }

func (s *Store) stamp() string { return s.now().UTC().Format(time.RFC3339) }

func validPassword(p string) bool { return len(p) >= minPasswordBytes && len(p) <= maxPasswordBytes }

// newToken returns a random cookie token (64 hex characters) and the hash stored for it.
func newToken() (token, hash string, err error) {
	raw := make([]byte, 32)
	if _, err = rand.Read(raw); err != nil {
		return "", "", err
	}
	sum := sha256.Sum256(raw)
	return hex.EncodeToString(raw), hex.EncodeToString(sum[:]), nil
}

// tokenHash is the stored hash of a cookie token, or false when the token is not 64 hex characters.
func tokenHash(token string) (string, bool) {
	if len(token) != 64 {
		return "", false
	}
	raw, err := hex.DecodeString(token)
	if err != nil {
		return "", false
	}
	sum := sha256.Sum256(raw)
	return hex.EncodeToString(sum[:]), true
}

// Register creates an account with its empty settings, bests and career rows, and logs it in.
func (s *Store) Register(ctx context.Context, username, password string) (Account, string, error) {
	if !validName(username) {
		return Account{}, "", errBadName
	}
	if !validPassword(password) {
		return Account{}, "", errBadPassword
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
	if err != nil {
		return Account{}, "", err
	}
	token, tHash, err := newToken()
	if err != nil {
		return Account{}, "", err
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Account{}, "", err
	}
	defer tx.Rollback()
	now := s.stamp()
	res, err := tx.ExecContext(ctx, "INSERT INTO users (username, password_hash, created_at) VALUES (?, ?, ?)", username, string(hash), now)
	if err != nil {
		var se *sqlite.Error
		if errors.As(err, &se) && se.Code() == sqlite3.SQLITE_CONSTRAINT_UNIQUE {
			return Account{}, "", errNameTaken
		}
		return Account{}, "", err
	}
	id, err := res.LastInsertId()
	if err != nil {
		return Account{}, "", err
	}
	for _, q := range []string{
		"INSERT INTO settings (user_id) VALUES (?)",
		"INSERT INTO solo_bests (user_id) VALUES (?)",
		"INSERT INTO career (user_id) VALUES (?)",
	} {
		if _, err := tx.ExecContext(ctx, q, id); err != nil {
			return Account{}, "", err
		}
	}
	if _, err := tx.ExecContext(ctx, "INSERT INTO sessions (token_hash, user_id, created_at) VALUES (?, ?, ?)", tHash, id, now); err != nil {
		return Account{}, "", err
	}
	if err := tx.Commit(); err != nil {
		return Account{}, "", err
	}
	return Account{ID: id, Username: username}, token, nil
}

// Login checks the credentials, case-insensitively on the name, and starts a new session.
func (s *Store) Login(ctx context.Context, username, password string) (Account, string, error) {
	var acc Account
	var hash string
	err := s.db.QueryRowContext(ctx, "SELECT id, username, password_hash FROM users WHERE username = ?", username).Scan(&acc.ID, &acc.Username, &hash)
	if errors.Is(err, sql.ErrNoRows) {
		bcrypt.CompareHashAndPassword(dummyHash, []byte(password))
		return Account{}, "", errWrongLogin
	}
	if err != nil {
		return Account{}, "", err
	}
	if bcrypt.CompareHashAndPassword([]byte(hash), []byte(password)) != nil {
		return Account{}, "", errWrongLogin
	}
	token, tHash, err := newToken()
	if err != nil {
		return Account{}, "", err
	}
	if _, err := s.db.ExecContext(ctx, "INSERT INTO sessions (token_hash, user_id, created_at) VALUES (?, ?, ?)", tHash, acc.ID, s.stamp()); err != nil {
		return Account{}, "", err
	}
	return acc, token, nil
}

// Session resolves a cookie token to its account.
func (s *Store) Session(ctx context.Context, token string) (Account, error) {
	hash, ok := tokenHash(token)
	if !ok {
		return Account{}, errNoSession
	}
	var acc Account
	err := s.db.QueryRowContext(ctx, "SELECT u.id, u.username FROM sessions s JOIN users u ON u.id = s.user_id WHERE s.token_hash = ?", hash).Scan(&acc.ID, &acc.Username)
	if errors.Is(err, sql.ErrNoRows) {
		return Account{}, errNoSession
	}
	return acc, err
}

// Logout ends the session. An unknown token is not an error.
func (s *Store) Logout(ctx context.Context, token string) error {
	hash, ok := tokenHash(token)
	if !ok {
		return nil
	}
	_, err := s.db.ExecContext(ctx, "DELETE FROM sessions WHERE token_hash = ?", hash)
	return err
}

// ChangePassword sets a new password and ends every session of the account except keepToken's.
func (s *Store) ChangePassword(ctx context.Context, userID int64, keepToken, current, next string) error {
	if !validPassword(next) {
		return errBadPassword
	}
	var hash string
	if err := s.db.QueryRowContext(ctx, "SELECT password_hash FROM users WHERE id = ?", userID).Scan(&hash); err != nil {
		return err
	}
	if bcrypt.CompareHashAndPassword([]byte(hash), []byte(current)) != nil {
		return errWrongLogin
	}
	newHash, err := bcrypt.GenerateFromPassword([]byte(next), bcrypt.DefaultCost)
	if err != nil {
		return err
	}
	keep, _ := tokenHash(keepToken)
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.ExecContext(ctx, "UPDATE users SET password_hash = ? WHERE id = ?", string(newHash), userID); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, "DELETE FROM sessions WHERE user_id = ? AND token_hash != ?", userID, keep); err != nil {
		return err
	}
	return tx.Commit()
}
