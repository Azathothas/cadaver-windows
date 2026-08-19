// DeltaV and DASL in front of x/net/webdav, which implements neither.
//
// cadaver's `search', `version', `checkin', `checkout', `uncheckout',
// `history' and `label' were compiled, linked and never once exercised:
// neither test server answers any of those methods, so nothing checked
// what cadaver sends or what it does with the answer.  The first run
// against this found that every DeltaV command was putting a trailing
// slash on the path of a plain resource.
//
// This is not an implementation of RFC 3253 or RFC 5323.  It is enough
// of one to answer what cadaver asks, in the shape the specifications
// give, so that the client side is exercised end to end.  Versions are
// held in memory and go when the server does.
package main

import (
	"encoding/xml"
	"fmt"
	"io"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// One version of one resource: the body as it was when the version was
// made, and the labels put on it.
type version struct {
	name    string
	created time.Time
	body    []byte
	labels  []string
}

type resourceVersions struct {
	versions   []*version
	checkedOut bool
}

type deltavHandler struct {
	next http.Handler
	dir  string

	mu    sync.Mutex
	state map[string]*resourceVersions
}

func newDeltavHandler(next http.Handler, dir string) *deltavHandler {
	return &deltavHandler{
		next:  next,
		dir:   dir,
		state: make(map[string]*resourceVersions),
	}
}

// The file behind a request path, or an error if it is outside the
// served directory or is not there.
func (h *deltavHandler) file(urlPath string) (string, os.FileInfo, error) {
	clean := path.Clean("/" + strings.TrimSuffix(urlPath, "/"))
	name := filepath.Join(h.dir, filepath.FromSlash(clean))

	info, err := os.Stat(name)
	if err != nil {
		return "", nil, err
	}

	return name, info, nil
}

func (h *deltavHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case "VERSION-CONTROL":
		h.versionControl(w, r)
	case "CHECKOUT":
		h.checkout(w, r)
	case "CHECKIN":
		h.checkin(w, r)
	case "UNCHECKOUT":
		h.uncheckout(w, r)
	case "REPORT":
		h.report(w, r)
	case "LABEL":
		h.label(w, r)
	case "SEARCH":
		h.search(w, r)
	case "OPTIONS":
		// A client is entitled to assume none of this is here unless
		// the OPTIONS response says otherwise.
		h.announce(w)
		h.next.ServeHTTP(w, r)
	default:
		h.next.ServeHTTP(w, r)
	}
}

// The DAV classes and the search grammar this understands.
func (h *deltavHandler) announce(w http.ResponseWriter) {
	w.Header().Add("DAV", "version-control,checkout-in-place,label")
	w.Header().Add("DASL", "<DAV:basicsearch>")
}

func (h *deltavHandler) versionControl(w http.ResponseWriter, r *http.Request) {
	name, _, err := h.file(r.URL.Path)
	if err != nil {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}

	body, err := os.ReadFile(name)
	if err != nil {
		http.Error(w, err.Error(), http.StatusForbidden)
		return
	}

	key := path.Clean("/" + strings.TrimSuffix(r.URL.Path, "/"))

	h.mu.Lock()
	defer h.mu.Unlock()

	if _, seen := h.state[key]; seen {
		// RFC 3253 section 3.5: already under version control is not
		// an error.
		w.WriteHeader(http.StatusOK)
		return
	}

	h.state[key] = &resourceVersions{
		versions: []*version{{
			name:    "1",
			created: time.Now().UTC(),
			body:    body,
		}},
	}

	w.WriteHeader(http.StatusCreated)
}

// The state of a versioned resource, or nil with the response already
// written.
func (h *deltavHandler) versioned(w http.ResponseWriter, r *http.Request) (string, *resourceVersions) {
	key := path.Clean("/" + strings.TrimSuffix(r.URL.Path, "/"))

	rv, ok := h.state[key]
	if !ok {
		http.Error(w, "not under version control",
			http.StatusForbidden)
		return "", nil
	}

	return key, rv
}

func (h *deltavHandler) checkout(w http.ResponseWriter, r *http.Request) {
	h.mu.Lock()
	defer h.mu.Unlock()

	_, rv := h.versioned(w, r)
	if rv == nil {
		return
	}

	if rv.checkedOut {
		http.Error(w, "already checked out", http.StatusConflict)
		return
	}

	rv.checkedOut = true
	w.WriteHeader(http.StatusOK)
}

