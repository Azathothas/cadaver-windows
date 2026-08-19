// A minimal WebDAV server built on golang.org/x/net/webdav, used as a
// second opinion when checking litmus itself.
//
// wsgidav, which tests/wsgidav.sh drives, answers LOCK with the
// Content-Type "application; charset=utf-8".  That is not a media type
// and not an XML one, so neon discards the body unparsed and litmus
// never sees the lock token: every test downstream of holding a lock
// is skipped there, which makes wsgidav useless for checking any change
// to the lock tests.  x/net/webdav implements locking, including the
// RFC 4918 section 7.3 treatment of a LOCK on an unmapped URL.
//
// It has gaps of its own -- no dead properties, so the props suite
// fails against it -- which is exactly why neither server alone is
// enough.
package main

import (
	"flag"
	"log"
	"net"
	"net/http"

	"golang.org/x/net/webdav"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:8909", "listen address")
	dir := flag.String("dir", ".", "directory to serve")
	flag.Parse()

	h := &webdav.Handler{
		Prefix:     "",
		FileSystem: webdav.Dir(*dir),
		LockSystem: webdav.NewMemLS(),
	}

	// Bind first and announce afterwards, so the "listening on" line is
	// a real readiness signal: a script that waits for it in the log
	// knows the port is accepting connections.  http.ListenAndServe
	// would have to be raced against instead.
	ln, err := net.Listen("tcp", *addr)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("serving %s, listening on %s", *dir, ln.Addr())
	log.Fatal(http.Serve(ln, h))
}
