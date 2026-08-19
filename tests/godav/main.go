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
// It has gaps of its own: no dead properties, so the props session
// fails against it.  Neither server alone is enough.
//
// -authprefix puts HTTP Basic authentication in front of one subtree,
// so that the sessions which check the .netrc handling have somewhere
// to authenticate against.  Nothing here is meant to be secure: the
// credentials are on the command line and the comparison is a plain
// one.  It is a test server on the loopback interface.
//
// -norangeprefix makes one subtree ignore a Range request and answer
// with the whole resource, which is what neon's ne_get_range() checks
// for only after the body has already gone to the file.  No real server
// here does that, and what it causes is silent corruption of the local
// file, so it has to be arranged deliberately.
//
// -slowprefix throttles the response body under one subtree.  Served
// from a local disk over the loopback interface, a transfer is over
// before anything can happen during it: a Ctrl-C that is meant to abort
// one, a progress indicator, and a throughput measurement all need a
// transfer that lasts.
//
// -escapeprefix makes one subtree name a member outside itself in
// every PROPFIND: an href that unescapes to "../../escaped.txt".
// `rget' writes local files under names the server chose, so a server
// that chooses one like that is the case worth having a test for, and
// no real server produces one.
//
// The prefixes are given without a leading slash, which is added here.
// A leading slash would look like an absolute path to the MSYS2 and Git
// Bash argument conversion, which rewrites one into a Windows path
// before a native program ever sees it.
package main

import (
	"bytes"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"golang.org/x/net/webdav"
)

type authHandler struct {
	next              http.Handler
	prefix            string
	user, pass, realm string
}

// noRangeHandler drops the Range header from any request under its
// prefix, so the handler behind it serves the whole resource with 200
// where the client asked for 206 and a part of it.
type noRangeHandler struct {
	next   http.Handler
	prefix string
}

func (h *noRangeHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if strings.HasPrefix(r.URL.Path, h.prefix) {
		r.Header.Del("Range")
		r.Header.Del("If-Range")
	}

	h.next.ServeHTTP(w, r)
}

// slowHandler paces the response body under its prefix: chunk bytes,
// then delay, until the body is done.  Wrapping the ResponseWriter also
// hides its ReadFrom method, so net/http copies through Write and the
// pacing is not bypassed by sendfile.
type slowHandler struct {
	next   http.Handler
	prefix string
	chunk  int
	delay  time.Duration
}

type slowWriter struct {
	http.ResponseWriter
	chunk int
	delay time.Duration
}

func (w *slowWriter) Write(p []byte) (int, error) {
	written := 0

	for len(p) > 0 {
		n := w.chunk
		if n > len(p) {
			n = len(p)
		}

		m, err := w.ResponseWriter.Write(p[:n])
		written += m
		if err != nil {
			return written, err
		}

		// Without the flush the bytes sit in net/http's buffer and
		// arrive in one burst at the end, which is the thing this is
		// here to avoid.
		if f, ok := w.ResponseWriter.(http.Flusher); ok {
			f.Flush()
		}

		p = p[n:]
		time.Sleep(w.delay)
	}

	return written, nil
}

// slowReader paces a request body the same way, so that an upload
// lasts as long as a download does.  Reading in chunk-sized pieces is
// what makes it take time: the pause happens once per Read, and a
// caller asking for a large buffer would otherwise pause once.
type slowReader struct {
	io.ReadCloser
	chunk int
	delay time.Duration
}

func (r *slowReader) Read(p []byte) (int, error) {
	if len(p) > r.chunk {
		p = p[:r.chunk]
	}

	n, err := r.ReadCloser.Read(p)
	time.Sleep(r.delay)

	return n, err
}

func (h *slowHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if strings.HasPrefix(r.URL.Path, h.prefix) {
		w = &slowWriter{ResponseWriter: w, chunk: h.chunk, delay: h.delay}
		if r.Body != nil {
			r.Body = &slowReader{
				ReadCloser: r.Body,
				chunk:      h.chunk,
				delay:      h.delay,
			}
		}
	}

	h.next.ServeHTTP(w, r)
}

// escapeHandler appends one hostile response to the multistatus the
// handler behind it produced.  Buffering the body is what makes that
// possible, and a PROPFIND body is small.
type escapeHandler struct {
	next   http.Handler
	prefix string
}

type captureWriter struct {
	header http.Header
	status int
	body   bytes.Buffer
}

func (w *captureWriter) Header() http.Header { return w.header }

func (w *captureWriter) WriteHeader(status int) { w.status = status }

func (w *captureWriter) Write(p []byte) (int, error) { return w.body.Write(p) }

// The member: an href that unescapes to a path leaving the collection,
// and one that unescapes to a name with a Windows separator in it.
const escapeResponses = `<D:response><D:href>%s..%%2F..%%2Fescaped.txt` +
	`</D:href><D:propstat><D:prop><D:resourcetype/>` +
	`<D:getcontentlength>5</D:getcontentlength></D:prop>` +
	`<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>` +
	`<D:response><D:href>%ssub%%5Cdir%%5Cout.txt` +
	`</D:href><D:propstat><D:prop><D:resourcetype/>` +
	`<D:getcontentlength>5</D:getcontentlength></D:prop>` +
	`<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>`

func (h *escapeHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != "PROPFIND" || !strings.HasPrefix(r.URL.Path, h.prefix) {
		h.next.ServeHTTP(w, r)
		return
	}

	capture := &captureWriter{header: make(http.Header), status: 200}
	h.next.ServeHTTP(capture, r)

	body := capture.body.String()
	const end = "</D:multistatus>"

	if capture.status == http.StatusMultiStatus &&
		strings.Contains(body, end) {
		base := r.URL.Path
		if !strings.HasSuffix(base, "/") {
			base += "/"
		}
		injected := fmt.Sprintf(escapeResponses, base, base)
		body = strings.Replace(body, end, injected+end, 1)
	}

	for name, values := range capture.header {
		for _, value := range values {
			w.Header().Add(name, value)
		}
	}
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	w.WriteHeader(capture.status)
	_, _ = io.WriteString(w, body)
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
	noRange := flag.String("norangeprefix", "",
		"answer a Range request under /PREFIX with the whole resource")
	slowPrefix := flag.String("slowprefix", "",
		"pace the response body under /PREFIX (given without the slash)")
	slowChunk := flag.Int("slowchunk", 4096,
		"bytes to write between pauses under -slowprefix")
	slowDelay := flag.Duration("slowdelay", 20*time.Millisecond,
		"how long to pause between chunks under -slowprefix")
	escapePrefix := flag.String("escapeprefix", "",
		"name a member outside the collection under /PREFIX")
	flag.Parse()

	var h http.Handler = &webdav.Handler{
		Prefix:     "",
		FileSystem: webdav.Dir(*dir),
		LockSystem: webdav.NewMemLS(),
	}

	// DeltaV and DASL, which x/net/webdav does not implement, so that
	// cadaver's `search' and its version commands have something to
	// talk to.  See deltav.go.
	h = newDeltavHandler(h, *dir)

	if *escapePrefix != "" {
		h = &escapeHandler{
			next:   h,
			prefix: "/" + strings.TrimPrefix(*escapePrefix, "/"),
		}
	}

	if *slowPrefix != "" {
		h = &slowHandler{
			next:   h,
			prefix: "/" + strings.TrimPrefix(*slowPrefix, "/"),
			chunk:  *slowChunk,
			delay:  *slowDelay,
		}
	}

	if *noRange != "" {
		h = &noRangeHandler{
			next:   h,
			prefix: "/" + strings.TrimPrefix(*noRange, "/"),
		}
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