func (h *deltavHandler) checkin(w http.ResponseWriter, r *http.Request) {
	name, _, err := h.file(r.URL.Path)
	if err != nil {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}

	body, err := os.ReadFile(name)
	if err != nil {
		http.Error(w, err.Error(), http.StatusForbidden)
		return
	}

	h.mu.Lock()
	defer h.mu.Unlock()

	key, rv := h.versioned(w, r)
	if rv == nil {
		return
	}

	if !rv.checkedOut {
		http.Error(w, "not checked out", http.StatusConflict)
		return
	}

	next := strconv.Itoa(len(rv.versions) + 1)
	rv.versions = append(rv.versions, &version{
		name:    next,
		created: time.Now().UTC(),
		body:    body,
	})
	rv.checkedOut = false

	w.Header().Set("Location", key+"?version="+next)
	w.WriteHeader(http.StatusCreated)
}

func (h *deltavHandler) uncheckout(w http.ResponseWriter, r *http.Request) {
	name, _, err := h.file(r.URL.Path)
	if err != nil {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}

	h.mu.Lock()
	defer h.mu.Unlock()

	_, rv := h.versioned(w, r)
	if rv == nil {
		return
	}

	if !rv.checkedOut {
		http.Error(w, "not checked out", http.StatusConflict)
		return
	}

	// Put the body back to the version that was checked out, which is
	// the whole point of the method.
	last := rv.versions[len(rv.versions)-1]
	if err := os.WriteFile(name, last.body, 0o644); err != nil {
		http.Error(w, err.Error(), http.StatusForbidden)
		return
	}

	rv.checkedOut = false
	w.WriteHeader(http.StatusOK)
}

func (h *deltavHandler) report(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	if !strings.Contains(string(body), "version-tree") {
		http.Error(w, "only DAV:version-tree is supported here",
			http.StatusForbidden)
		return
	}

	h.mu.Lock()
	defer h.mu.Unlock()

	key, rv := h.versioned(w, r)
	if rv == nil {
		return
	}

	var out strings.Builder
	out.WriteString(xml.Header)
	out.WriteString(`<D:multistatus xmlns:D="DAV:">`)

	for _, v := range rv.versions {
		fmt.Fprintf(&out, `<D:response><D:href>%s?version=%s</D:href>`+
			`<D:propstat><D:prop>`+
			`<D:version-name>%s</D:version-name>`+
			`<D:creator-displayname>%s</D:creator-displayname>`+
			`<D:getcontentlength>%d</D:getcontentlength>`+
			`<D:getlastmodified>%s</D:getlastmodified>`+
			`</D:prop><D:status>HTTP/1.1 200 OK</D:status>`+
			`</D:propstat></D:response>`,
			escape(key), escape(v.name), escape(v.name), "cadaver-test",
			len(v.body), v.created.Format(http.TimeFormat))
	}

	out.WriteString(`</D:multistatus>`)

	w.Header().Set("Content-Type", "application/xml; charset=utf-8")
	w.WriteHeader(http.StatusMultiStatus)
	_, _ = io.WriteString(w, out.String())
}

// The body of a LABEL request: exactly one of add, set or remove.
type labelRequest struct {
	XMLName xml.Name `xml:"DAV: label"`
	Add     *struct {
		Name string `xml:"DAV: label-name"`
	} `xml:"DAV: add"`
	Set *struct {
		Name string `xml:"DAV: label-name"`
	} `xml:"DAV: set"`
	Remove *struct {
		Name string `xml:"DAV: label-name"`
	} `xml:"DAV: remove"`
}

