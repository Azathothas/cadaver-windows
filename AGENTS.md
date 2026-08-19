# Driving cadaver from a program

This is written for an agent that needs to move files over WebDAV and
wants to use cadaver to do it. It assumes you can run commands and read
text output. For build instructions see [README.md](README.md); this
file is about using the tool.

The short version:

1. Get a binary (below), or build one.
2. Put the commands in a file and run `cadaver -r FILE URL`, or pipe
   them in.
3. **Read the output.** cadaver's exit status is 0 whether the commands
   worked or not, so the transcript is the only thing that tells you.
4. When something looks wrong at the protocol level, `set debug http`.

If all you need is one `PUT` or one `GET` with no authentication
subtleties, `curl -T` and `curl -o` are simpler. cadaver earns its
place when you need `PROPFIND`, `PROPPATCH`, locking, `COPY`/`MOVE`, or
a sequence of those against one connection.

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
cadaver 0.28-win1
neon 0.37.1: Bundled build, Expat 2.8.1, LFS, OpenSSL 3.6.3 9 Jun 2026 (thread-safe).
readline 8.3
```

Every request carries `User-Agent: cadaver/0.28-win1 neon/0.37.1`, so a
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
when you want each answer labelled with the command that produced it —
which is worth having when a session is long enough that counting
answers is a way to get it wrong.

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

**A session always exits 0.** Not "0 unless something failed" — always,
including when every command in it failed, and including when the
connection was refused. Do not branch on it. This is inherited from
upstream and is the single most important thing to know before
scripting cadaver; it is [recorded in TODO](TODO) as the first thing to
change.

`--help`, `--version` and a usage error are the other way round: they
exit -1, which Windows reports as 4294967295 and a POSIX shell as 127
or 255. So `cadaver --version` looks like a failure to a script with
`set -e` in it even though it printed what you asked for.

What you have instead is the transcript, which is regular:

| Ending | Meaning |
| --- | --- |
| `succeeded.` | The command worked. |
| `failed:` followed by a line | It did not. The next line is the reason, usually an HTTP status and phrase. |
| `collection is empty.` | `ls` on a collection with no members. |
| `authentication failed.` | Credentials were wrong or absent. |
| `could not connect to server.` | Nothing answered. |
| `connection timed out.` | Something answered and then stopped. |
| `redirect to URL` | The server redirected and cadaver did not follow it. |

So a session is clean if no line ends `failed:` and none of the other
failure endings appear. In Python:

```python
import subprocess

FAILED = ("failed:", "authentication failed.", "could not connect to server.",
          "connection timed out.")

def run(script, url):
    p = subprocess.run(["./cadaver.exe", "-r", script, url],
                       stdin=subprocess.DEVNULL,
                       capture_output=True, text=True)
    lines = p.stdout.splitlines()
    bad = [n for n, line in enumerate(lines)
           if line.rstrip().endswith(FAILED)]
    return lines, bad

lines, bad = run("commands.txt", "http://127.0.0.1:8080/dav/")
for n in bad:
    # The failure line, and the reason on the line after it.
    print("\n".join(lines[n:n + 2]))
```

Match on the ending rather than on the whole line: the part before it
names the command and the resource and is not a fixed string.

Anything cadaver prints that is not one of those endings is the answer
to a command that produces output — `ls`, `cat`, `propget`, `head`,
`showlocks` — and its format is per command. `cat` writes the resource
itself, unmodified and in binary, so a session that ends in `cat` is a
usable way to fetch a file to standard output.

## Commands worth knowing about

`help` prints the list and `help COMMAND` describes one, both without a
connection. The whole set is in [README.md](README.md). These are the
ones whose behaviour is worth knowing before you script them.

### `get` prompts when the local file exists

```
dav:/dav/> get report.pdf
Enter local filename for `report.pdf': 
```

With standard input at end of file that reads nothing and prints
`cancelled.`, so a script silently downloads nothing. Either name a
destination that does not exist, or delete it first. `get remote local`
takes the same path — the prompt is about the *local* file existing, not
about the remote name.

Worse, a `get` that fails leaves a zero-length local file behind,
because the file is created before the request is made. The next `get`
then finds it and prompts. If a download failed, remove the local file
before retrying.

### `resumeget` needs the local file to exist

It sends a `Range` request from the current local size and appends. If
the file is not there it reports so and does nothing. If the server
ignores `Range` and answers 200 with the whole body, cadaver reports an
error, but the body has already been appended: **check the size after a
failed `resumeget` rather than trusting the file.**

### `delete` and `rmcol` are not interchangeable

`delete` refuses a collection and `rmcol` refuses a plain resource, each
with a message telling you to use the other. `rmcol` removes the
collection and everything in it, with no confirmation.

### `copy` and `move` follow the shell's shape

`copy src... dest`: with more than two arguments, or with a destination
that is a collection, each source is copied *into* the destination. A
collection source into a collection destination lands as a subcollection
of it. A collection source with a plain destination is refused.
`rename src dest` is a `MOVE` with `Overwrite: F` and ignores the
`overwrite` option.

### `edit` runs a program

It locks the resource, downloads it to a temporary file, runs the
editor, uploads the result with `If-Match` and unlocks. The editor must
not return before the user has finished — set it to something that
blocks:

