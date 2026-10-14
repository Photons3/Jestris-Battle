package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"sync"
	"time"
)

// webFileNames are the only files of the web build that are served.
var webFileNames = []string{"index.html", "index.js", "index.wasm"}

// scriptSrc matches how emcc's {{{ SCRIPT }}} names index.js in index.html. Release builds
// of newer emsdk minify the page and drop the quotes (src=index.js), so any quoting matches;
// the character after an unquoted value is kept as the second group.
var scriptSrc = regexp.MustCompile(`\bsrc=(?:"index\.js"|'index\.js'|index\.js([\s/>]))`)

// webBuild serves just the web build's own files from dir, so the rest of the build
// directory (CMakeCache.txt, _deps/, listings) is not published to the LAN.
//
// index.html is always revalidated, and it asks for index.js?v=<version>, where the version
// hashes index.js and index.wasm (the page's locateFile puts the same ?v= on index.wasm).
// A request carrying the current version can then be cached for good: the next build has
// a new version, so browsers fetch it under a new URL. The version is recomputed whenever
// a file's size or mod time changes, so rebuilding under a running server is fine too.
type webBuild struct {
	dir string

	mu      sync.Mutex
	stamp   string // sizes and mod times the fields below were computed from
	version string
	page    []byte // index.html with the versioned script tag
	etag    string
}

func newWebBuild(dir string) *webBuild { return &webBuild{dir: dir} }

// load returns the current version and versioned index.html, recomputing them if the build changed.
func (b *webBuild) load() (version string, page []byte, etag string, err error) {
	var stamp strings.Builder
	for _, name := range webFileNames {
		fi, err := os.Stat(filepath.Join(b.dir, name))
		if err != nil {
			return "", nil, "", err
		}
		fmt.Fprintf(&stamp, "%d/%d;", fi.Size(), fi.ModTime().UnixNano())
	}

	b.mu.Lock()
	defer b.mu.Unlock()
	if stamp.String() == b.stamp {
		return b.version, b.page, b.etag, nil
	}
	html, err := os.ReadFile(filepath.Join(b.dir, "index.html"))
	if err != nil {
		return "", nil, "", err
	}
	if n := len(scriptSrc.FindAllIndex(html, -1)); n != 1 {
		return "", nil, "", fmt.Errorf("index.html names index.js in %d script tags, want 1", n)
	}
	h := sha256.New()
	for _, name := range []string{"index.js", "index.wasm"} {
		if err := hashFile(h, filepath.Join(b.dir, name)); err != nil {
			return "", nil, "", err
		}
	}
	version = hex.EncodeToString(h.Sum(nil))[:16]
	page = scriptSrc.ReplaceAll(html, []byte(`src="index.js?v=`+version+`"$1`))
	sum := sha256.Sum256(page)
	b.stamp, b.version, b.page, b.etag = stamp.String(), version, page, `"`+hex.EncodeToString(sum[:8])+`"`
	return b.version, b.page, b.etag, nil
}

func hashFile(w io.Writer, path string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	_, err = io.Copy(w, f)
	return err
}

func (b *webBuild) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	name := strings.TrimPrefix(r.URL.Path, "/")
	if name == "" {
		name = "index.html"
	}
	if !slices.Contains(webFileNames, name) {
		http.NotFound(w, r)
		return
	}
	version, page, etag, err := b.load()
	if err != nil {
		log.Printf("serving the web build without versioning: %v", err)
	}

	if name == "index.html" {
		w.Header().Set("Cache-Control", "no-cache")
		if err == nil {
			w.Header().Set("ETag", etag)
			http.ServeContent(w, r, name, time.Time{}, bytes.NewReader(page))
			return
		}
		// ServeFile redirects any request path ending in /index.html to ./
		r = r.Clone(r.Context())
		r.URL.Path = "/"
	} else if err == nil && r.URL.Query().Get("v") == version {
		w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	} else {
		// Unversioned or stale URL: make the browser revalidate rather than guess.
		w.Header().Set("Cache-Control", "no-cache")
	}
	http.ServeFile(w, r, filepath.Join(b.dir, name))
}
