// Command tetris-server hosts duels: it serves the web build and relays matches over /ws.
// It speaks plain HTTP/WS by default, HTTPS/WSS with -cert and -key, and also works behind
// a TLS-terminating reverse proxy (the client picks wss:// whenever the page is https://).
package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"strings"
)

func main() {
	addr := flag.String("addr", "0.0.0.0:8080", "address to listen on")
	dir := flag.String("dir", "../build-web", "directory holding the web build (index.html, index.js, index.wasm)")
	match := flag.Duration("match", defaultMatchLength, "match length, e.g. 20s for quick manual tests")
	cert := flag.String("cert", "", "TLS certificate file (PEM, full chain); serves HTTPS/WSS when set with -key")
	key := flag.String("key", "", "TLS private key file (PEM)")
	origins := flag.String("origins", "", "comma-separated extra page hosts allowed to open /ws, for proxies that rewrite Host")
	flag.Parse()

	if (*cert == "") != (*key == "") {
		log.Fatal("-cert and -key must be given together")
	}
	useTLS := *cert != ""

	for _, name := range webFileNames {
		if _, err := os.Stat(filepath.Join(*dir, name)); err != nil {
			log.Fatalf("%s not found in %s; build the web version first:\n"+
				"  emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web\n"+
				"(if emcc complains about Python, prefix with EMSDK_PYTHON=/opt/homebrew/bin/python3.14)", name, *dir)
		}
	}
	_, port, err := net.SplitHostPort(*addr)
	if err != nil {
		log.Fatalf("bad -addr: %v", err)
	}

	mux := http.NewServeMux()
	mux.Handle("/", webFiles(*dir))
	hub := NewHub()
	hub.matchLength = *match
	for _, o := range strings.Split(*origins, ",") {
		if o = strings.TrimSpace(o); o != "" {
			hub.originPatterns = append(hub.originPatterns, o)
		}
	}
	mux.Handle("/ws", hub)

	scheme := "http"
	if useTLS {
		scheme = "https"
	}
	fmt.Println("Jestris Battle server running. Players open one of:")
	for _, ip := range lanIPs() {
		fmt.Printf("  %s://%s:%s\n", scheme, ip, port)
	}
	fmt.Printf("  %s://localhost:%s  (this machine)\n", scheme, port)
	if useTLS {
		log.Fatal(http.ListenAndServeTLS(*addr, *cert, *key, mux))
	}
	log.Fatal(http.ListenAndServe(*addr, mux))
}

// webFileNames are the only files of the web build that are served.
var webFileNames = []string{"index.html", "index.js", "index.wasm"}

// webFiles serves just the web build's own files from dir, so the rest of the build
// directory (CMakeCache.txt, _deps/, listings) is not published to the LAN.
func webFiles(dir string) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		name := strings.TrimPrefix(r.URL.Path, "/")
		if name == "" {
			name = "index.html"
		}
		if !slices.Contains(webFileNames, name) {
			http.NotFound(w, r)
			return
		}
		if name == "index.html" {
			// ServeFile redirects any request path ending in /index.html to ./
			r = r.Clone(r.Context())
			r.URL.Path = "/"
		}
		// Without this, browsers cache index.js and index.wasm heuristically and keep
		// running the old build after a deploy. no-cache still allows a cheap 304
		// revalidation through Last-Modified.
		w.Header().Set("Cache-Control", "no-cache")
		http.ServeFile(w, r, filepath.Join(dir, name))
	})
}

// lanIPs lists this machine's IPv4 addresses that other devices on the network can reach.
func lanIPs() []string {
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return nil
	}
	var ips []string
	for _, a := range addrs {
		n, ok := a.(*net.IPNet)
		if !ok || n.IP.To4() == nil || n.IP.IsLoopback() || n.IP.IsLinkLocalUnicast() {
			continue
		}
		ips = append(ips, n.IP.String())
	}
	return ips
}
