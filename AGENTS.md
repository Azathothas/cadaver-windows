# Driving cadaver from a program

This is written for an agent that needs to move files over WebDAV and
wants to use cadaver to do it. It assumes you can run commands and read
text output. For build instructions see [README.md](README.md); this
file is about using the tool.

The short version:

1. Get a binary (below), or build one.
2. Put the commands in a file and run `cadaver --json -r FILE URL`.
3. **Branch on the exit status**: 0 means every command succeeded, and
   anything else is the number that did not.
4. Read `--json` for the detail, not the transcript.
5. When something looks wrong at the protocol level, `--trace`.

If all you need is one `PUT` or one `GET` with no authentication
subtleties, `curl -T` and `curl -o` are simpler. cadaver earns its
place when you need `PROPFIND`, `PROPPATCH`, locking, `COPY`/`MOVE`, a
whole directory tree, or a sequence of those against one connection.

## Getting cadaver

Check whether it is already here before downloading anything:

```bash
command -v cadaver.exe || ls ./cadaver.exe 2>/dev/null || echo "not installed"
```

If it is missing, take the latest release. Asset names carry no version
string, so one fixed URL always resolves to the newest one. No API call,
no `jq`, no parsing:

```bash
curl -fsSL -o cadaver.exe https://github.com/Azathothas/cadaver-windows/releases/latest/download/cadaver.exe
```

```powershell
Invoke-WebRequest https://github.com/Azathothas/cadaver-windows/releases/latest/download/cadaver.exe -OutFile cadaver.exe
```

It is a statically linked x86_64 Windows executable. It needs no MSYS2
and no OpenSSL, expat or readline DLLs. There is no installer and
nothing to configure:

```bash
./cadaver.exe --version
```

which prints the fork version and the bundled neon build:

```
cadaver 0.28-win3
neon 0.37.1: Bundled build, Expat 2.8.1, LFS, OpenSSL 3.6.3 9 Jun 2026 (thread-safe).
readline 8.3
```

Every request carries `User-Agent: cadaver/0.28-win3 neon/0.37.1`, so a
server log identifies the tool, the fork release and the HTTP library
behind it.

## Running a session

cadaver is interactive by design: it connects, prints a prompt, and
reads commands until end of input or `quit`. Two ways to drive it
without a terminal, both equivalent:

```bash
printf 'ls\nquit\n' | ./cadaver.exe http://127.0.0.1:8080/dav/
```

```bash
./cadaver.exe -r commands.txt http://127.0.0.1:8080/dav/ < /dev/null
```

The two produce different transcripts, and which you want depends on
what is reading it.

Piped in, readline echoes the prompt and the command, so the transcript
reads as a session and every answer is attributable to the command
above it:

```
dav:/dav/> ls
Listing collection `/dav/': succeeded.
        report.pdf                          88213  Aug 19 10:01
dav:/dav/> quit
Connection to `127.0.0.1' closed.
```

With `-r`, neither the prompt nor the command appears, so the output is
answers only:

```
Listing collection `/dav/': succeeded.
        report.pdf                          88213  Aug 19 10:01
Connection to `127.0.0.1' closed.
```

Prefer `-r` for anything longer than a line or two: the file is easier
to build, easier to log, and `#` comments are allowed in it. Redirect
standard input from nothing as well, so that a session which runs off
the end of the file does not sit waiting. Pipe the commands in instead
when you want each answer labelled with the command that produced it,
which helps once a session is long enough that counting answers is a way
to get it wrong.

A `-r` file that cannot be read is a usage error: cadaver says so and
exits 2 without connecting to anything.

Three things matter about the URL:

* It must be absolute, with an `http:` or `https:` scheme. `host/path`
  is not a URL and is refused.
* It must be a collection that already exists. cadaver does not create
  it.
* Credentials must not be in it. `http://user:pass@host/` is refused;
  use `.netrc` or the prompt.

A trailing slash is added if you leave it off, at the cost of a
redirect round trip on some servers.

## Reading the result

### The exit status

**0 means every command succeeded.** Anything else is the number that
did not, capped at 125 so that it can never be read as a shell's
signalled exit:

```bash
./cadaver.exe -r commands.txt https://dav.example.com/path/ < /dev/null
case $? in
    0) echo "all of it worked" ;;
    *) echo "$? commands failed" ;;
esac
```