func (h *deltavHandler) label(w http.ResponseWriter, r *http.Request) {
	var req labelRequest

	if err := xml.NewDecoder(io.LimitReader(r.Body, 1<<20)).Decode(&req); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	h.mu.Lock()
	defer h.mu.Unlock()

	_, rv := h.versioned(w, r)
	if rv == nil {
		return
	}

	target := rv.versions[len(rv.versions)-1]

	switch {
	case req.Add != nil:
		for _, have := range target.labels {
			if have == req.Add.Name {
				http.Error(w, "label already present",
					http.StatusConflict)
				return
			}
		}
		target.labels = append(target.labels, req.Add.Name)
	case req.Set != nil:
		target.labels = []string{req.Set.Name}
	case req.Remove != nil:
		kept := target.labels[:0]
		found := false
		for _, have := range target.labels {
			if have == req.Remove.Name {
				found = true
				continue
			}
			kept = append(kept, have)
		}
		if !found {
			http.Error(w, "no such label", http.StatusConflict)
			return
		}
		target.labels = kept
	default:
		http.Error(w, "label request names no action",
			http.StatusBadRequest)
		return
	}

	w.WriteHeader(http.StatusOK)
}

func escape(s string) string {
	var out strings.Builder
	_ = xml.EscapeText(&out, []byte(s))
	return out.String()
}

// --- DASL ----------------------------------------------------------

// The parts of a DAV:basicsearch this understands.  A condition is a
// tree: and, or and not join comparisons of one property against one
// literal.
type searchRequest struct {
	XMLName xml.Name `xml:"DAV: searchrequest"`
	Basic   struct {
		From struct {
			Scope struct {
				Href  string `xml:"DAV: href"`
				Depth string `xml:"DAV: depth"`
			} `xml:"DAV: scope"`
		} `xml:"DAV: from"`
		Where condition `xml:"DAV: where"`
	} `xml:"DAV: basicsearch"`
}

// A node of the where clause, decoded by hand: the operator is the
// element name, so a struct with fixed field names cannot express it.
type condition struct {
	op       string
	prop     string
	literal  string
	children []condition
}

func (c *condition) UnmarshalXML(d *xml.Decoder, start xml.StartElement) error {
	c.op = start.Name.Local

	for {
		token, err := d.Token()
		if err != nil {
			return err
		}

		switch t := token.(type) {
		case xml.StartElement:
			switch t.Name.Local {
			case "prop":
				// <D:prop><D:getcontentlength/></D:prop>
				for {
					inner, err := d.Token()
					if err != nil {
						return err
					}
					if s, ok := inner.(xml.StartElement); ok {
						c.prop = s.Name.Local
						if err := d.Skip(); err != nil {
							return err
						}
					}
					if e, ok := inner.(xml.EndElement); ok &&
						e.Name.Local == "prop" {
						break
					}
				}
			case "literal":
				var text string
				if err := d.DecodeElement(&text, &t); err != nil {
					return err
				}
				c.literal = text
			default:
				var child condition
				if err := child.UnmarshalXML(d, t); err != nil {
					return err
				}
				c.children = append(c.children, child)
			}
		case xml.EndElement:
			if t.Name.Local == start.Name.Local {
				return nil
			}
		}
	}
}

// What one candidate resource offers a search.
type candidate struct {
	href     string
	name     string
	size     int64
	modified time.Time
	isDir    bool
}

func (c *condition) match(res candidate) bool {
	switch c.op {
	case "where":
		// The wrapper: a where with nothing in it matches everything.
		if len(c.children) == 0 {
			return true
		}
		return c.children[0].match(res)
	case "and":
		for i := range c.children {
			if !c.children[i].match(res) {
				return false
			}
		}
		return true
	case "or":
		for i := range c.children {
			if c.children[i].match(res) {
				return true
			}
		}
		return false
	case "not":
		return len(c.children) > 0 && !c.children[0].match(res)
	case "like":
		return likeMatch(res.value(c.prop), c.literal)
	case "eq", "lt", "gt", "lte", "gte":
		return compare(c.op, res.value(c.prop), c.literal)
	default:
		return false
	}
}

func (c candidate) value(prop string) string {
	switch prop {
	case "displayname":
		return c.name
	case "getcontentlength":
		return strconv.FormatInt(c.size, 10)
	case "getlastmodified":
		return c.modified.Format(http.TimeFormat)
	case "resourcetype":
		if c.isDir {
			return "collection"
		}
		return ""
	default:
		return ""
	}
}

