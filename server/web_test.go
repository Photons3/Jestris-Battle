package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"regexp"
	"testing"
	"time"
)

const testPage = `<html><script async type="text/javascript" src="index.js"></script></html>`

// writeBuild writes a fake web build into dir with the given index.wasm contents.
func writeBuild(t *testing.T, dir, wasm string) {
	t.Helper()
	files := map[string]string{"index.html": testPage, "index.js": "js", "index.wasm": wasm, "CMakeCache.txt": "x"}
	for name, body := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func get(t *testing.T, url string, header ...string) (*http.Response, string) {
	t.Helper()
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i+1 < len(header); i += 2 {
		req.Header.Set(header[i], header[i+1])
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	return resp, string(body)
}

var versionRE = regexp.MustCompile(`src="index\.js\?v=([0-9a-f]+)"`)

// pageVersion fetches the page and returns the version its script tag carries.
func pageVersion(t *testing.T, base string) string {
	t.Helper()
	resp, body := get(t, base+"/")
	if got := resp.Header.Get("Cache-Control"); got != "no-cache" {
		t.Errorf("page Cache-Control %q, want no-cache", got)
	}
	m := versionRE.FindStringSubmatch(body)
	if m == nil {
		t.Fatalf("page has no versioned script tag: %s", body)
	}
	return m[1]
}

func TestWebFilesServesOnlyTheAllowlist(t *testing.T) {
	dir := t.TempDir()
	writeBuild(t, dir, "wasm")
	if err := os.Mkdir(filepath.Join(dir, "_deps"), 0o755); err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(newWebBuild(dir))
	t.Cleanup(srv.Close)

	for _, path := range []string{"/", "/index.html", "/index.js", "/index.wasm"} {
		resp, _ := get(t, srv.URL+path)
		if resp.StatusCode != http.StatusOK {
			t.Errorf("GET %s: status %d, want 200", path, resp.StatusCode)
		}
	}
	resp, _ := get(t, srv.URL+"/index.wasm")
	if got := resp.Header.Get("Content-Type"); got != "application/wasm" {
		t.Errorf("index.wasm Content-Type %q, want application/wasm", got)
	}
	resp, _ = get(t, srv.URL+"/")
	if got := resp.Header.Get("Content-Type"); got != "text/html; charset=utf-8" {
		t.Errorf("page Content-Type %q, want text/html", got)
	}
	for _, path := range []string{"/CMakeCache.txt", "/_deps/", "/_deps"} {
		if resp, _ := get(t, srv.URL+path); resp.StatusCode != http.StatusNotFound {
			t.Errorf("GET %s: status %d, want 404", path, resp.StatusCode)
		}
	}
}

func TestOnlyCurrentVersionIsCachedForGood(t *testing.T) {
	dir := t.TempDir()
	writeBuild(t, dir, "wasm")
	srv := httptest.NewServer(newWebBuild(dir))
	t.Cleanup(srv.Close)
	v := pageVersion(t, srv.URL)

	cases := map[string]string{
		"/index.js?v=" + v:     "public, max-age=31536000, immutable",
		"/index.wasm?v=" + v:   "public, max-age=31536000, immutable",
		"/index.js":            "no-cache",
		"/index.wasm":          "no-cache",
		"/index.wasm?v=stale1": "no-cache",
	}
	for path, want := range cases {
		resp, _ := get(t, srv.URL+path)
		if got := resp.Header.Get("Cache-Control"); got != want {
			t.Errorf("GET %s: Cache-Control %q, want %q", path, got, want)
		}
	}
}

func TestNewBuildGetsNewVersion(t *testing.T) {
	dir := t.TempDir()
	writeBuild(t, dir, "wasm")
	srv := httptest.NewServer(newWebBuild(dir))
	t.Cleanup(srv.Close)
	old := pageVersion(t, srv.URL)

	writeBuild(t, dir, "new wasm")
	later := time.Now().Add(time.Minute)
	if err := os.Chtimes(filepath.Join(dir, "index.wasm"), later, later); err != nil {
		t.Fatal(err)
	}
	if v := pageVersion(t, srv.URL); v == old {
		t.Fatalf("version stayed %s after index.wasm changed", v)
	}
	resp, _ := get(t, srv.URL+"/index.wasm?v="+old)
	if got := resp.Header.Get("Cache-Control"); got != "no-cache" {
		t.Errorf("old version Cache-Control %q, want no-cache", got)
	}
}

func TestPageRevalidatesWithETag(t *testing.T) {
	dir := t.TempDir()
	writeBuild(t, dir, "wasm")
	srv := httptest.NewServer(newWebBuild(dir))
	t.Cleanup(srv.Close)

	resp, _ := get(t, srv.URL+"/")
	etag := resp.Header.Get("ETag")
	if etag == "" {
		t.Fatal("page has no ETag")
	}
	if resp, _ := get(t, srv.URL+"/", "If-None-Match", etag); resp.StatusCode != http.StatusNotModified {
		t.Errorf("revalidation status %d, want 304", resp.StatusCode)
	}
}

func TestPageWithoutScriptTagIsServedUnversioned(t *testing.T) {
	dir := t.TempDir()
	writeBuild(t, dir, "wasm")
	if err := os.WriteFile(filepath.Join(dir, "index.html"), []byte("<html></html>"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, _, _, err := newWebBuild(dir).load(); err == nil {
		t.Error("load accepted a page without the script tag")
	}
	srv := httptest.NewServer(newWebBuild(dir))
	t.Cleanup(srv.Close)
	resp, body := get(t, srv.URL+"/")
	if resp.StatusCode != http.StatusOK || body != "<html></html>" {
		t.Errorf("got %d %q, want the page as is", resp.StatusCode, body)
	}
	if got := resp.Header.Get("Cache-Control"); got != "no-cache" {
		t.Errorf("Cache-Control %q, want no-cache", got)
	}
}

// Release builds of newer emsdk minify index.html, dropping the quotes around attributes.
func TestMinifiedScriptTagIsVersioned(t *testing.T) {
	for _, tag := range []string{`<script async src=index.js></script>`, `<script src='index.js'></script>`} {
		dir := t.TempDir()
		writeBuild(t, dir, "wasm")
		if err := os.WriteFile(filepath.Join(dir, "index.html"), []byte("<html>"+tag+"</html>"), 0o644); err != nil {
			t.Fatal(err)
		}
		_, page, _, err := newWebBuild(dir).load()
		if err != nil {
			t.Fatalf("%s: %v", tag, err)
		}
		if !versionRE.Match(page) {
			t.Errorf("%s: page has no versioned script tag: %s", tag, page)
		}
	}
}