A command fails when the server refused it, when the connection could
not be made, when it was given arguments it cannot use, or when it
refused to act: `rmcol` on a plain resource, `delete` on a collection.
The connection cadaver makes from the command line counts as a command
of its own, so a session that could not connect exits non-zero even
though it ran nothing.

Two statuses are not a count:

* **2 from a command line that could not be understood** — an unknown
  option, more than one URL, a `--clobber` value that is not one of the
  three, a `-r` file that cannot be read. Nothing was executed, which is
  what tells it apart from two failed commands.
* **0 from `--help` and `--version`**, which answer a question about
  the program rather than run a session. They write plain text to
  standard output even under `--json`.

A session ended by a signal exits 128 plus the signal number and writes
no `--json` document. Writing one from a signal handler is not safe, and
a session cut short has no result to report. Treat a status of 128 or
more as "no document", not as "the document is missing something".

### `--json`

`--json` writes one JSON object to standard output and nothing else.
Everything a person would have read goes into that object, so nothing
is lost; a prompt, and a trace with no file named, go to standard
error.

```bash
./cadaver.exe --json -r commands.txt https://dav.example.com/path/ \
    < /dev/null > result.json
```

```json
{
  "tool": "cadaver",
  "version": "0.28-win3",
  "target": "http://127.0.0.1:8931/",
  "started": "2026-08-19T07:35:16.974Z",
  "duration": 0.004,
  "commands": [
    {"command": "open", "args": ["http://127.0.0.1:8931/"],
     "target": null, "status": "ok", "duration": 0.002, "output": []},
    {"command": "put", "args": ["f.txt"], "target": "/f.txt",
     "status": "ok", "duration": 0.001,
     "output": ["Uploading f.txt to `/f.txt': [28 bytes] succeeded."]},
    {"command": "delete", "args": ["nosuch"], "target": "/nosuch",
     "status": "failed", "duration": 0.000,
     "context": "404 Not Found",
     "error": {"op": "DELETE", "path": "/nosuch", "status": 404},
     "output": ["Deleting `/nosuch': failed:", "404 Not Found"]}
  ],
  "output": ["Connection to `127.0.0.1' closed."],
  "summary": {"total": 5, "ok": 4, "failed": 1}
}
```

The run:

| Field | Notes |
| --- | --- |
| `tool` | Always `"cadaver"`. |
| `version` | The fork version, the same string `--version` prints. |
| `target` | The URL given on the command line, or `null` when cadaver was started without one. |
| `started` | When the run began, ISO 8601 in UTC with millisecond precision, always suffixed `Z`. Truncated rather than rounded, so it never names a moment that had not happened yet. `null` if the clock could not be read. |
| `duration` | Seconds, to millisecond resolution. Wall-clock, so it includes server and network time. |
| `commands` | Every command line that was executed, in order, including the `open` for the URL on the command line. A blank line or a comment produces nothing. |
| `output` | Present only when something was printed between commands: the connection banner, the closing message. |
| `summary` | `total`, `ok` and `failed`. `ok + failed` equals `total`. |

Each command:

| Field | Notes |
| --- | --- |
| `command` | The command as typed, so an alias appears as the alias: `rm`, not `delete`. |
| `args` | Its arguments, after wildcard expansion. Always present, possibly empty. |
| `status` | `"ok"` or `"failed"`, and nothing else. The set is closed. |
| `duration` | Seconds, to millisecond resolution, wall-clock. |
| `target` | The request target of the last request the command made, exactly as it went on the wire. `null` when it made none: `lpwd`, `set`, or a command that refused before asking. |
| `context` | Why it failed. Absent when it did not. Prose, and not a stable interface: branch on `error`. |
| `error` | Machine-readable classification of the failure. See below. |
| `operations` | Present only on a command that performed more than one: `mput`, `mget`, `copy` with several sources, `rput`, `rget`. See below. |
| `output` | What the command printed, one array element per line, with the line endings removed. Always present. |

and, for the commands that produce data rather than only an outcome:

| Field | On | Notes |
| --- | --- | --- |
| `listing` | `ls` | One object per member: `name`, `href`, `type` (`collection`, `resource`, `reference` or `error`), and then either `size`, `modified` and `executable`, or `status` and `reason` for a member the server could not report on. `modified` is ISO 8601 in UTC, or `null` where the server gave none. |
| `properties` | `propget`, `propnames` | `namespace`, `name`, and `value`: `null` for `propnames`, which asks for the names alone. |
| `headers` | `head` | The response headers, as an object. |
| `http_status` | `head` | The response status, as an integer. |
| `locks` | `lock`, `discover`, `steal`, `showlocks` | `token`, `href`, `scope`, `depth` (`0`, `1` or `"infinity"`), `timeout` in seconds, `"infinity"`, or `null` where the server named none, and `owner`. |
| `path` | `pwd`, `lpwd` | The path the command was asked to report. |
| `options` | `set` with no argument | Every option and its value, as an object. |
| `benchmark` | `bench` | The measurement. See below. |

`search` and `history` have no structured field: what they found is in
`output` as text. That is a gap in this contract rather than a decision;
[TODO](TODO) records it.

#### `output` is split on newlines

`output` is the lines the command printed, so a value that contains a
newline becomes several elements. A property value is the usual way to
meet this: `propget` prints it as it came back, and a multi-line one
arrives as one element per line. The structured field carries it whole,
so read `properties[].value` rather than reassembling `output`.

### Classifying a failure

`context` is prose. To branch on the kind of failure without matching
strings, read `error`:

```json
"error": {"op": "DELETE", "path": "/nosuch", "status": 404}
```

| Field | Notes |
| --- | --- |
| `error` | Present only on a command whose `status` is `"failed"`, and only if it got as far as sending a request. A command that refused before asking, such as `rmcol` on a plain resource or an argument that was wrong, has none. |
| `error.op` | The HTTP method, as sent. `OPTIONS`, `GET`, `HEAD`, `PUT`, `DELETE`, `MKCOL`, `COPY`, `MOVE`, `PROPFIND`, `PROPPATCH`, `LOCK`, `UNLOCK`, and the DeltaV and DASL methods for the commands that use them. |
| `error.path` | The request target exactly as it went on the wire: an absolute path, escaped. |
| `error.status` | The response status as an integer, or `null` when no response arrived at all: a refused connection, a timeout, a TLS failure. `null` and a missing `error` mean different things. `null` means cadaver asked and got nothing back. |

Two consequences. A command resolves a path with a `PROPFIND` before
doing anything with it, so `error.op` is the method of the *last*
request, which is the one the failure is about. And a redirect or an
authentication challenge is retried by neon on the same request, so
`status` is the final response rather than the intermediate one.

```python
if c["status"] == "failed":
    err = c.get("error")
    if err is None:
        kind = "no request made"        # read c["context"]
    elif err["status"] is None:
        kind = "transport"              # never reached the server
    else:
        kind = "%s -> %d" % (err["op"], err["status"])
