package main

import (
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

const (
	maxAPIBodyBytes = 16 << 10
	sessionMaxAge   = 30 * 24 * 60 * 60

	loginFailureLimit  = 10
	loginFailureWindow = 15 * time.Minute
	registrationLimit  = 5
	registrationWindow = time.Hour
)

// accountAPI is the same-origin JSON API for accounts, mounted next to the page and /ws.
type accountAPI struct {
	store *Store
	hub   *Hub
	now   func() time.Time
	// logins counts failed logins per lower-cased username, registrations counts accounts created per client IP.
	logins, registrations *rateLimiter
}

func newAccountAPI(store *Store, hub *Hub) *accountAPI {
	a := &accountAPI{store: store, hub: hub, now: time.Now}
	now := func() time.Time { return a.now() }
	a.logins = newRateLimiter(loginFailureLimit, loginFailureWindow, now)
	a.registrations = newRateLimiter(registrationLimit, registrationWindow, now)
	return a
}

func (a *accountAPI) routes(mux *http.ServeMux) {
	mux.HandleFunc("POST /api/register", a.register)
	mux.HandleFunc("POST /api/login", a.login)
	mux.HandleFunc("POST /api/logout", a.logout)
	mux.HandleFunc("GET /api/me", a.me)
	mux.HandleFunc("PUT /api/settings", a.settings)
	mux.HandleFunc("POST /api/password", a.password)
	mux.HandleFunc("POST /api/bests", a.bests)
}

// meBody is the body of GET /api/me, and of a successful register or login.
type meBody struct {
	Username   string     `json:"username"`
	Settings   string     `json:"settings"`
	SprintMs   *int       `json:"sprintMs"`
	CheeseMs   *int       `json:"cheeseMs"`
	BlitzScore *int       `json:"blitzScore"`
	Career     careerView `json:"career"`
}

type credentials struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

func (a *accountAPI) register(w http.ResponseWriter, r *http.Request) {
	var c credentials
	if !readJSON(w, r, &c) {
		return
	}
	ip := clientIP(r)
	if !a.registrations.reserve(ip) {
		http.Error(w, "try again later", http.StatusTooManyRequests)
		return
	}
	acc, token, err := a.store.Register(r.Context(), c.Username, c.Password)
	a.registrations.settle(ip, err == nil)
	switch {
	case errors.Is(err, errNameTaken):
		http.Error(w, "username is taken", http.StatusConflict)
		return
	case errors.Is(err, errBadName):
		http.Error(w, "username must be 1 to 12 of A-Z a-z 0-9 _ - .", http.StatusConflict)
		return
	case errors.Is(err, errBadPassword):
		http.Error(w, "password must be 8 to 72 bytes", http.StatusBadRequest)
		return
	case err != nil:
		serverError(w, "register", err)
		return
	}
	a.startSession(w, r, acc, token)
}

func (a *accountAPI) login(w http.ResponseWriter, r *http.Request) {
	var c credentials
	if !readJSON(w, r, &c) {
		return
	}
	// No account can have such a name, so it never reaches the limiter, whose keys would
	// otherwise be as large as the attacker's body.
	if !validName(c.Username) {
		http.Error(w, "wrong username or password", http.StatusUnauthorized)
		return
	}
	key := strings.ToLower(c.Username)
	if !a.logins.reserve(key) {
		http.Error(w, "try again later", http.StatusTooManyRequests)
		return
	}
	acc, token, err := a.store.Login(r.Context(), c.Username, c.Password)
	a.logins.settle(key, errors.Is(err, errWrongLogin))
	switch {
	case errors.Is(err, errWrongLogin):
		http.Error(w, "wrong username or password", http.StatusUnauthorized)
		return
	case err != nil:
		serverError(w, "login", err)
		return
	}
	a.startSession(w, r, acc, token)
}

// startSession sets the cookie for token and answers with the account's profile.
func (a *accountAPI) startSession(w http.ResponseWriter, r *http.Request, acc Account, token string) {
	http.SetCookie(w, &http.Cookie{
		Name:     sessionCookie,
		Value:    token,
		Path:     "/",
		MaxAge:   sessionMaxAge,
		HttpOnly: true,
		SameSite: http.SameSiteLaxMode,
		Secure:   secureRequest(r),
	})
	a.writeProfile(w, r, acc)
}

func (a *accountAPI) logout(w http.ResponseWriter, r *http.Request) {
	acc, token, ok := a.session(w, r)
	if !ok {
		return
	}
	if err := a.store.Logout(r.Context(), token); err != nil {
		serverError(w, "logout", err)
		return
	}
	http.SetCookie(w, &http.Cookie{
		Name:     sessionCookie,
		Path:     "/",
		MaxAge:   -1,
		HttpOnly: true,
		SameSite: http.SameSiteLaxMode,
		Secure:   secureRequest(r),
	})
	a.hub.DisconnectAccount(acc.ID)
	w.WriteHeader(http.StatusNoContent)
}

func (a *accountAPI) me(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	acc, _, ok := a.session(w, r)
	if !ok {
		return
	}
	a.writeProfile(w, r, acc)
}

func (a *accountAPI) settings(w http.ResponseWriter, r *http.Request) {
	acc, _, ok := a.session(w, r)
	if !ok {
		return
	}
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxAPIBodyBytes))
	if err != nil {
		http.Error(w, "bad request", http.StatusBadRequest)
		return
	}
	if err := a.store.SaveSettings(r.Context(), acc.ID, string(body)); err != nil {
		serverError(w, "save settings", err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *accountAPI) password(w http.ResponseWriter, r *http.Request) {
	acc, token, ok := a.session(w, r)
	if !ok {
		return
	}
	var req struct {
		Current string `json:"current"`
		Next    string `json:"next"`
	}
	if !readJSON(w, r, &req) {
		return
	}
	err := a.store.ChangePassword(r.Context(), acc.ID, token, req.Current, req.Next)
	switch {
	case errors.Is(err, errWrongLogin):
		http.Error(w, "wrong password", http.StatusForbidden)
		return
	case errors.Is(err, errBadPassword):
		http.Error(w, "password must be 8 to 72 bytes", http.StatusBadRequest)
		return
	case err != nil:
		serverError(w, "change password", err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *accountAPI) bests(w http.ResponseWriter, r *http.Request) {
	acc, _, ok := a.session(w, r)
	if !ok {
		return
	}
	var req struct {
		Mode   string `json:"mode"`
		Result int    `json:"result"`
	}
	if !readJSON(w, r, &req) {
		return
	}
	b, err := a.store.SubmitBest(r.Context(), acc.ID, req.Mode, req.Result)
	switch {
	case errors.Is(err, errBadMode):
		http.Error(w, "bad request", http.StatusBadRequest)
		return
	case err != nil:
		serverError(w, "submit best", err)
		return
	}
	writeJSON(w, b)
}

// session resolves the request's cookie. On failure it has already answered 401 (or 500) and ok is false.
func (a *accountAPI) session(w http.ResponseWriter, r *http.Request) (acc Account, token string, ok bool) {
	c, err := r.Cookie(sessionCookie)
	if err != nil {
		http.Error(w, "not logged in", http.StatusUnauthorized)
		return Account{}, "", false
	}
	acc, err = a.store.Session(r.Context(), c.Value)
	if errors.Is(err, errNoSession) {
		http.Error(w, "not logged in", http.StatusUnauthorized)
		return Account{}, "", false
	}
	if err != nil {
		serverError(w, "session lookup", err)
		return Account{}, "", false
	}
	return acc, c.Value, true
}

func (a *accountAPI) writeProfile(w http.ResponseWriter, r *http.Request, acc Account) {
	p, err := a.store.Profile(r.Context(), acc.ID)
	if err != nil {
		serverError(w, "profile", err)
		return
	}
	writeJSON(w, meBody{
		Username:   p.Username,
		Settings:   p.Settings,
		SprintMs:   p.Bests.SprintMs,
		CheeseMs:   p.Bests.CheeseMs,
		BlitzScore: p.Bests.BlitzScore,
		Career:     p.Career.view(),
	})
}

// readJSON decodes the capped body into v, answering 400 when it cannot.
func readJSON(w http.ResponseWriter, r *http.Request, v any) bool {
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, maxAPIBodyBytes)).Decode(v); err != nil {
		http.Error(w, "bad request", http.StatusBadRequest)
		return false
	}
	return true
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	if err := json.NewEncoder(w).Encode(v); err != nil {
		log.Printf("write json: %v", err)
	}
}

func serverError(w http.ResponseWriter, what string, err error) {
	log.Printf("%s: %v", what, err)
	http.Error(w, "internal error", http.StatusInternalServerError)
}

func secureRequest(r *http.Request) bool {
	return r.TLS != nil || r.Header.Get("X-Forwarded-Proto") == "https"
}

// clientIP is the first X-Forwarded-For entry when the TCP peer is loopback (the proxy on this machine),
// and the peer's own address otherwise, so a remote client cannot pick the address it is counted under.
func clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		host = r.RemoteAddr
	}
	if ip := net.ParseIP(host); ip != nil && ip.IsLoopback() {
		first, _, _ := strings.Cut(r.Header.Get("X-Forwarded-For"), ",")
		if first = strings.TrimSpace(first); net.ParseIP(first) != nil {
			return first
		}
	}
	return host
}

