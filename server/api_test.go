package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/coder/websocket"
)

// newAPIServer serves the account API and /ws over a temp store; the hub resolves cookies through the same store.
func newAPIServer(t *testing.T) (*httptest.Server, *accountAPI) {
	t.Helper()
	store := openTestStore(t)
	hub := NewHub()
	hub.sessions = store
	api := newAccountAPI(store, hub)
	mux := http.NewServeMux()
	api.routes(mux)
	mux.Handle("/ws", hub)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv, api
}

// newBrowser is an HTTP client that keeps cookies, like a browser tab.
func newBrowser(t *testing.T) *http.Client {
	t.Helper()
	jar, err := cookiejar.New(nil)
	if err != nil {
		t.Fatal(err)
	}
	return &http.Client{Jar: jar}
}

// call sends the request and returns the status and the body without its trailing newline.
func call(t *testing.T, c *http.Client, method, url, body string) (int, string) {
	t.Helper()
	req, err := http.NewRequest(method, url, strings.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	resp, err := c.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, strings.TrimRight(string(data), "\n")
}

func creds(user, pass string) string {
	b, _ := json.Marshal(map[string]string{"username": user, "password": pass})
	return string(b)
}

// register and login expect a 200 and return the decoded profile.
func register(t *testing.T, c *http.Client, srv *httptest.Server, user, pass string) meBody {
	t.Helper()
	return okMe(t, c, "POST", srv.URL+"/api/register", creds(user, pass))
}

func login(t *testing.T, c *http.Client, srv *httptest.Server, user, pass string) meBody {
	t.Helper()
	return okMe(t, c, "POST", srv.URL+"/api/login", creds(user, pass))
}

func okMe(t *testing.T, c *http.Client, method, url, body string) meBody {
	t.Helper()
	status, got := call(t, c, method, url, body)
	if status != 200 {
		t.Fatalf("%s %s: status %d %q", method, url, status, got)
	}
	var m meBody
	if err := json.Unmarshal([]byte(got), &m); err != nil {
		t.Fatalf("%s %s: body %q: %v", method, url, got, err)
	}
	return m
}

func expectCall(t *testing.T, c *http.Client, method, url, body string, wantStatus int, wantBody string) {
	t.Helper()
	status, got := call(t, c, method, url, body)
	if status != wantStatus || got != wantBody {
		t.Fatalf("%s %s %s: got %d %q, want %d %q", method, url, body, status, got, wantStatus, wantBody)
	}
}

func TestRegisterLoginMeLogout(t *testing.T) {
	srv, _ := newAPIServer(t)
	resp, err := http.Post(srv.URL+"/api/register", "application/json", strings.NewReader(creds("Sam", "password1")))
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("register status %d", resp.StatusCode)
	}
	var cookie *http.Cookie
	for _, c := range resp.Cookies() {
		if c.Name == sessionCookie {
			cookie = c
		}
	}
	if cookie == nil {
		t.Fatal("no jestris_session cookie")
	}
	if !cookie.HttpOnly || cookie.SameSite != http.SameSiteLaxMode || cookie.Path != "/" || cookie.MaxAge != 2592000 || cookie.Secure {
		t.Fatalf("cookie %+v", cookie)
	}
	if len(cookie.Value) != 64 {
		t.Fatalf("cookie value %q", cookie.Value)
	}

	a := newBrowser(t)
	if m := register(t, a, srv, "Tom", "password1"); m.Username != "Tom" {
		t.Fatalf("register: %+v", m)
	}
	status, body := call(t, a, "GET", srv.URL+"/api/me", "")
	if status != 200 {
		t.Fatalf("me status %d %q", status, body)
	}
	for _, want := range []string{
		`"username":"Tom"`, `"settings":""`, `"sprintMs":null`, `"cheeseMs":null`, `"blitzScore":null`,
		`"career":{`, `"avgPlace":null`, `"pps":0`, `"apm":0`, `"vs":0`,
	} {
		if !strings.Contains(body, want) {
			t.Fatalf("me body %s lacks %s", body, want)
		}
	}
	expectCall(t, a, "POST", srv.URL+"/api/logout", "", 204, "")
	expectCall(t, a, "GET", srv.URL+"/api/me", "", 401, "not logged in")

	b := newBrowser(t)
	if m := login(t, b, srv, "tom", "password1"); m.Username != "Tom" {
		t.Fatalf("login: %+v", m)
	}
	if status, _ := call(t, b, "GET", srv.URL+"/api/me", ""); status != 200 {
		t.Fatalf("me after login: %d", status)
	}
}

