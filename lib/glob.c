/*
   Filename globbing for cadaver
   Copyright (C) 2026 the cadaver-windows authors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include "glob.h"

/* Expansion proceeds one path segment at a time.  A working set holds
 * the directory prefixes reached so far -- initially just the root of
 * the pattern -- and each segment replaces it with the set of paths
 * that matched.  A segment with no wildcard in it is appended without
 * reading a directory, which is what keeps a pattern like "dir" plus a
 * wildcard to one request against a server rather than one per entry.
 *
 * There is no recursion, so a pattern with many segments costs no
 * stack, and the abort flag is honoured between every directory read.
 */

struct strvec {
    char **v;
    size_t n;      /* entries in use, not counting the NULL terminator */
    size_t alloc;  /* entries allocated, including room for the NULL */
};

/* State threaded through the expansion, so that the flags and the
 * callbacks do not have to be passed to everything separately. */
struct globctx {
    const glob_t *g;
    int flags;
    int (*errfunc)(const char *, int);
    int esc;       /* backslash quotes the next character */
    int fold;      /* matching ignores case */
    int alt;       /* the gl_* callbacks are in use */
};

/* Windows has two path separators and no meaningful case distinction in
 * file names.  A remote pattern is a URI path, so neither applies to
 * it; ctx->alt selects between the two. */
#ifdef _WIN32
#define NATIVE_SEP(c) ((c) == '/' || (c) == '\\')
#else
#define NATIVE_SEP(c) ((c) == '/')
#endif

static int is_sep(const struct globctx *ctx, char c)
{
    return ctx->alt ? (c == '/') : NATIVE_SEP(c);
}

static int vec_push(struct strvec *sv, char *s)
{
    if (sv->n + 2 > sv->alloc) {
        size_t want = sv->alloc ? sv->alloc * 2 : 16;
        char **nv = realloc(sv->v, want * sizeof *nv);

        if (nv == NULL) return -1;
        sv->v = nv;
        sv->alloc = want;
    }

    sv->v[sv->n++] = s;
    sv->v[sv->n] = NULL;
    return 0;
}

static void vec_free(struct strvec *sv)
{
    size_t n;

    for (n = 0; n < sv->n; n++) free(sv->v[n]);
    free(sv->v);
    sv->v = NULL;
    sv->n = sv->alloc = 0;
}

/* Joins a directory prefix and a name.  An empty prefix means "the
 * directory the pattern started from", whose members are named without
 * any prefix at all -- so that `glob("*.c")' returns "foo.c" rather
 * than "./foo.c". */
static char *path_join(const struct globctx *ctx, const char *dir,
                       const char *name)
{
    size_t dlen = strlen(dir), nlen = strlen(name);
    int sep = dlen > 0 && !is_sep(ctx, dir[dlen - 1]);
    char *ret = malloc(dlen + (size_t)sep + nlen + 1);

    if (ret == NULL) return NULL;

    memcpy(ret, dir, dlen);
    if (sep) ret[dlen] = '/';
    memcpy(ret + dlen + sep, name, nlen + 1);

    return ret;
}

/* The name to open a prefix by: an empty prefix is the starting
 * directory, which is spelled "." both for opendir() and for cadaver's
 * gl_opendir(), where it resolves to the current collection. */
static const char *dir_name(const char *prefix)
{
    return *prefix ? prefix : ".";
}

static int has_magic(const struct globctx *ctx, const char *seg, size_t len)
{
    size_t n;

    for (n = 0; n < len; n++) {
        switch (seg[n]) {
        case '*': case '?': case '[':
            return 1;
        case '\\':
            if (ctx->esc) n++;
            break;
        }
    }

    return 0;
}

/* Strips the quoting backslashes from a segment that turned out to have
 * no wildcards in it, so that a segment written as "foo\ bar" looks for
 * the name "foo bar". */
static char *unquote(const struct globctx *ctx, const char *seg, size_t len)
{
    char *ret = malloc(len + 1);
    size_t in, out = 0;

    if (ret == NULL) return NULL;

    for (in = 0; in < len; in++) {
        if (ctx->esc && seg[in] == '\\' && in + 1 < len) in++;
        ret[out++] = seg[in];
    }

    ret[out] = '\0';
    return ret;
}

