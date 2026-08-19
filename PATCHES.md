# Patches applied to the bundled neon

The `neon/` directory is a copy of [notroj/neon](https://github.com/notroj/neon)
checked in as ordinary files. It is not a submodule. The copy is neon
0.37.1, commit `170c36704bfc2ac8b0e5607c3e1fdd4c159674db`.

If you replace it with a newer neon, the changes below have to be
applied again. Upstream neon does not carry any of them.

To see the current state of the patches:

```bash
git log --oneline -- neon/
```

All four are portability fixes, three of them shared with
[litmus-windows](https://github.com/Azathothas/litmus-windows), which
vendors the same neon.

## Portability

### 1. `src/ne_socket.c`: setsockopt argument type

Two `setsockopt` calls passed an `int *` for the option value. POSIX
declares that parameter `const void *`, so this is fine on Unix, but the
Winsock declaration is `const char *`. GCC 14 and later treat the
mismatch as an error rather than a warning, so the build fails.

Both calls now cast to `const char *`. The affected calls are
`SO_REUSEADDR` in `do_bind()` and `TCP_NODELAY` in `ne_sock_connect()`.

### 2. `src/ne_defs.h`: symbol visibility on PE

`NE_PRIVATE` was defined as `__attribute__((visibility ("hidden")))` for
any GCC 3 or later. PE/COFF has no symbol visibility, so GCC ignores the
attribute and warns about it once per translation unit that uses one —
eight warnings in a clean build, none of them actionable.

The definition is now skipped on `_WIN32` and `__CYGWIN__`. The
unconditional `#ifndef NE_PRIVATE / #define NE_PRIVATE` further down the
same file already supplies the empty fallback, so nothing else changes.

### 3. `src/ne_dates.c`: `gmt_to_local_win32` was not static

The Windows branch of the local-time offset defines
`gmt_to_local_win32()` with external linkage, although it is used only
by the `GMTOFF` macro a few lines above it and is declared nowhere. With
`--enable-warnings`, which turns on `-Wmissing-declarations`, that is
the one warning left in a clean Windows build.

It is now `static`.

### 4. `src/Makefile.in`: missing distclean target

cadaver's own `distclean` runs `cd neon/src && $(MAKE) distclean`, but
`neon/src/Makefile.in` only defined `clean`, so `make distclean` failed
at that line. A `distclean` target that removes the generated `Makefile`
has been added.

## What was pruned

Upstream neon ships more than a library. Four directories are not in
this copy, because cadaver cannot use them and a checked-in tree should
not carry code that is never built:

| Removed | Why |
| --- | --- |
| `neon/test/` | neon's own test suite, which needs neon's own build system. cadaver's tests are in `tests/`. |
| `neon/doc/` | The manual pages, published at <https://notroj.github.io/neon/>. |
| `neon/po/` | Translations for neon's own messages, which need gettext; this fork does not build with NLS. |
| `neon/.github/` | neon's CI workflows. GitHub only reads workflows from the repository root, so they could never have run. |

What is left is `neon/src`, which is the library, `neon/macros`, which
is what `configure.ac` uses through `NEON_VPATH_BUNDLED` and friends,
and the four files that carry neon's authorship and licensing:
`AUTHORS`, `NEWS`, `README.md` and `THANKS`. The library's licence is
`neon/src/COPYING.LIB`.

## Updating the bundled neon

1. Take the new `src` and `macros` directories from upstream.
2. Reapply the four patches above.
3. Regenerate the checked-in configuration, which records the neon
   version and the sizes and formats neon works out at configure time:

   ```bash
   ./win32/regen-config.sh
   ```

4. Rebuild both ways and run the gates:

   ```bash
   make -f Makefile.w32 && ./tests/offline.sh && ./tests/glob.sh
   ./tests/godav.sh && ./tests/wsgidav.sh
   ```

Both build paths must stay warning-free. The autotools one turns the
warnings on with `--enable-warnings`; `Makefile.w32` passes the same set
unconditionally.
