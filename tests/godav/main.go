// A minimal WebDAV server built on golang.org/x/net/webdav, used as a
// second opinion when checking cadaver itself.
//
// wsgidav, which tests/wsgidav.sh drives, answers LOCK with the
// Content-Type "application; charset=utf-8".  That is not a media type
// and not an XML one, so neon discards the body unparsed and cadaver
// never sees the lock token: nothing downstream of holding a lock can
// be checked there, which makes wsgidav useless for any change to the
// locking.  x/net/webdav implements locking, including the RFC 4918
// section 7.3 treatment of a LOCK on an unmapped URL.
//
// It has gaps of its own -- no dead properties, so the props session
// fails against it -- which is exactly why neither server alone is
// enough.
//
// -authprefix puts HTTP Basic authentication in front of one subtree,
// so that the sessions which check the .netrc handling have somewhere
// to authenticate against.  Nothing here is meant to be secure: the
// credentials are on the command line and the comparison is a plain
// one.  It is a test server on the loopback interface.
//
// The prefix is given without a leading slash, which is added here.  A
// leading slash would make it look like an absolute path to the MSYS2
// and Git Bash argument conversion, which rewrites one into a Windows
// path before a native program ever sees it.
package main

import (
	"flag"
	"log"
	"net"
	"net/http"
	"strings"

	"golang.org/x/net/webdav"
)

type authHandler struct {
	next              http.Handler
	prefix            string
	user, pass, realm string
}

func (h *authHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if h.prefix == "" || !strings.HasPrefix(r.URL.Path, h.prefix) {
		h.next.ServeHTTP(w, r)
		return
	}

	user, pass, ok := r.BasicAuth()
	if !ok || user != h.user || pass != h.pass {
		w.Header().Set("WWW-Authenticate", `Basic realm="`+h.realm+`"`)
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}

	h.next.ServeHTTP(w, r)
}

func main() {
	addr := flag.String("addr", "127.0.0.1:8909", "listen address")
	dir := flag.String("dir", ".", "directory to serve")
	authPrefix := flag.String("authprefix", "",
		"require Basic auth under /PREFIX (given without the slash)")
	authUser := flag.String("authuser", "", "user name Basic auth accepts")
	authPass := flag.String("authpass", "", "password Basic auth accepts")
	authRealm := flag.String("authrealm", "cadaver-test",
		"realm named in the challenge")
	flag.Parse()

	var h http.Handler = &webdav.Handler{
		Prefix:     "",
		FileSystem: webdav.Dir(*dir),
		LockSystem: webdav.NewMemLS(),
	}

	if *authPrefix != "" {
		h = &authHandler{
			next:   h,
			prefix: "/" + strings.TrimPrefix(*authPrefix, "/"),
			user:   *authUser,
			pass:   *authPass,
			realm:  *authRealm,
		}
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