static int chr_eq(const struct globctx *ctx, char a, char b)
{
    if (a == b) return 1;
    if (!ctx->fold) return 0;

    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

/* Matches one [...] set against `c', with `p' pointing just after the
 * opening bracket.  Returns the position just after the closing
 * bracket, or NULL if the set is unterminated -- in which case the
 * caller treats the bracket as an ordinary character, as a shell
 * does. */
static const char *match_set(const struct globctx *ctx, const char *p,
                             char c, int *matched)
{
    int negate = 0, found = 0, first = 1;

    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }

    for (; *p; first = 0, p++) {
        if (*p == ']' && !first) break;

        if (p[0] == '[' && p[1] == ':') {
            const char *end = strstr(p + 2, ":]");
            char name[16];
            size_t len;

            if (end == NULL) return NULL;

            len = (size_t)(end - (p + 2));
            if (len < sizeof name) {
                unsigned char u = (unsigned char)c;

                memcpy(name, p + 2, len);
                name[len] = '\0';

                if ((strcmp(name, "alpha") == 0 && isalpha(u))
                    || (strcmp(name, "digit") == 0 && isdigit(u))
                    || (strcmp(name, "alnum") == 0 && isalnum(u))
                    || (strcmp(name, "space") == 0 && isspace(u))
                    || (strcmp(name, "upper") == 0 && isupper(u))
                    || (strcmp(name, "lower") == 0 && islower(u))
                    || (strcmp(name, "punct") == 0 && ispunct(u))
                    || (strcmp(name, "print") == 0 && isprint(u))
                    || (strcmp(name, "graph") == 0 && isgraph(u))
                    || (strcmp(name, "cntrl") == 0 && iscntrl(u))
                    || (strcmp(name, "blank") == 0 && (c == ' ' || c == '\t'))
                    || (strcmp(name, "xdigit") == 0 && isxdigit(u)))
                    found = 1;

                /* A class cadaver does not know is no match rather than
                 * an error, which is what leaving found alone does. */
            }

            p = end + 1; /* the loop's p++ steps past the ']' */
            continue;
        }

        if (ctx->esc && *p == '\\' && p[1]) p++;

        /* A range, unless the '-' is the last character before the
         * closing bracket, where it stands for itself. */
        if (p[1] == '-' && p[2] && p[2] != ']') {
            char lo = p[0], hi = p[2];

            if (ctx->fold) {
                char c1 = (char)tolower((unsigned char)c);

                if ((c1 >= tolower((unsigned char)lo)
                     && c1 <= tolower((unsigned char)hi))
                    || (c >= lo && c <= hi))
                    found = 1;
            }
            else if (c >= lo && c <= hi) {
                found = 1;
            }

            p += 2;
            continue;
        }

        if (chr_eq(ctx, *p, c)) found = 1;
    }

    if (*p != ']') return NULL;

    *matched = negate ? !found : found;
    return p + 1;
}

/* Matches one path segment.  `pat' and `str' are NUL-terminated copies
 * of the segment and the candidate name.  Iterative with a single
 * backtrack point, so a pattern like "*a*b*c" costs no stack and cannot
 * blow up on a long name. */
static int seg_match(const struct globctx *ctx, const char *pat,
                     const char *str)
{
    const char *star_pat = NULL, *star_str = NULL;

    while (*str) {
        int matched = 0;
        const char *next;

        switch (*pat) {
        case '*':
            /* Remember where to resume if the rest fails to match, and
             * try the shortest expansion first. */
            star_pat = ++pat;
            star_str = str;
            continue;

        case '?':
            pat++;
            str++;
            continue;

        case '[':
            next = match_set(ctx, pat + 1, *str, &matched);
            if (next != NULL) {
                if (matched) {
                    pat = next;
                    str++;
                    continue;
                }
                break; /* backtrack */
            }
            /* Unterminated set: the bracket is a literal. */
            /* fall through */

        default: {
            const char *p = pat;

            if (ctx->esc && *p == '\\' && p[1]) p++;
            if (*p && chr_eq(ctx, *p, *str)) {
                pat = p + 1;
                str++;
                continue;
            }
            break; /* backtrack */
        }
        }

        if (star_pat == NULL) return 0;

        /* Let the last '*' swallow one more character and retry. */
        pat = star_pat;
        str = ++star_str;
    }

    while (*pat == '*') pat++;

    return *pat == '\0';
}

/* Whether `name' may be considered at all: entries starting with a dot
 * are hidden from a wildcard, as in a shell, and "." and ".." never
 * appear in a result. */
static int visible(const struct globctx *ctx, const char *pat,
                   const char *name)
{
    if (name[0] != '.') return 1;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;

    if (ctx->flags & GLOB_PERIOD) return 1;

    return pat[0] == '.';
}