func TestRegisterErrors(t *testing.T) {
	srv, _ := newAPIServer(t)
	c := newBrowser(t)
	register(t, c, srv, "Sam", "password1")
	d := newBrowser(t)
	expectCall(t, d, "POST", srv.URL+"/api/register", creds("SAM", "password1"), 409, "username is taken")
	expectCall(t, d, "POST", srv.URL+"/api/register", creds("bad name", "password1"), 409, "username must be 1 to 12 of A-Z a-z 0-9 _ - .")
	expectCall(t, d, "POST", srv.URL+"/api/register", creds("Ann", "short"), 400, "password must be 8 to 72 bytes")
	expectCall(t, d, "POST", srv.URL+"/api/register", "{", 400, "bad request")
}

func TestLoginMismatchAndUnknownUserLookTheSame(t *testing.T) {
	srv, _ := newAPIServer(t)
	register(t, newBrowser(t), srv, "Sam", "password1")
	c := newBrowser(t)
	expectCall(t, c, "POST", srv.URL+"/api/login", creds("Sam", "wrongpass1"), 401, "wrong username or password")
	expectCall(t, c, "POST", srv.URL+"/api/login", creds("Nobody", "password1"), 401, "wrong username or password")
}

func TestSecureCookieBehindHTTPS(t *testing.T) {
	srv, _ := newAPIServer(t)
	req, _ := http.NewRequest("POST", srv.URL+"/api/register", strings.NewReader(creds("Sam", "password1")))
	req.Header.Set("X-Forwarded-Proto", "https")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	for _, c := range resp.Cookies() {
		if c.Name == sessionCookie {
			if !c.Secure {
				t.Fatalf("cookie is not Secure: %+v", c)
			}
			return
		}
	}
	t.Fatal("no jestris_session cookie")
}

func TestSettingsRoundTrip(t *testing.T) {
	srv, _ := newAPIServer(t)
	c := newBrowser(t)
	register(t, c, srv, "Sam", "password1")
	const text = "version=1\nsound=off\n"
	expectCall(t, c, "PUT", srv.URL+"/api/settings", text, 204, "")
	if m := okMe(t, c, "GET", srv.URL+"/api/me", ""); m.Settings != text {
		t.Fatalf("settings %q", m.Settings)
	}
}

func TestBestsKeepOnlyBetterResults(t *testing.T) {
	srv, _ := newAPIServer(t)
	c := newBrowser(t)
	register(t, c, srv, "Sam", "password1")
	url := srv.URL + "/api/bests"
	expectCall(t, c, "POST", url, `{"mode":"sprint","result":60000}`, 200, `{"sprintMs":60000,"cheeseMs":null,"blitzScore":null}`)
	expectCall(t, c, "POST", url, `{"mode":"sprint","result":65000}`, 200, `{"sprintMs":60000,"cheeseMs":null,"blitzScore":null}`)
	expectCall(t, c, "POST", url, `{"mode":"blitz","result":9000}`, 200, `{"sprintMs":60000,"cheeseMs":null,"blitzScore":9000}`)
	expectCall(t, c, "POST", url, `{"mode":"blitz","result":8000}`, 200, `{"sprintMs":60000,"cheeseMs":null,"blitzScore":9000}`)
	expectCall(t, c, "POST", url, `{"mode":"zen","result":1}`, 400, "bad request")
	expectCall(t, c, "POST", url, `{"mode":"sprint","result":-1}`, 400, "bad request")
}

func TestPasswordChange(t *testing.T) {
	srv, _ := newAPIServer(t)
	a, b := newBrowser(t), newBrowser(t)
	register(t, a, srv, "Sam", "password1")
	login(t, b, srv, "Sam", "password1")

	url := srv.URL + "/api/password"
	expectCall(t, a, "POST", url, `{"current":"nope-nope","next":"newpass12"}`, 403, "wrong password")
	expectCall(t, a, "POST", url, `{"current":"password1","next":"short"}`, 400, "password must be 8 to 72 bytes")
	expectCall(t, a, "POST", url, `{"current":"password1","next":"newpass12"}`, 204, "")
	if status, _ := call(t, a, "GET", srv.URL+"/api/me", ""); status != 200 {
		t.Fatalf("A's me: %d", status)
	}
	expectCall(t, b, "GET", srv.URL+"/api/me", "", 401, "not logged in")

	c := newBrowser(t)
	expectCall(t, c, "POST", srv.URL+"/api/login", creds("Sam", "password1"), 401, "wrong username or password")
	login(t, c, srv, "Sam", "newpass12")
}