```
set editor "code --wait"
```

A `PUT` refused with 412 means the resource changed while it was being
edited. If the editor makes no change, cadaver says so and uploads
nothing.

### Wildcards are expanded by cadaver

`*`, `?`, `[abc]`, `[a-z]`, `[!abc]` and POSIX classes work for the
commands that take file names, locally for `mput` and against the server
for the rest. A pattern that matches nothing is passed through
unexpanded, as a shell does, so `mput nosuch*.txt` ends up trying to
upload a file called `nosuch*.txt` and failing on it. Check for
`no matches.]` in the transcript if that matters.

Remote expansion costs one `PROPFIND` per collection, so
`delete */*/*.tmp` against a wide tree is not free.

## Authentication

Three ways, in the order cadaver tries them:

1. **`.netrc`** in the home directory — `%USERPROFILE%\.netrc` on
   Windows unless `$HOME` is set. Standard `ftp(1)` format:

   ```
   machine dav.example.com
   login alice
   password s3cret
   ```

   Both lines are needed: an entry with a `login` and no `password` is
   ignored entirely and you get the prompt for both. A `default` entry
   with no `machine` matches any host, and is looked at only after the
   named ones.

   Two things about the parser to know before you generate a `.netrc`
   from somewhere else. A quote character anywhere in a value is
   removed, so a password containing `"` or `'` is silently mangled and
   authentication fails with no indication why. And the file's
   permissions are not checked, so restricting them is your
   responsibility. Both are [in TODO](TODO).

2. **The prompt**, which reads the username with readline and the
   password with echo off. In a script both come from standard input,
   one line each, so credentials can be piped in — but they will be in
   the process's input, which is usually worse than a `.netrc` with
   restrictive permissions.

3. **A client certificate**, for servers that want mutual TLS:

   ```
   set client-cert C:\certs\client.p12
   ```

   If it is encrypted cadaver asks for the passphrase, three attempts.

An untrusted server certificate is refused outright when standard input
is not a terminal — cadaver prints the certificate details and
`Certificate rejected.` rather than asking. There is no flag to accept
one anyway. Install the CA, or use a certificate the machine trusts.

## Seeing what is on the wire

`set debug` turns on neon's tracing, which goes to standard error, so it
stays out of the transcript on standard output:

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
| `cleartext` | Credentials in the clear — never paste that output anywhere |

`set debug http,xml` is the usual starting point for a request that is
being answered in a way you did not expect.

## Telling a server problem from a cadaver problem

Default to reading the status code. cadaver reports what the server
said, so the message names both the operation and the answer.

It is the server when:

* The failure line names a method and an HTTP status. `403` on
  `PROPPATCH` means no dead property store; `423` means locked; `409`
  means the parent collection does not exist; `405` on `MKCOL` means
  something is already there.
* `Location does not advertise WebDAV class 1 support` — the server did
  not put `DAV: 1` in its `OPTIONS` response. Some servers that work
  fine otherwise get this wrong; `-t` proceeds anyway.
* Every `LOCK` fails with `Response missing activelock` — the server
  answered `LOCK` with a `Content-Type` that is not an XML media type,
  so neon discarded the body. wsgidav does exactly this.

It is the environment when:

* Everything fails at the first request. Check the URL, the port and
  the credentials.
* `Could not parse URL` — the argument has no scheme.
* A download produced nothing and the transcript says `cancelled.` —
  the local file already existed.
* A local path did not resolve. On Windows, `lcd C:\Users\me` works, but
  a path from an MSYS or Git Bash shell such as `/c/Users/me` does not:
  cadaver is a native Windows program and does not know about the
  translation. Use `cygpath -w` to convert.

## Quoting a command line

A command line is split on whitespace. `#` starts a comment, and single
or double quotes group an argument that contains spaces.

On Windows a backslash is an ordinary character, because it is the path
separator: `lcd C:\Users\me` and `lls C:\dir\*` both mean what they
look like. A name containing a space or a `#` is written in quotes:

```
lcd "C:\Program Files\data"
get "release #3.txt"
```

Everywhere else a backslash quotes the character after it, as a shell
does, so `get my\ file` works there instead. A Windows file name cannot
contain a quote character, so nothing is left without a spelling.

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

./cadaver.exe -r session.txt https://dav.example.com/uploads/ \
    < /dev/null > transcript.txt 2>&1

if grep -q 'failed:' transcript.txt; then
    grep -A1 'failed:' transcript.txt
    exit 1
fi
```

and to fetch one file to standard output, with no local file involved:

```bash
printf 'cat report.pdf\nquit\n' \
    | ./cadaver.exe https://dav.example.com/uploads/2026-08-19/ \
    > report.pdf
```

That last one needs care: `cat` writes the resource, but the prompt
lines and the closing message go to standard output too. Use it only
when you can strip those, or when the consumer tolerates them. For a
plain download `get` into a named file is the right command.

## What this fork changed

Everything above describes this fork. Against upstream cadaver on
Windows the differences that would bite a script are that binary
transfers are no longer corrupted by line-ending translation, that
`lcd` and `put` accept Windows paths, that `lls` works at all, and that
wildcards are matched by a new implementation. [NEWS](NEWS) has the
full list, and [TODO](TODO) has what is still missing — the exit status
first among them.
