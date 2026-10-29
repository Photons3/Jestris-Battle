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
	if a.registrations.blocked(ip) {
		http.Error(w, "try again later", http.StatusTooManyRequests)
		return
	}
	acc, token, err := a.store.Register(r.Context(), c.Username, c.Password)
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
	a.registrations.add(ip)
	a.startSession(w, r, acc, token)
}

func (a *accountAPI) login(w http.ResponseWriter, r *http.Request) {
	var c credentials
	if !readJSON(w, r, &c) {
		return
	}
	key := strings.ToLower(c.Username)
	if a.logins.blocked(key) {
		http.Error(w, "try again later", http.StatusTooManyRequests)
		return
	}
	acc, token, err := a.store.Login(r.Context(), c.Username, c.Password)
	switch {
	case errors.Is(err, errWrongLogin):
		a.logins.add(key)
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
type rateLimiter struct {
	mu     sync.Mutex
	max    int
	window time.Duration
	now    func() time.Time
	events map[string][]time.Time
}

func newRateLimiter(max int, window time.Duration, now func() time.Time) *rateLimiter {
	return &rateLimiter{max: max, window: window, now: now, events: map[string][]time.Time{}}
}

// prune drops key's events older than the window, and the key itself once none is left. Caller holds mu.
func (l *rateLimiter) prune(key string) []time.Time {
	cutoff := l.now().Add(-l.window)
	list := l.events[key]
	i := 0
	for i < len(list) && !list[i].After(cutoff) {
		i++
	}
	list = list[i:]
	if len(list) == 0 {
		delete(l.events, key)
		return nil
	}
	l.events[key] = list
	return list
}

// blocked reports whether key has already used up its events in the window.
func (l *rateLimiter) blocked(key string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.prune(key)) >= l.max
}

// add counts one event for key.
func (l *rateLimiter) add(key string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.events[key] = append(l.prune(key), l.now())
}