// A comparison that is numeric when both sides are numbers and
// lexicographic otherwise.  A client asking for
// "getcontentlength < 100" means the number.
func compare(op, left, right string) bool {
	order := 0

	l, lerr := strconv.ParseInt(left, 10, 64)
	r, rerr := strconv.ParseInt(right, 10, 64)

	if lerr == nil && rerr == nil {
		switch {
		case l < r:
			order = -1
		case l > r:
			order = 1
		}
	} else {
		order = strings.Compare(left, right)
	}

	switch op {
	case "eq":
		return order == 0
	case "lt":
		return order < 0
	case "gt":
		return order > 0
	case "lte":
		return order <= 0
	case "gte":
		return order >= 0
	}

	return false
}

// The SQL-style pattern DAV:like takes: % for any run of characters and
// _ for one.  Translated to a walk rather than to a regular expression,
// because the pattern comes from a client and a regular expression
// built from one is a way to hang a server.
func likeMatch(value, pattern string) bool {
	if pattern == "" {
		return value == ""
	}

	if pattern[0] == '%' {
		for i := 0; i <= len(value); i++ {
			if likeMatch(value[i:], pattern[1:]) {
				return true
			}
		}
		return false
	}

	if value == "" {
		return false
	}

	if pattern[0] == '_' || pattern[0] == value[0] {
		return likeMatch(value[1:], pattern[1:])
	}

	return false
}

// Enough of a media type for a search result to carry one, which is
// what cadaver shows at the end of each line.
func contentType(c candidate) string {
	if c.isDir {
		return "httpd/unix-directory"
	}

	switch strings.ToLower(path.Ext(c.name)) {
	case ".txt":
		return "text/plain"
	case ".html", ".htm":
		return "text/html"
	case ".xml":
		return "application/xml"
	default:
		return "application/octet-stream"
	}
}

func (h *deltavHandler) search(w http.ResponseWriter, r *http.Request) {
	var req searchRequest

	if err := xml.NewDecoder(io.LimitReader(r.Body, 1<<20)).Decode(&req); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	scope := req.Basic.From.Scope.Href
	if scope == "" {
		scope = r.URL.Path
	}
	if !strings.HasSuffix(scope, "/") {
		scope += "/"
	}

	root := filepath.Join(h.dir, filepath.FromSlash(path.Clean("/"+scope)))

	var found []candidate

	err := filepath.Walk(root, func(name string, info os.FileInfo, err error) error {
		if err != nil {
			return nil
		}

		rel, rerr := filepath.Rel(root, name)
		if rerr != nil || rel == "." {
			return nil
		}

		if req.Basic.From.Scope.Depth == "0" ||
			(req.Basic.From.Scope.Depth == "1" &&
				strings.Contains(filepath.ToSlash(rel), "/")) {
			if info.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}

		href := scope + filepath.ToSlash(rel)
		if info.IsDir() {
			href += "/"
		}

		found = append(found, candidate{
			href:     href,
			name:     info.Name(),
			size:     info.Size(),
			modified: info.ModTime().UTC(),
			isDir:    info.IsDir(),
		})

		return nil
	})
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	// One order, so that a transcript of one run reads like the next.
	sort.Slice(found, func(i, j int) bool {
		return found[i].href < found[j].href
	})

	var out strings.Builder
	out.WriteString(xml.Header)
	out.WriteString(`<D:multistatus xmlns:D="DAV:">`)

	for _, res := range found {
		if !req.Basic.Where.match(res) {
			continue
		}

		fmt.Fprintf(&out, `<D:response><D:href>%s</D:href>`+
			`<D:propstat><D:prop>`+
			`<D:displayname>%s</D:displayname>`+
			`<D:getcontentlength>%d</D:getcontentlength>`+
			`<D:getlastmodified>%s</D:getlastmodified>`+
			`<D:getcontenttype>%s</D:getcontenttype>`,
			escape(res.href), escape(res.name), res.size,
			res.modified.Format(http.TimeFormat),
			escape(contentType(res)))

		if res.isDir {
			out.WriteString(`<D:resourcetype><D:collection/></D:resourcetype>`)
		} else {
			out.WriteString(`<D:resourcetype/>`)
		}

		out.WriteString(`</D:prop><D:status>HTTP/1.1 200 OK</D:status>` +
			`</D:propstat></D:response>`)
	}

	out.WriteString(`</D:multistatus>`)

	w.Header().Set("Content-Type", "application/xml; charset=utf-8")
	w.WriteHeader(http.StatusMultiStatus)
	_, _ = io.WriteString(w, out.String())
}