```

### Commands that do several things

`mput a b c` performs one operation per file. The command object then
carries an `operations` array, one entry per operation, each with its
own `target`, `status`, `duration`, `context` and `error`:

```json
{"command": "mput", "args": ["a.txt", "b.txt"], "target": "/dav/b.txt",
 "status": "failed", "duration": 0.126,
 "context": "403 Forbidden",
 "error": {"op": "PUT", "path": "/dav/b.txt", "status": 403},
 "operations": [
   {"target": "/dav/a.txt", "status": "ok", "duration": 0.062},
   {"target": "/dav/b.txt", "status": "failed", "duration": 0.063,
    "context": "403 Forbidden",
    "error": {"op": "PUT", "path": "/dav/b.txt", "status": 403}}]}
```

`operations` is absent when the command performed at most one, so a
consumer that ignores it still branches correctly: the command's own
`target`, `context` and `error` describe **the first operation that
failed**, or the last one if none did.

`rput` and `rget` produce one operation per directory and per file, so
a large tree makes a large array.

### What `bench` measured

```json
"benchmark": {
  "target": "/dav/",
  "started": "2026-08-19T16:12:03.114Z",
  "iterations": 3,
  "payload_bytes": 1048576,
  "latency": {"op": "PROPFIND", "samples": 3,
              "min_ms": 1.204, "median_ms": 1.431, "max_ms": 3.102},
  "upload": {"bytes": 3145728, "seconds": 0.412, "mib_per_second": 7.63},
  "download": {"bytes": 3145728, "seconds": 0.298, "mib_per_second": 10.55}
}
```

Byte counts are exact. Durations are wall-clock seconds to millisecond
resolution, so they include server and network time; the latency
figures are in milliseconds, because a round trip is routinely under
one. Rates are MiB/s, powers of 1024.

`mib_per_second` is `null` where `seconds` reads as 0.000: a transfer
that took less than the resolution beside it gives a rate the two
numbers next to it do not support. Ask for a larger payload.

### Three commands `--json` refuses

`cat` writes the resource to standard output, `less` runs a pager that
writes there too, and `edit` runs a program that might. Under `--json`
standard output carries the result document, so all three are refused
and recorded as failed commands saying so. Use `get` and `put`.

### A minimal consumer

```python
import json, subprocess

