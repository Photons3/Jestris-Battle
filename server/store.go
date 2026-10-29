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

var errBadMode = errors.New("bad mode")

// Bests are the solo bests. A mode never played is nil.
type Bests struct {
	SprintMs   *int `json:"sprintMs"`
	CheeseMs   *int `json:"cheeseMs"`
	BlitzScore *int `json:"blitzScore"`
}

// Career is the stored career totals, and also the delta one match adds to them.
type Career struct {
	DuelWins, DuelLosses, DuelDraws, KosDealt, KosTaken, RoyaleMatches, RoyaleWins, RoyalePlaceSum,
	Pieces, Attack, Sent, GarbageCleared, Cancelled, Tetrises, TSpins, MaxCombo, MsPlayed int
}

// Profile is everything an account owns.
type Profile struct {
	Username, Settings string
	Bests              Bests
	Career             Career
}

// careerView is the career as the menu shows it, with the rates computed.
type careerView struct {
	DuelWins      int      `json:"duelWins"`
	DuelLosses    int      `json:"duelLosses"`
	DuelDraws     int      `json:"duelDraws"`
	KosDealt      int      `json:"kosDealt"`
	KosTaken      int      `json:"kosTaken"`
	RoyaleMatches int      `json:"royaleMatches"`
	RoyaleWins    int      `json:"royaleWins"`
	AvgPlace      *float64 `json:"avgPlace"`
	PPS           float64  `json:"pps"`
	APM           float64  `json:"apm"`
	VS            float64  `json:"vs"`
}

func (c Career) view() careerView {
	v := careerView{
		DuelWins: c.DuelWins, DuelLosses: c.DuelLosses, DuelDraws: c.DuelDraws,
		KosDealt: c.KosDealt, KosTaken: c.KosTaken,
		RoyaleMatches: c.RoyaleMatches, RoyaleWins: c.RoyaleWins,
	}
	if c.RoyaleMatches > 0 {
		avg := float64(c.RoyalePlaceSum) / float64(c.RoyaleMatches)
		v.AvgPlace = &avg
	}
	if c.MsPlayed > 0 {
		ms := float64(c.MsPlayed)
		v.PPS = float64(c.Pieces) / (ms / 1000)
		v.APM = float64(c.Attack) / (ms / 60000)
		v.VS = float64(c.Attack+c.GarbageCleared) / (ms / 1000) * 100
	}
	return v
}

// Profile returns the account's name, settings text, solo bests and career totals.
func (s *Store) Profile(ctx context.Context, userID int64) (Profile, error) {
	var p Profile
	c := &p.Career
	err := s.db.QueryRowContext(ctx, `
SELECT u.username, st.body, b.sprint_ms, b.cheese_ms, b.blitz_score,
       c.duel_wins, c.duel_losses, c.duel_draws, c.kos_dealt, c.kos_taken, c.royale_matches, c.royale_wins,
       c.royale_place_sum, c.pieces, c.attack, c.sent, c.garbage_cleared, c.cancelled, c.tetrises, c.tspins,
       c.max_combo, c.ms_played
FROM users u
JOIN settings st ON st.user_id = u.id
JOIN solo_bests b ON b.user_id = u.id
JOIN career c ON c.user_id = u.id
WHERE u.id = ?`, userID).Scan(&p.Username, &p.Settings, &p.Bests.SprintMs, &p.Bests.CheeseMs, &p.Bests.BlitzScore,
		&c.DuelWins, &c.DuelLosses, &c.DuelDraws, &c.KosDealt, &c.KosTaken, &c.RoyaleMatches, &c.RoyaleWins,
		&c.RoyalePlaceSum, &c.Pieces, &c.Attack, &c.Sent, &c.GarbageCleared, &c.Cancelled, &c.Tetrises, &c.TSpins,
		&c.MaxCombo, &c.MsPlayed)
	return p, err
}

// SaveSettings stores the settings text as is.
func (s *Store) SaveSettings(ctx context.Context, userID int64, body string) error {
	_, err := s.db.ExecContext(ctx, "UPDATE settings SET body = ? WHERE user_id = ?", body, userID)
	return err
}

// bestUpdates replace a best only when the new result is better: sprint and cheese are times, blitz is a score.
var bestUpdates = map[string]string{
	"sprint": "UPDATE solo_bests SET sprint_ms = ? WHERE user_id = ? AND (sprint_ms IS NULL OR sprint_ms > ?)",
	"cheese": "UPDATE solo_bests SET cheese_ms = ? WHERE user_id = ? AND (cheese_ms IS NULL OR cheese_ms > ?)",
	"blitz":  "UPDATE solo_bests SET blitz_score = ? WHERE user_id = ? AND (blitz_score IS NULL OR blitz_score < ?)",
}

// SubmitBest records a solo result when it beats the stored best, and returns the three stored bests.
func (s *Store) SubmitBest(ctx context.Context, userID int64, mode string, result int) (Bests, error) {
	q, ok := bestUpdates[mode]
	if !ok || result < 0 {
		return Bests{}, errBadMode
	}
	if _, err := s.db.ExecContext(ctx, q, result, userID, result); err != nil {
		return Bests{}, err
	}
	var b Bests
	err := s.db.QueryRowContext(ctx, "SELECT sprint_ms, cheese_ms, blitz_score FROM solo_bests WHERE user_id = ?", userID).
		Scan(&b.SprintMs, &b.CheeseMs, &b.BlitzScore)
	return b, err
}

// AddCareer adds a match's delta to the totals. max_combo takes the greater value, every other column sums.
func (s *Store) AddCareer(ctx context.Context, userID int64, d Career) error {
	_, err := s.db.ExecContext(ctx, `
UPDATE career SET
  duel_wins = duel_wins + ?, duel_losses = duel_losses + ?, duel_draws = duel_draws + ?,
  kos_dealt = kos_dealt + ?, kos_taken = kos_taken + ?,
  royale_matches = royale_matches + ?, royale_wins = royale_wins + ?, royale_place_sum = royale_place_sum + ?,
  pieces = pieces + ?, attack = attack + ?, sent = sent + ?, garbage_cleared = garbage_cleared + ?,
  cancelled = cancelled + ?, tetrises = tetrises + ?, tspins = tspins + ?,
  max_combo = MAX(max_combo, ?), ms_played = ms_played + ?
WHERE user_id = ?`,
		d.DuelWins, d.DuelLosses, d.DuelDraws, d.KosDealt, d.KosTaken,
		d.RoyaleMatches, d.RoyaleWins, d.RoyalePlaceSum,
		d.Pieces, d.Attack, d.Sent, d.GarbageCleared, d.Cancelled, d.Tetrises, d.TSpins,
		d.MaxCombo, d.MsPlayed, userID)
	return err
}
