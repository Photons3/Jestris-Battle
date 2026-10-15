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
	"strings"
)

func main() {
	addr := flag.String("addr", "0.0.0.0:8080", "address to listen on")
	dir := flag.String("dir", "../build-web", "directory holding the web build (index.html, index.js, index.wasm)")
	match := flag.Duration("match", defaultMatchLength, "match length, e.g. 20s for quick manual tests")
	cert := flag.String("cert", "", "TLS certificate file (PEM, full chain); serves HTTPS/WSS when set with -key")
	key := flag.String("key", "", "TLS private key file (PEM)")
	origins := flag.String("origins", "", "comma-separated extra page hosts allowed to open /ws, for proxies that rewrite Host")
	authMode := flag.String("authority", "log", "log: simulate every match and log disagreements; enforce: the simulation decides")
	lead := flag.Duration("lead", defaultLead, "how far a player's game may run ahead of the server's match clock")
	lag := flag.Duration("lag", defaultLag, "how far behind the server's match clock a player's game is forced forward")
	garbageWindow := flag.Duration("garbage-window", defaultGarbageWindow, "time a player has to accept sent garbage")
	maxResyncs := flag.Int("max-resyncs", defaultMaxResyncs, "RESYNCs per 10 s before a player is disconnected (enforce only)")
	flag.Parse()

	if *authMode != "log" && *authMode != "enforce" {
		log.Fatal("-authority must be log or enforce")
	}

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
	web := newWebBuild(*dir)
	if _, _, _, err := web.load(); err != nil {
		log.Fatalf("cannot version the web build in %s: %v", *dir, err)
	}
	_, port, err := net.SplitHostPort(*addr)
	if err != nil {
		log.Fatalf("bad -addr: %v", err)
	}

	mux := http.NewServeMux()
	mux.Handle("/", web)
	hub := NewHub()
	hub.matchLength = *match
	if err := warmEngine(*authMode, newSharedSim); err != nil {
		log.Fatal(err)
	}
	hub.auth = authority{mode: *authMode, lead: *lead, lag: *lag, garbageWindow: *garbageWindow, maxResyncs: *maxResyncs}
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