p = subprocess.run(["./cadaver.exe", "--json", "-r", "commands.txt", url],
                   stdin=subprocess.DEVNULL, capture_output=True, text=True)
result = json.loads(p.stdout)

for c in result["commands"]:
    if c["status"] == "failed":
        print(c["command"], c["args"], "->", c.get("context"))

# The exit status says the same thing without parsing anything, up to
# the cap.
assert p.returncode == min(result["summary"]["failed"], 125)
```

### Without `--json`

The transcript is regular enough to read, and is what a person sees:

| Ending | Meaning |
| --- | --- |
| `succeeded.` | The command worked. |
| `failed:` followed by a line | It did not. The next line is the reason, usually an HTTP status and phrase. |
| `collection is empty.` | `ls` on a collection with no members. |
| `it is already there.` | `rput` on a collection that exists. |
| `N versions in history:` | `history` on a version-controlled resource. |
| `the server reported no locks.` | `discover` or `steal` on a resource the server said holds none. |
| `authentication failed.` | Credentials were wrong or absent. |
| `could not connect to server.` | Nothing answered. |
| `connection timed out.` | Something answered and then stopped. |
| `redirect to URL` | The server redirected and cadaver did not follow it. |

Match on the ending, not on the whole line: the part before it names the
command and the resource and is not a fixed string. Anything else
cadaver prints is the answer to a command that produces output, such as
`ls`, `cat`, `propget`, `head` or `showlocks`, and its format is per
command. Prefer `--json`, where all of it is a field.

A transfer that finished says how many bytes it moved, in square
brackets before the outcome, when the output is not a terminal:
`Uploading f.txt to `/f.txt': [28 bytes] succeeded.` A transfer that did
not finish says nothing there, because what arrived was the error body.

## Seeing what actually happened

`--trace` is the flag that matters when a command fails and you do not
know why. It dumps every request and response, tagged with the command
that issued it:

```bash
./cadaver.exe --json --trace=wire.log -r commands.txt URL > result.json
```

Sent lines are prefixed `>`, received lines `<`. Bodies are printed in
square brackets rather than prefixed, because a `PROPFIND` or `LOCK`
body is XML and nearly every line of one starts with a `<`:

```
--- 5 (put hello.txt) ---
> PUT /dav/hello.txt HTTP/1.1
> User-Agent: cadaver/0.28-win3 neon/0.37.1
> Connection: TE
> TE: trailers
> Host: 127.0.0.1:8907
> Content-Length: 28
< HTTP/1.1 201 Created
< content-length: 0
<
```

The number in the header is the command's position in the session,
counting the `open` for the URL on the command line as the first, and
the text after it is the command. A wildcard makes its requests before
the line has been parsed, so those carry the pattern as typed and the
requests after them carry the names it matched: one command can appear
under two labels. To find the exchange behind a failure, grep for it:

```bash
grep -A 30 "(put hello.txt)" wire.log
```

With no filename the trace goes to standard error. Use `-` for standard
output, which `--json` then refuses to share; that combination is a
usage error rather than a corrupted document.

**The body of a transfer is not traced.** A `put`, `get`, `mput`,
`mget`, `rput`, `rget`, `cat` or `edit` moves the resource itself, which
is not a protocol document and may be very large; its headers and its
outcome are traced and its bytes are not. Every other body is.