static int cad_stat(const struct globctx *ctx, const char *path,
                    struct stat *st)
{
    if (ctx->alt) {
        /* Never the real stat() while expanding a remote pattern: the
         * path is a name on the server and asking the local file
         * system about it would answer a different question. */
        if (ctx->g->gl_stat == NULL) {
            errno = ENOSYS;
            return -1;
        }

        return ctx->g->gl_stat(path, st);
    }

    return stat(path, st);
}

static int is_dir(const struct globctx *ctx, const char *path)
{
    struct stat st;

    memset(&st, 0, sizeof st);
    if (cad_stat(ctx, path, &st) != 0) return 0;

    return S_ISDIR(st.st_mode) ? 1 : 0;
}

static int exists(const struct globctx *ctx, const char *path)
{
    struct stat st;

    memset(&st, 0, sizeof st);
    return cad_stat(ctx, path, &st) == 0;
}

static int aborted(const struct globctx *ctx)
{
    return ctx->g->gl_abort != NULL && *ctx->g->gl_abort != 0;
}

/* Reads one directory, appending every entry matching `pat' to `out'.
 * Returns 0, GLOB_NOSPACE or GLOB_ABORTED. */
static int expand_dir(struct globctx *ctx, const char *prefix,
                      const char *pat, int last, struct strvec *out)
{
    void *handle;
    struct dirent *ent;
    DIR *dp = NULL;
    int rv = 0;

    if (aborted(ctx)) return GLOB_ABORTED;

    errno = 0;
    if (ctx->alt) {
        handle = ctx->g->gl_opendir(dir_name(prefix));
    }
    else {
        dp = opendir(dir_name(prefix));
        handle = dp;
    }

    if (handle == NULL) {
        int err = errno;

        /* A prefix that is not a directory is not an error: it simply
         * contributes nothing, so a wildcard followed by a
         * further segment behaves when some of the names it matched are
         * plain files rather than directories. */
        if (err == ENOTDIR) return 0;

        if (ctx->errfunc && ctx->errfunc(dir_name(prefix), err))
            return GLOB_ABORTED;

        return (ctx->flags & GLOB_ERR) ? GLOB_ABORTED : 0;
    }

    for (;;) {
        char *path;

        if (ctx->alt)
            ent = ctx->g->gl_readdir(handle);
        else
            ent = readdir(dp);

        if (ent == NULL) break;

        if (aborted(ctx)) {
            rv = GLOB_ABORTED;
            break;
        }

        if (!visible(ctx, pat, ent->d_name)) continue;
        if (!seg_match(ctx, pat, ent->d_name)) continue;

        path = path_join(ctx, prefix, ent->d_name);
        if (path == NULL) {
            rv = GLOB_NOSPACE;
            break;
        }

        /* Everything but the last segment has to be a directory to be
         * worth descending into. */
        if (!last && !is_dir(ctx, path)) {
            free(path);
            continue;
        }

        if (vec_push(out, path)) {
            free(path);
            rv = GLOB_NOSPACE;
            break;
        }
    }

    if (ctx->alt)
        ctx->g->gl_closedir(handle);
    else
        closedir(dp);

    return rv;
}

/* The length of the part of the pattern that is not subject to
 * matching: "/" for an absolute path, "C:\" or "C:/" for a drive,
 * "//server/share/" for a UNC path, and nothing for a relative one. */
static size_t root_length(const struct globctx *ctx, const char *pat)
{
    size_t n = 0;

#ifdef _WIN32
    if (!ctx->alt) {
        if (isalpha((unsigned char)pat[0]) && pat[1] == ':') {
            n = NATIVE_SEP(pat[2]) ? 3 : 2;
            return n;
        }

        if (NATIVE_SEP(pat[0]) && NATIVE_SEP(pat[1])) {
            /* Skip the server and the share, which are not patterns. */
            int seen = 0;

            n = 2;
            while (pat[n] && seen < 2) {
                if (NATIVE_SEP(pat[n])) seen++;
                if (seen < 2) n++;
            }
            if (pat[n]) n++;
            return n;
        }
    }
#endif

    while (is_sep(ctx, pat[n])) n++;

    return n;
}

