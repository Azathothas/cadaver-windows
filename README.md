# cadaver for Windows

cadaver is a command-line WebDAV client: upload, download, on-screen
display, in-place editing, `COPY` and `MOVE`, collection creation and
deletion, property manipulation and resource locking, driven the way
`ftp` and `smbclient` are. This is a port of
[notroj/cadaver](https://github.com/notroj/cadaver) that builds and runs
on Windows as a native MinGW-w64 program: no Cygwin runtime, no WSL, no
MSYS2 needed at run time.

```bash
cadaver https://dav.example.com/path/
```

It is one executable, `cadaver.exe`, with no installer and nothing to
configure.

## Getting a binary

Released builds are statically linked x86_64 Windows executables. They
need no MSYS2, no OpenSSL DLLs and no readline or expat DLLs. Asset
names carry no version, so these URLs always resolve to the newest
release:

```bash
curl -fsSL -o cadaver.exe https://github.com/Azathothas/cadaver-windows/releases/latest/download/cadaver.exe
```

```powershell
Invoke-WebRequest https://github.com/Azathothas/cadaver-windows/releases/latest/download/cadaver.exe -OutFile cadaver.exe
```

The same release also carries `cadaver-windows-x86_64.zip`, which holds
the executable plus `README.md`, `AGENTS.md` and `COPYING`:

```bash
curl -fsSL -o cadaver-windows-x86_64.zip https://github.com/Azathothas/cadaver-windows/releases/latest/download/cadaver-windows-x86_64.zip
```

Check it runs:

```bash
./cadaver.exe --version
```

```
cadaver 0.28-win1
neon 0.37.1: Bundled build, Expat 2.8.1, LFS, OpenSSL 3.6.3 9 Jun 2026 (thread-safe).
readline 8.3
```

Release tags look like `v0.28-win1`. The `0.28` is the upstream cadaver
version this fork tracks; the suffix counts releases of the fork against
it. Every release bundles neon 0.37.1.

## Building

You need MSYS2, and nothing else installed first.

### 1. Install MSYS2

Download the installer from <https://www.msys2.org/> and run it, or with
winget:

```bash
winget install MSYS2.MSYS2
```

The default install location is `C:\msys64`.

### 2. Install the toolchain

Open the **MSYS2 UCRT64** shortcut from the Start menu — not the plain
MSYS2 shell, and not MINGW64. Then:

```bash
pacman -S --needed autoconf automake libtool m4 make pkgconf mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-expat mingw-w64-ucrt-x86_64-readline mingw-w64-ucrt-x86_64-libiconv mingw-w64-ucrt-x86_64-pkgconf
```

If pacman asks you to close the terminal and reopen it, do that and run
the command again.

### 3. Clone

```bash
git clone https://github.com/Azathothas/cadaver-windows
```

There is no `--recurse-submodules` step. neon is checked into the tree.

### 4. Build

Two build paths produce the same executable. Autotools:

```bash
./autogen.sh && ./configure --with-ssl=openssl && make
```

You do not need `--with-included-neon`; there is no system neon on
Windows to prefer, so the bundled copy is used regardless. Add
`--enable-warnings` to build with the warning set CI gates on.

Or without autotools, using the configuration checked in at
`win32/config.h`:

```bash
make -f Makefile.w32
```

This needs only GNU make and a MinGW-w64 compiler whose sysroot has
OpenSSL, expat, readline and iconv, which a UCRT64 shell already has. It
is faster and has fewer moving parts, and it always builds with the
warnings on.

To build an executable that runs on a machine with no MSYS2:

```bash
make -f Makefile.w32 STATIC=1
```

That links the libraries in statically, so the result depends only on
DLLs that ship with Windows. This is how the released binaries are
built. It is correspondingly larger: 10069228 bytes (9.6 MiB) against
1461364 bytes (1.4 MiB) dynamically linked, measured against OpenSSL
3.6.3, expat 2.8.1 and readline 8.3. Expect a different exact figure
from a different toolchain.

If those libraries live outside the toolchain, point `PREFIX` at a
sysroot providing all four:

```bash
make -f Makefile.w32 PREFIX=/some/sysroot
```

`PREFIX` must belong to the same toolchain as the compiler. Aiming a
standalone MinGW-w64 at MSYS2's `lib` directory pulls in MSYS2's C
runtime as well, and the link fails on `_gnu_exception_handler` and
`__mingw_oldexcpt_handler`.

## Running it

```bash
cadaver [OPTIONS] URL
```

```bash
cadaver http://dav.example.com/path/
cadaver -t https://dav.example.com/path/
cadaver -r session.txt https://dav.example.com/path/
```

The URL must be an absolute `http:` or `https:` URI. cadaver connects,
then reads commands until end of input or `quit`.

| Option | What it does |
| --- | --- |
| `-t`, `--tolerant` | Allow `cd` and `open` into a collection that does not advertise WebDAV class 1 |
| `-r`, `--rcfile=FILE` | Read commands from FILE instead of the default rcfile |
| `-p`, `--proxy=HOST[:PORT]` | Use a proxy server |
| `-V`, `--version` | Print the version and the bundled neon build |
| `-h`, `--help` | Print the option list and the default rcfile path |

### Commands

`help` lists them; `help COMMAND` describes one.

| Command | What it does |
| --- | --- |
| `ls [path]` | List the current or a named collection |
| `cd path` | Change collection; `cd -` returns to the previous one |
| `pwd`, `lpwd` | Print the current remote or local directory |
| `lcd [dir]`, `lls [-l] [dir]` | Change or list the local directory |
| `put local [remote]`, `mput local...` | Upload |
| `get remote [local]`, `mget remote...` | Download |
| `resumeget remote [local]` | Resume a download into an existing local file |
| `cat remote...`, `less remote...` | Display a resource, directly or through a pager |
| `head remote` | Show the status and headers of a `HEAD` |
| `edit remote` | Lock, download, run an editor, upload, unlock |
| `mkcol remote...`, `rmcol remote...` | Create and delete collections |
| `delete remote...` | Delete non-collection resources |
| `copy src... dest`, `move src... dest`, `rename src dest` | `COPY` and `MOVE` |
| `lock`, `unlock`, `discover`, `steal`, `showlocks` | Locking |
| `propget`, `propset`, `propdel`, `propnames` | Properties |
| `chexec + remote` | Set the Apache `executable` property |
| `search query` | DASL search |
| `version`, `checkin`, `checkout`, `uncheckout`, `history`, `label` | DeltaV |
| `set`, `unset`, `describe` | Options; `set` on its own lists them |
| `open URL`, `close`, `logout` | Connect, disconnect, forget credentials |
| `about`, `help`, `quit` | |

`rm`, `mkdir`, `mv`, `cp` and `more` are aliases for `delete`, `mkcol`,
`move`, `copy` and `less`; `exit` and `bye` are aliases for `quit`.

### Wildcards

`*`, `?`, `[abc]`, `[a-z]`, `[!abc]` and POSIX classes such as
`[[:digit:]]` are expanded for the commands that take file names, both
locally and against the server:

```bash
mput *.txt
delete backup/*.old
copy 2026-*/report.pdf archive
```

A remote expansion costs one `PROPFIND` per collection it has to look
inside, and Ctrl-C interrupts it. Matching is case-insensitive against
the local file system on Windows and case-sensitive against a server,
which is what each of them does with names.

### Scripting

`-r FILE` runs the commands in FILE. A `quit` in the file ends the
session; without one, cadaver carries on reading standard input, so
piping commands in works too:

```bash
printf 'ls\nquit\n' | cadaver http://dav.example.com/path/
```

`#` starts a comment, and single or double quotes group an argument
containing spaces. On Windows a backslash is an ordinary character,
because it is the path separator: `lcd C:\Users\me` reaches `chdir`
intact, and a name containing a space or a `#` is written in quotes
rather than with backslashes. Elsewhere a backslash quotes the
character after it, as a shell does.

The exit status does not yet report whether the commands succeeded; see
[TODO](TODO).

### The rcfile

Without `-r`, cadaver reads `.cadaverrc` from the home directory if it
is there — `%USERPROFILE%\.cadaverrc` on Windows unless `$HOME` is set.
`cadaver --help` prints the path it will use. It holds the same commands
a session does, which is the place for `set` lines:

```
set editor "code --wait"
set pager less
open https://dav.example.com/path/
```

`.netrc` in the same directory supplies credentials, in the format
`ftp(1)` uses. Both a `login` and a `password` line are needed for a
machine before cadaver will use either.

## Options

`set` with no argument lists them, `describe NAME` explains one, and
`set NAME VALUE` changes one.

| Option | Effect |
| --- | --- |
| `tolerant` | Tolerate collections that do not advertise WebDAV |
| `overwrite` | Whether `copy` and `move` overwrite the destination |
| `expect100` | Use `Expect: 100-continue` on uploads |
| `utf8` | Treat local names as UTF-8 rather than converting |
| `quiet` | Whether to print connection progress |
| `keepalive` | Persistent connections |
| `editor`, `pager` | Programs for `edit` and `less` |
| `namespace` | XML namespace for `propget` and `propset` |
| `lockowner`, `lockscope`, `lockdepth`, `lockstore` | Locking |
| `client-cert`, `client-cert-uri` | TLS client certificate |
| `proxy`, `proxy-port`, `systemproxy` | Proxy |
| `searchall`, `searchdepth`, `searchorder`, `searchdorder` | DASL search |
| `debug` | Protocol tracing; see below |

### Seeing what is on the wire

`set debug` turns on neon's own tracing, which goes to standard error:

```
dav:/path/> set debug http
dav:/path/> ls
```

The value is a comma-separated list of `http`, `xml`, `xmlparse`,
`socket`, `ssl`, `httpauth`, `httpbody`, `locks`, `files` and
`cleartext`. `set debug http,httpbody` is the usual starting point for
a request that is being answered in a way you did not expect;
`cleartext` also shows credentials, so do not paste that output
anywhere.

## Testing the build

Four harnesses, cheapest first. `make check` runs all of them.

```bash
./tests/offline.sh
```

18 checks that need no server, no network and no Python: that the
executable reports the version in `VERSION`, that the usage message and
the option errors are right, that a refused connection and an
unparsable URL are reported, and that a closed standard input ends the
session rather than hanging. Under a second.

```bash
./tests/glob.sh
```

48 unit tests for `lib/glob.c`, compiled on the spot against
`win32/config.h`. Most run the matcher against a synthetic directory
tree through the `GLOB_ALTDIRFUNC` callbacks, so they behave the same
everywhere; the rest create a real directory, because that path resolves
names differently on Windows.

```bash
./tests/godav.sh
./tests/wsgidav.sh
```

Ten scripted sessions — `basic`, `binary`, `copymove`, `edit`,
`errors`, `glob`, `locks`, `options`, `paths` and `props` — run against a real
WebDAV server, with the transcript compared line for line against
`tests/expected/`. Each session gets a collection of its own, created
through the server, so one cannot leave anything behind that another
trips over. `tests/normalise.py` replaces the parts that legitimately
differ between runs: timestamps, lock tokens, etags, the port, the
random half of a temporary file name.

`--regenerate` rewrites the expected transcripts after a deliberate
change. Read the diff before committing it: the point of those files is
that a change in behaviour has to be looked at.

Neither test server is complete, which is why both are used:

* [x/net/webdav](https://pkg.go.dev/golang.org/x/net/webdav), from the
  Go source in `tests/godav`, locks correctly but has no dead property
  store, so most of the `props` session fails against it. It needs a Go
  toolchain and, on the first run, network access.
* [wsgidav](https://github.com/mar10/wsgidav) has a dead property store,
  so `props` is meaningful there, but it answers `LOCK` with the
  `Content-Type` `application; charset=utf-8`, which is not a media
  type: neon discards the body unparsed and cadaver never sees the lock
  token. **No change to locking is visible against wsgidav.**

On the first run `tests/wsgidav.sh` creates a virtualenv in `dav-venv`
and installs wsgidav into it, which needs a **native Windows Python**
from <https://www.python.org/downloads/> or the Microsoft Store, and
network access. Later runs reuse it. Do not use MSYS2's own `python`:
wsgidav depends on `bcrypt`, which has no mingw wheel and needs a Rust
toolchain to build from source. The script detects an MSYS2 python and
skips over it; if it cannot find another, say where one is:

```bash
PYTHON=/c/Python313/python.exe ./tests/wsgidav.sh
```

CI runs all four on every push, for both build paths and for the static
build.

## Windows notes

**The wrong DLLs.** Git Bash ships its own `libssl`, `libcrypto` and
`libexpat` in `/mingw64/bin`. If those come first on `PATH` they shadow
the ones a dynamically linked build was built against. Put `ucrt64/bin`
first, or build with `STATIC=1`.

**`win32/config.h` is generated.** It is the output of `configure`,
checked in so `Makefile.w32` works without autotools.
`./win32/regen-config.sh` rewrites it: it runs `configure` in a scratch
directory, so a build in the source tree is left alone. Run it after
changing `configure.ac` or updating the bundled neon.

**`VERSION`.** The fork version lives in one file at the top of the
tree, read by `Makefile.w32` and by `configure.ac`, and reaching the
code as `-DPACKAGE_VERSION`. `win32/config.h` deliberately does not
carry a copy.

**Line endings.** cadaver writes its standard output in text mode, so
lines end CRLF on Windows. Transfers do not: everything cadaver puts or
gets is a byte stream, and `cat` switches its own output to binary for
the length of the transfer.

**IPv6.** `configure` does not define `USE_GETADDRINFO` on MinGW, so
neon falls back to the older resolver. IPv4 works; IPv6 may not.

## Differences from the upstream source

The Windows portability work, the neon patches and this fork's own
fixes are recorded in [NEWS](NEWS). The changes inside `neon/` are
listed separately in [PATCHES.md](PATCHES.md), because anyone updating
the bundled neon has to reapply them. What is left to do, including the
upstream issues this fork has inherited, is in [TODO](TODO).

[AGENTS.md](AGENTS.md) is written for a program driving cadaver rather
than a person typing at it.

## Licensing

cadaver is under the GNU GPL. See [COPYING](COPYING).

```
cadaver is Copyright (C) 1999-2025 Joe Orton
Portions are:
Copyright (C) 85, 88, 90, 91, 1995-1999 Free Software Foundation, Inc.
Copyright (C) GRASE Lab, UCSC
```

The bundled neon library is under the GNU Library GPL. See
[neon/src/COPYING.LIB](neon/src/COPYING.LIB) and
[neon/AUTHORS](neon/AUTHORS).

```
neon is Copyright (C) 1999-2025 Joe Orton
```

Report anything that is not Windows specific to
[upstream](https://github.com/notroj/cadaver/issues).