`--verbose` widens the trace to everything neon reports, including
socket, TLS, XML parser and authentication detail. Reach for it when
the problem looks like a connection, a handshake or an authentication
exchange rather than a protocol one. It deliberately leaves out the
credentials in the clear; `set debug cleartext` is how you ask for
those, and the output should not be pasted anywhere.

## Commands to know about before scripting them

`help` prints the list and `help COMMAND` describes one, both without a
connection. The whole set is in [README.md](README.md).

### `get` asks before overwriting, unless told what to do

By default a `get` whose local file already exists prompts for another
name. In a script that reads end of input and the command fails, having
downloaded nothing. That is a clear answer, and rarely the one you
want. Say which you want instead:

```bash
./cadaver.exe --clobber=yes -r commands.txt URL   # overwrite it
./cadaver.exe --clobber=no  -r commands.txt URL   # fail, keep the file
```

or `set clobber yes` inside a session. The three values are `ask`,
which is the default, `yes` and `no`.

A `get` that fails leaves the local file exactly as it found it. The
download goes to a temporary name beside the destination and is renamed
over it once the whole body has arrived, so neither a truncated file nor
an empty one is left behind. That used to happen, and then made every
retry prompt for a name instead of downloading.

### `rput` and `rget` move a whole tree

`rput local [remote]` walks the local directory and creates a collection
per directory, then uploads each file. A collection that is already
there is reported as such and is not a failure. One that could not be
created skips its subtree, because nothing under a collection that is
not there can succeed; a sibling is still tried.

`rget remote [local]` does the reverse, one `PROPFIND` per collection,
creating the local directories as it goes. `clobber` applies to every
file it writes, so `--clobber=yes` is usually what a script wants.

Either given a plain resource rather than a directory or a collection
does what `put` or `get` would have done. Both stop at a Ctrl-C, and
both refuse to go more than 64 levels deep, so a junction or a symbolic
link that makes a directory contain itself is not walked forever.

`rget` is the only command that writes a local file under a name the
server chose. A member the server names `..`, or names with a path
separator in it, or with a colon on Windows, is refused and counted as
a failure: joining such a name to the destination would write outside
the directory you asked for.

### `resumeget` needs the local file to exist

It sends a `Range` request from the current local size and appends. If
the file is not there it reports so and does nothing.

A server that ignores `Range` and answers 200 with the whole resource
is reported as an error (`Resource does not support ranged GET
requests`) and the local file is left exactly as it was. cadaver records
its size before the request and puts it back afterwards if the request
failed, so that holds whatever the response was.

### `mput` on a directory

`mput *` matches directories as well as files. Each one is reported as
a failure naming what it is. Use `rput` for a tree.

### `delete` and `rmcol` are not interchangeable

`delete` refuses a collection and `rmcol` refuses a plain resource, each
with a message telling you to use the other. `rmcol` removes the
collection and everything in it, with no confirmation.

### `copy` and `move` follow the shell's shape

`copy src... dest`: with more than two arguments, or with a destination
that is a collection, each source is copied *into* the destination. A
collection source into a collection destination lands as a subcollection
of it.

A collection source with a destination that **exists and is not a
collection** is refused. A destination that does not exist yet is a
rename, and works: `move reports reports-2026` renames the collection.

With `overwrite` off the request carries `Overwrite: F`, and a
destination that is already there answers 412. cadaver names the
resource that was in the way and says that `set overwrite on` replaces
it. `rename` is a `MOVE` with `Overwrite: F` whatever the option says,
so it reports that `move` is the command that can overwrite.

### `edit` runs a program

It locks the resource, downloads it to a temporary file, runs the
editor, uploads the result with `If-Match` and unlocks. The editor must
not return before the user has finished, so set it to something that
blocks:

```
set editor "code --wait"
```

A `PUT` refused with 412 means the resource changed while it was being
edited. If the editor makes no change, cadaver says so and uploads
nothing.

### `bench` writes to the server

It puts a generated payload into the current collection under the name
`cadaver-bench.dat`, reads it back and deletes it. A resource already
under that name is left alone and the command refuses, because deleting
it afterwards is part of what `bench` does. Point it at a collection you
are willing to have written to.

### Wildcards are expanded by cadaver