// rateLimiter allows max events per key within window, remembering them in memory only.
// A caller reserves a slot before the work and settles it after, so parallel requests cannot all pass the check.
type rateLimiter struct {
	mu        sync.Mutex
	max       int
	window    time.Duration
	now       func() time.Time
	keys      map[string]*limitKey
	lastSweep time.Time
}

type limitKey struct {
	events   []time.Time // recorded events, oldest first
	inflight int         // reservations not yet settled
}

func newRateLimiter(max int, window time.Duration, now func() time.Time) *rateLimiter {
	return &rateLimiter{max: max, window: window, now: now, keys: map[string]*limitKey{}}
}

// expire drops k's events older than the window. Caller holds mu.
func (l *rateLimiter) expire(k *limitKey, now time.Time) {
	cutoff := now.Add(-l.window)
	i := 0
	for i < len(k.events) && !k.events[i].After(cutoff) {
		i++
	}
	k.events = k.events[i:]
}

// sweep forgets every key with no live events and no reservation, at most once per window,
// so the map holds only the keys that were active in the last window. Caller holds mu.
func (l *rateLimiter) sweep(now time.Time) {
	if d := now.Sub(l.lastSweep); d >= 0 && d < l.window {
		return
	}
	l.lastSweep = now
	for key, k := range l.keys {
		l.expire(k, now)
		if len(k.events) == 0 && k.inflight == 0 {
			delete(l.keys, key)
		}
	}
}

// reserve claims a slot for key, or reports false when the events in the window plus the
// reservations in flight already reach the limit. Every true must be followed by one settle.
func (l *rateLimiter) reserve(key string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := l.now()
	l.sweep(now)
	k := l.keys[key]
	if k == nil {
		k = &limitKey{}
	} else {
		l.expire(k, now)
	}
	if len(k.events)+k.inflight >= l.max {
		return false
	}
	k.inflight++
	l.keys[key] = k
	return true
}

// settle releases a reserved slot, and records an event for key when counted.
func (l *rateLimiter) settle(key string, counted bool) {
	l.mu.Lock()
	defer l.mu.Unlock()
	k := l.keys[key]
	if k == nil {
		return
	}
	k.inflight--
	if counted {
		k.events = append(k.events, l.now())
	}
	if len(k.events) == 0 && k.inflight == 0 {
		delete(l.keys, key)
	}
}