static int cmp_path(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int glob(const char *pattern, int flags,
         int (*errfunc)(const char *, int), glob_t *pglob)
{
    struct globctx ctx;
    struct strvec cur = {NULL, 0, 0}, next = {NULL, 0, 0};
    const char *p;
    char *root;
    size_t rootlen, n;
    int rv = 0;

    pglob->gl_pathc = 0;
    pglob->gl_pathv = NULL;
    pglob->gl_flags = flags;

    ctx.g = pglob;
    ctx.flags = flags;
    ctx.errfunc = errfunc;
    ctx.alt = (flags & GLOB_ALTDIRFUNC) != 0
        && pglob->gl_opendir && pglob->gl_readdir && pglob->gl_closedir;
    ctx.esc = (flags & GLOB_NOESCAPE) == 0;
    ctx.fold = 0;

#ifdef _WIN32
    /* The local file system is case-insensitive and its separator is
     * also the quoting character, so quoting is off there.  A remote
     * pattern is a URI path and neither applies. */
    if (!ctx.alt) {
        ctx.fold = 1;
        ctx.esc = 0;
    }
#endif

    rootlen = root_length(&ctx, pattern);
    root = malloc(rootlen + 1);
    if (root == NULL) return GLOB_NOSPACE;
    memcpy(root, pattern, rootlen);
    root[rootlen] = '\0';

    if (vec_push(&cur, root)) {
        free(root);
        return GLOB_NOSPACE;
    }

    p = pattern + rootlen;

    while (*p && rv == 0) {
        const char *seg = p;
        size_t seglen;
        int last, magic;

        while (*p && !is_sep(&ctx, *p)) {
            if (ctx.esc && *p == '\\' && p[1]) p++;
            p++;
        }

        seglen = (size_t)(p - seg);
        while (is_sep(&ctx, *p)) p++;
        last = (*p == '\0');

        /* "a//b" and a trailing separator both give an empty segment,
         * which names the directory itself. */
        if (seglen == 0) continue;

        magic = has_magic(&ctx, seg, seglen);

        for (n = 0; n < cur.n && rv == 0; n++) {
            if (magic) {
                char *pat = malloc(seglen + 1);

                if (pat == NULL) {
                    rv = GLOB_NOSPACE;
                    break;
                }
                memcpy(pat, seg, seglen);
                pat[seglen] = '\0';

                rv = expand_dir(&ctx, cur.v[n], pat, last, &next);
                free(pat);
            }
            else {
                /* No wildcard: no directory has to be read, so join the
                 * literal on and let the next segment -- or the final
                 * existence check -- rule it out. */
                char *lit = unquote(&ctx, seg, seglen);
                char *path;

                if (lit == NULL) {
                    rv = GLOB_NOSPACE;
                    break;
                }

                path = path_join(&ctx, cur.v[n], lit);
                free(lit);
                if (path == NULL) {
                    rv = GLOB_NOSPACE;
                    break;
                }

                if ((last && !exists(&ctx, path))
                    || (!last && !is_dir(&ctx, path))) {
                    free(path);
                    continue;
                }

                if (vec_push(&next, path)) {
                    free(path);
                    rv = GLOB_NOSPACE;
                }
            }
        }

        vec_free(&cur);
        cur = next;
        next.v = NULL;
        next.n = next.alloc = 0;

        if (cur.n == 0) break;
    }

    if (rv != 0) {
        vec_free(&cur);
        vec_free(&next);
        return rv;
    }

    if (cur.n == 0) {
        vec_free(&cur);

        if ((flags & GLOB_NOCHECK) == 0) return GLOB_NOMATCH;

        root = strdup(pattern);
        if (root == NULL || vec_push(&cur, root)) {
            free(root);
            vec_free(&cur);
            return GLOB_NOSPACE;
        }
    }
    else if ((flags & GLOB_NOSORT) == 0) {
        qsort(cur.v, cur.n, sizeof *cur.v, cmp_path);
    }

    if (flags & GLOB_MARK) {
        for (n = 0; n < cur.n; n++) {
            if (is_dir(&ctx, cur.v[n])) {
                char *marked = path_join(&ctx, cur.v[n], "");

                if (marked == NULL) {
                    vec_free(&cur);
                    return GLOB_NOSPACE;
                }
                free(cur.v[n]);
                cur.v[n] = marked;
            }
        }
    }

    pglob->gl_pathc = cur.n;
    pglob->gl_pathv = cur.v;

    return 0;
}

void globfree(glob_t *pglob)
{
    size_t n;

    if (pglob->gl_pathv == NULL) return;

    for (n = 0; n < pglob->gl_pathc; n++) free(pglob->gl_pathv[n]);
    free(pglob->gl_pathv);

    pglob->gl_pathv = NULL;
    pglob->gl_pathc = 0;
}