`*`, `?`, `[abc]`, `[a-z]`, `[!abc]` and POSIX classes work for the
commands that take file names, locally for `mput` and against the server
for the rest. A pattern that matches nothing is passed through
unexpanded, as a shell does, so `mput nosuch*.txt` ends up trying to
upload a file called `nosuch*.txt` and failing on it. Check for
`no matches.]` in the transcript if that matters. A name beginning with
a dot is not matched, as in a shell.

Remote expansion costs one `PROPFIND` per collection, so
`delete */*/*.tmp` against a wide tree is not free. What that listing
says about each member is remembered for the rest of the command, so
the deletes that follow do not ask about them again.

### Ctrl-C

Ctrl-C during a transfer abandons it and returns to the prompt. The
command is recorded as failed with the reason `Interrupted.`, a download
leaves no local file, and a partly uploaded resource is the server's to
clean up. A second Ctrl-C during the same transfer ends the session,
which is the way out of one that has stalled so completely that no block
arrives to check the first. A session ended that way writes no `--json`
document; see the exit status above.

## Authentication

Three ways, in the order cadaver tries them:

1. **`.netrc`** in the home directory, at `%USERPROFILE%\.netrc` on
   Windows unless `$HOME` is set. Standard `ftp(1)` format:

   ```
   machine dav.example.com
   login alice
   password s3cret
   ```

   An entry with a `login` and no `password` supplies the login, and
   only the password is asked for. A `default` entry with no `machine`
   matches any host, and is used only when no named entry does.

   A quote only quotes when it opens a value, so that is how a value
   containing a space is written; inside one it is an ordinary
   character, so a generated password with an apostrophe in it survives.
   There is no escape, so a value that has to *begin* with a quote
   character cannot be spelled.

   The file's permissions are not checked, so restricting them is your
   responsibility.

   A `machine` entry is matched without regard to case, and wins over a
   `default` entry wherever the two appear in the file.

2. **The prompt**, which reads the username with readline and the
   password with echo off. In a script both come from standard input,
   one line each, so credentials can be piped in. They will be in the
   process's input, which is usually worse than a `.netrc` with
   restrictive permissions.

3. **A client certificate**, for servers that want mutual TLS:

   ```
   set client-cert C:\certs\client.p12
   ```

   If it is encrypted cadaver asks for the passphrase, three attempts.

An untrusted server certificate is refused outright when standard input
is not a terminal: cadaver prints the certificate details and
`Certificate rejected.` instead of asking. There is no flag to accept
one anyway. Install the CA, or use a certificate the machine trusts.

## Options

`set` with no argument lists every option and its value; `describe NAME`
explains one. A boolean takes `on` or `off`, or no value at all to turn
it on, and `unset NAME` turns it off. `set` on an option that does not
exist is a failed command, and so is a value the option does not know.

## `set debug`, the older way

`--trace` and `--verbose` cover what `set debug` does and tag it with
the command that caused it, so reach for those first. `set debug` is
still there for turning one keyword on part way through a session, and
it is the only way to ask for the credentials in the clear. It writes
to wherever `--trace` was pointed, or to standard error:

```bash
printf 'set debug http\nls\nquit\n' | ./cadaver.exe URL > out.txt 2> wire.txt
```

The value is a comma-separated list:

| Keyword | Shows |
| --- | --- |
| `http` | Request and response headers, and the request line |
| `httpbody` | Message bodies as well |
| `xml`, `xmlparse` | The XML cadaver sends and how neon parses it |
| `socket` | Connection, read and write activity |
| `ssl` | TLS handshake detail |
| `httpauth` | Authentication negotiation |
| `locks` | neon's lock store |
| `files` | cadaver's own path resolution |
| `cleartext` | Credentials in the clear; never paste that output anywhere |

`--verbose` is every one of those except `cleartext`.

## Telling a server problem from a cadaver problem

Default to reading the status code. cadaver reports what the server
said, so the message names both the operation and the answer.

It is the server when:

* The failure line names a method and an HTTP status. `403` on
  `PROPPATCH` means no dead property store; `423` means locked; `409`
  means the parent collection does not exist; `405` on `MKCOL` means
  something is already there.