func TestLoggedInRoutesNeedASession(t *testing.T) {
	srv, _ := newAPIServer(t)
	c := newBrowser(t)
	for _, r := range []struct{ method, path, body string }{
		{"POST", "/api/logout", ""},
		{"GET", "/api/me", ""},
		{"PUT", "/api/settings", "version=1\n"},
		{"POST", "/api/password", `{"current":"password1","next":"newpass12"}`},
		{"POST", "/api/bests", `{"mode":"sprint","result":1}`},
	} {
		expectCall(t, c, r.method, srv.URL+r.path, r.body, 401, "not logged in")
	}
}

func TestLogoutClosesTheAccountsSocket(t *testing.T) {
	srv, _ := newAPIServer(t)
	c := newBrowser(t)
	register(t, c, srv, "Sam", "password1")

	u, _ := url.Parse(srv.URL)
	header := http.Header{}
	for _, ck := range c.Jar.Cookies(u) {
		header.Add("Cookie", ck.String())
	}
	if header.Get("Cookie") == "" {
		t.Fatal("no cookie in the jar")
	}
	dctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(dctx, "ws"+strings.TrimPrefix(srv.URL, "http")+"/ws", &websocket.DialOptions{HTTPHeader: header})
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	t.Cleanup(func() { conn.CloseNow() })
	ws := &client{t: t, conn: conn}
	ws.send("LIST")
	ws.expect("ROOMS")

	expectCall(t, c, "POST", srv.URL+"/api/logout", "", 204, "")
	ws.expectClosed()
}

// settableClock is a settable clock that the handlers and the test can both touch.
type settableClock struct {
	mu sync.Mutex
	t  time.Time
}

func (f *settableClock) now() time.Time {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.t
}

func (f *settableClock) advance(d time.Duration) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.t = f.t.Add(d)
}

func TestLoginFailuresAreCappedPerUsername(t *testing.T) {
	srv, api := newAPIServer(t)
	clock := &settableClock{t: time.Date(2026, 10, 29, 12, 0, 0, 0, time.UTC)}
	api.now = clock.now
	register(t, newBrowser(t), srv, "Sam", "password1")
	register(t, newBrowser(t), srv, "Ann", "password1")

	c := newBrowser(t)
	for i := 0; i < 10; i++ {
		expectCall(t, c, "POST", srv.URL+"/api/login", creds("sam", "wrongpass1"), 401, "wrong username or password")
	}
	expectCall(t, c, "POST", srv.URL+"/api/login", creds("sam", "password1"), 429, "try again later")
	expectCall(t, c, "POST", srv.URL+"/api/login", creds("SAM", "password1"), 429, "try again later")
	login(t, newBrowser(t), srv, "Ann", "password1")

	clock.advance(15 * time.Minute)
	login(t, newBrowser(t), srv, "Sam", "password1")
}

func TestRegistrationsAreCappedPerIP(t *testing.T) {
	srv, api := newAPIServer(t)
	clock := &settableClock{t: time.Date(2026, 10, 29, 12, 0, 0, 0, time.UTC)}
	api.now = clock.now
	names := []string{"a1", "a2", "a3", "a4", "a5", "a6", "a7"}
	c := newBrowser(t)
	for _, n := range names[:5] {
		register(t, c, srv, n, "password1")
	}
	expectCall(t, c, "POST", srv.URL+"/api/register", creds(names[5], "password1"), 429, "try again later")
	clock.advance(time.Hour)
	register(t, c, srv, names[5], "password1")
}

func TestClientIP(t *testing.T) {
	const forwarded = "203.0.113.9, 10.0.0.1"
	for _, c := range []struct{ remote, header, want string }{
		{"127.0.0.1:5000", forwarded, "203.0.113.9"},
		{"[::1]:5000", forwarded, "203.0.113.9"},
		{"198.51.100.7:5000", forwarded, "198.51.100.7"},
		{"127.0.0.1:5000", "", "127.0.0.1"},
	} {
		r := httptest.NewRequest("GET", "/", nil)
		r.RemoteAddr = c.remote
		if c.header != "" {
			r.Header.Set("X-Forwarded-For", c.header)
		}
		if got := clientIP(r); got != c.want {
			t.Errorf("clientIP(%s, %q) = %q, want %q", c.remote, c.header, got, c.want)
		}
	}
}