* `Location does not advertise WebDAV class 1 support`, meaning the
  server did not put `DAV: 1` in its `OPTIONS` response. Some servers
  that work fine otherwise get this wrong; `-t` proceeds anyway.
* Every `LOCK` fails with `Response missing activelock`, meaning the
  server answered `LOCK` with a `Content-Type` that is not an XML media
  type, so neon discarded the body. wsgidav does exactly this.

It is the environment when:

* Everything fails at the first request. Check the URL, the port and
  the credentials.
* `Could not parse URL`, meaning the argument has no scheme.
* A download produced nothing and the transcript says `cancelled.`,
  meaning the local file already existed.
* A local path did not resolve. On Windows, `lcd C:\Users\me` works, but
  a path from an MSYS or Git Bash shell such as `/c/Users/me` does not:
  cadaver is a native Windows program and does not know about the
  translation. Use `cygpath -w` to convert.

## Quoting a command line

A command line is split on whitespace. `#` starts a comment, and single
or double quotes group an argument that contains spaces:

```
lcd "C:\Program Files\data"
get "release #3.txt"
propset report.pdf comment ""
```

A quote quotes only where it opens an argument. Inside one it is an
ordinary character, so `set lockowner foo"bar` sets exactly that, and a
password or a property value containing a quote can be written. An empty
argument is written `""` and reaches the command as an empty string.

On Windows a backslash is an ordinary character too, because it is the
path separator: `lcd C:\Users\me` and `lls C:\dir\*` both mean what they
look like. Everywhere else a backslash quotes the character after it, as
a shell does, so `get my\ file` works there instead.

## A worked example

Upload a directory of files, verify what arrived, and fetch one back:

```bash
cat > session.txt <<'EOF'
# Everything local happens relative to this directory.
lcd C:\work\outbox
mkcol 2026-08-19
cd 2026-08-19
mput *.pdf
ls
propset manifest.pdf batch 2026-08-19
propget manifest.pdf batch
quit
EOF

./cadaver.exe --json --trace=wire.log -r session.txt \
    https://dav.example.com/uploads/ < /dev/null > result.json
```

The exit status is the number of commands that failed, so the whole
check is one branch:

```bash
if [ $? -ne 0 ]; then
    python -c "
import json
for c in json.load(open('result.json'))['commands']:
    if c['status'] == 'failed':
        print(c['command'], c['args'], '->', c.get('context'))
"
    exit 1
fi
```

and if one of them needs looking at, the exchange behind it is in the
trace under the command that caused it:

```bash
grep -A 30 "(propset manifest.pdf batch 2026-08-19)" wire.log
```

The whole tree in one command, when the layout is what you want to keep:

```bash
printf 'rput C:\\work\\outbox uploads\nquit\n' > session.txt
./cadaver.exe --json --clobber=yes -r session.txt \
    https://dav.example.com/ < /dev/null > result.json
```

To fetch one file, `get` it into a named file. `cat` writes the
resource to standard output, which is fine at a terminal and not under
`--json`, where standard output carries the result document; cadaver
refuses it there instead of corrupting the document.

## What this fork changed

Everything above describes this fork. Against upstream cadaver the
differences that would bite a script are:

* The exit status says how many commands failed. Upstream exits 0
  however the session went, so a script has to parse the transcript.
* `--json` exists at all, and so do `--trace` and `--verbose`.
* `rput`, `rget` and `bench` exist.
* Ctrl-C during a transfer returns to the prompt instead of ending the
  process part way through an upload.
* A failed `get` leaves the local file as it found it, and `--clobber`
  decides what it does when one is there.
* `resumeget` truncates back to where it started when the request fails,
  instead of leaving a second copy appended.
* Renaming a collection with `move` works.
* A lock owner containing XML markup characters is escaped, so `set
  lockowner` accepts one.
* A `.netrc` value keeps its quote characters, and an entry with only a
  login is used for the login.
* The DeltaV commands no longer put a trailing slash on the path of a
  plain resource.

and, on Windows specifically, that binary transfers are no longer
corrupted by line-ending translation, that a path outside the active
ANSI code page works, that `lcd` and `put` accept Windows paths, that
`lls` works at all, and that wildcards are matched by a new
implementation. [NEWS](NEWS) has the full list and [TODO](TODO) has what
is still missing.
