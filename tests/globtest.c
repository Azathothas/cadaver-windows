/*
   Unit tests for lib/glob.c
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

/* Built and run by tests/glob.sh, which is where the compiler
 * invocation lives.  It is a separate program rather than part of
 * cadaver so that the shipped executable stays the only thing the build
 * produces.
 *
 * Most of the tests drive glob() through the GLOB_ALTDIRFUNC callbacks
 * against a directory tree described by a table, so the matcher is
 * exercised without touching the file system at all and the same
 * expectations hold on every platform.  A second, smaller set creates a
 * real directory, because that path resolves names differently: two
 * separators and no case distinction on Windows, one separator and case
 * sensitivity elsewhere.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

#include "glob.h"

static int failures, checks;

static void fail(const char *test, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__ ((format (printf, 2, 3)))
#endif
;

static void fail(const char *test, const char *fmt, ...)
{
    va_list ap;

    printf("FAIL %s: ", test);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    failures++;
}

/* --- the synthetic tree ------------------------------------------- */

/* One row per entry.  `dir' is the directory it lives in, spelled the
 * way glob() will ask for it: "." for the top. */
struct entry {
    const char *dir;
    const char *name;
    int is_dir;
};

static const struct entry TREE[] = {
    {".", "alpha.txt", 0},
    {".", "beta.txt", 0},
    {".", "Gamma.TXT", 0},
    {".", "delta.dat", 0},
    {".", ".hidden", 0},
    {".", "one", 1},
    {".", "two", 1},
    /* A directory the callbacks refuse to open, so that the error
     * path has something to report. */
    {".", "denied", 1},
    {".", "plain", 0},
    {"one", "a1.txt", 0},
    {"one", "a2.txt", 0},
    {"one", "deep", 1},
    {"two", "b1.txt", 0},
    {"one/deep", "buried.txt", 0},
    {NULL, NULL, 0}
};

struct fake_dir {
    const char *dir;
    int index;
    struct dirent ent;
};

static int opendir_calls;

static void *fake_opendir(const char *dir)
{
    struct fake_dir *d;
    int n, found = 0;

    for (n = 0; TREE[n].dir; n++)
        if (strcmp(TREE[n].dir, dir) == 0) found = 1;

    if (!found) {
        errno = ENOENT;
        return NULL;
    }

    opendir_calls++;
    d = calloc(1, sizeof *d);
    d->dir = dir;
    d->index = 0;
    return d;
}

static struct dirent *fake_readdir(void *handle)
{
    struct fake_dir *d = handle;

    while (TREE[d->index].dir) {
        const struct entry *e = &TREE[d->index++];

        if (strcmp(e->dir, d->dir) != 0) continue;

        memset(&d->ent, 0, sizeof d->ent);
        strncpy(d->ent.d_name, e->name, sizeof d->ent.d_name - 1);
        return &d->ent;
    }

    return NULL;
}

static void fake_closedir(void *handle)
{
    free(handle);
}

static int fake_stat(const char *path, struct stat *st)
{
    int n;
    const char *slash;
    const char *dir, *name;
    char dirbuf[256];

    slash = strrchr(path, '/');
    if (slash) {
        size_t len = (size_t)(slash - path);

        if (len >= sizeof dirbuf) return -1;
        memcpy(dirbuf, path, len);
        dirbuf[len] = '\0';
        dir = len ? dirbuf : ".";
        name = slash + 1;
    }
    else {
        dir = ".";
        name = path;
    }

    for (n = 0; TREE[n].dir; n++) {
        if (strcmp(TREE[n].dir, dir) == 0
            && strcmp(TREE[n].name, name) == 0) {
            memset(st, 0, sizeof *st);
            st->st_mode = TREE[n].is_dir ? S_IFDIR : S_IFREG;
            return 0;
        }
    }

    errno = ENOENT;
    return -1;
}

static void alt_setup(glob_t *g)
{
    memset(g, 0, sizeof *g);
    g->gl_opendir = fake_opendir;
    g->gl_readdir = fake_readdir;
    g->gl_closedir = fake_closedir;
    g->gl_stat = fake_stat;
}

/* Runs one pattern against the synthetic tree and compares the result
 * with `want', a comma-separated list in the order glob() should have
 * produced. */
static void expect(const char *pattern, int flags, const char *want)
{
    glob_t g;
    char got[512];
    size_t n;
    int rv;

    checks++;

    alt_setup(&g);
    rv = glob(pattern, flags | GLOB_ALTDIRFUNC, NULL, &g);

    if (rv != 0 && rv != GLOB_NOMATCH) {
        fail(pattern, "returned %d", rv);
        globfree(&g);
        return;
    }

    got[0] = '\0';
    for (n = 0; n < g.gl_pathc; n++) {
        if (n) strcat(got, ",");
        if (strlen(got) + strlen(g.gl_pathv[n]) + 2 > sizeof got) break;
        strcat(got, g.gl_pathv[n]);
    }

    if (strcmp(got, want) != 0)
        fail(pattern, "got [%s], wanted [%s]", got, want);

    globfree(&g);

    /* A second globfree must be safe: cadaver calls it on the failure
     * paths too, where the first one may already have run. */
    globfree(&g);
}

static void expect_rv(const char *pattern, int flags, int want)
{
    glob_t g;
    int rv;

    checks++;

    alt_setup(&g);
    rv = glob(pattern, flags | GLOB_ALTDIRFUNC, NULL, &g);
    if (rv != want) fail(pattern, "returned %d, wanted %d", rv, want);
    globfree(&g);
}

static void test_matching(void)
{
    /* A wildcard against one segment. */
    expect("*.txt", 0, "alpha.txt,beta.txt");
    expect("*.dat", 0, "delta.dat");
    expect("*", 0,
           "Gamma.TXT,alpha.txt,beta.txt,delta.dat,denied,one,plain,two");

    /* A single character. */
    expect("?????.txt", 0, "alpha.txt");
    expect("????.txt", 0, "beta.txt");

    /* Sets, ranges, negation and a POSIX class. */
    expect("[ab]*.txt", 0, "alpha.txt,beta.txt");
    expect("[a-b]*.txt", 0, "alpha.txt,beta.txt");
    expect("[!ab]*.txt", 0, "");
    expect("[^ab]*.txt", 0, "");
    expect("[[:alpha:]]lpha.txt", 0, "alpha.txt");
    expect("[[:digit:]]lpha.txt", 0, "");

    /* An unterminated set is a literal bracket, as in a shell. */
    expect("[abc", 0, "");

    /* A leading dot is not matched by a wildcard unless the pattern
     * has one too, or GLOB_PERIOD is given. */
    expect(".*", 0, ".hidden");
    expect("*hidden", 0, "");
    expect("*hidden", GLOB_PERIOD, ".hidden");

    /* Matching is case sensitive when the callbacks are in use, because
     * the names are URI path segments rather than file names. */
    expect("gamma*", 0, "");
    expect("Gamma*", 0, "Gamma.TXT");

    /* More than one segment, with a wildcard in each. */
    expect("one/*.txt", 0, "one/a1.txt,one/a2.txt");
    expect("*/b1.txt", 0, "two/b1.txt");
    expect("*/*.txt", 0, "one/a1.txt,one/a2.txt,two/b1.txt");
    expect("*/*/*.txt", 0, "one/deep/buried.txt");
    expect("one/deep/*", 0, "one/deep/buried.txt");

    /* A segment that is not a directory contributes nothing rather
     * than failing the whole expansion. */
    expect("*/a1.txt", 0, "one/a1.txt");

    /* A literal segment costs no directory read: it is joined on and
     * checked with stat.  A name that does not exist drops out. */
    expect("one/a1.txt", 0, "one/a1.txt");
    expect("one/nosuch.txt", 0, "");
    expect("nosuch/a1.txt", 0, "");

    /* That is not just an implementation detail: against a server each
     * directory read is a PROPFIND, so a pattern with no wildcard in it
     * has to cost none. */
    checks++;
    opendir_calls = 0;
    expect("one/deep/buried.txt", 0, "one/deep/buried.txt");
    if (opendir_calls != 0)
        fail("literal segments", "read %d directories, wanted 0",
             opendir_calls);

    /* Trailing and doubled separators name the directory itself. */
    expect("one/", 0, "one");
    expect("one//a1.txt", 0, "one/a1.txt");

    /* Return values. */
    expect_rv("nosuch*", 0, GLOB_NOMATCH);
    expect_rv("*.txt", 0, 0);

    /* GLOB_NOCHECK returns the pattern when nothing matched. */
    expect("nosuch*", GLOB_NOCHECK, "nosuch*");

    /* GLOB_MARK marks the directories. */
    expect("one", GLOB_MARK, "one/");
    expect("*.dat", GLOB_MARK, "delta.dat");

    /* GLOB_NOSORT leaves the enumeration order alone, which for the
     * table above is the order the rows are in. */
    expect("*.txt", GLOB_NOSORT, "alpha.txt,beta.txt");
}

static void test_escaping(void)
{
    /* A backslash quotes a wildcard, so the pattern is a literal that
     * matches nothing here.  With GLOB_NOESCAPE the backslash is an
     * ordinary character and the star still matches. */
    expect("alpha\\.txt", 0, "alpha.txt");
    expect("\\*.txt", 0, "");
}

static void test_abort(void)
{
    volatile int flag = 1;
    glob_t g;
    int rv;

    checks++;

    alt_setup(&g);
    g.gl_abort = &flag;
    rv = glob("*.txt", GLOB_ALTDIRFUNC, NULL, &g);
    if (rv != GLOB_ABORTED) fail("gl_abort", "returned %d, wanted GLOB_ABORTED", rv);
    globfree(&g);
}

static int errfunc_calls;

static int count_errors(const char *path, int err)
{
    (void) path;
    (void) err;
    errfunc_calls++;
    return 0;
}

static void test_errfunc(void)
{
    glob_t g;
    int rv;

    checks++;

    /* A directory that cannot be opened calls errfunc, and without
     * GLOB_ERR the expansion carries on and simply finds nothing. */
    errfunc_calls = 0;
    alt_setup(&g);
    rv = glob("denied/*", GLOB_ALTDIRFUNC, count_errors, &g);
    if (rv != GLOB_NOMATCH)
        fail("errfunc", "returned %d, wanted GLOB_NOMATCH", rv);
    if (errfunc_calls != 1)
        fail("errfunc", "called %d times, wanted 1", errfunc_calls);
    globfree(&g);

    /* With GLOB_ERR the same failure stops the expansion instead. */
    checks++;
    errfunc_calls = 0;
    alt_setup(&g);
    rv = glob("denied/*", GLOB_ALTDIRFUNC | GLOB_ERR, count_errors, &g);
    if (rv != GLOB_ABORTED)
        fail("errfunc/GLOB_ERR", "returned %d, wanted GLOB_ABORTED", rv);
    globfree(&g);
}

/* --- the real file system ------------------------------------------ */

static void write_file(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        fail("setup", "could not create %s: %s", path, strerror(errno));
        return;
    }
    fputs("x\n", f);
    fclose(f);
}

static void expect_native(const char *pattern, const char *want)
{
    glob_t g;
    char got[512];
    size_t n;
    int rv;

    checks++;

    memset(&g, 0, sizeof g);
    rv = glob(pattern, 0, NULL, &g);

    if (rv != 0 && rv != GLOB_NOMATCH) {
        fail(pattern, "returned %d", rv);
        globfree(&g);
        return;
    }

    got[0] = '\0';
    for (n = 0; n < g.gl_pathc; n++) {
        if (n) strcat(got, ",");
        if (strlen(got) + strlen(g.gl_pathv[n]) + 2 > sizeof got) break;
        strcat(got, g.gl_pathv[n]);
    }

    if (strcmp(got, want) != 0)
        fail(pattern, "got [%s], wanted [%s]", got, want);

    globfree(&g);
}

static void test_native(const char *root)
{
    if (MKDIR(root) != 0 && errno != EEXIST) {
        fail("setup", "could not create %s: %s", root, strerror(errno));
        return;
    }

    if (chdir(root) != 0) {
        fail("setup", "could not enter %s: %s", root, strerror(errno));
        return;
    }

    write_file("one.txt");
    write_file("two.txt");
    write_file("three.dat");
    MKDIR("sub");
    write_file("sub/nested.txt");

    expect_native("*.txt", "one.txt,two.txt");
    expect_native("*.dat", "three.dat");
    expect_native("sub/*.txt", "sub/nested.txt");
    expect_native("nosuch*", "");

#ifdef _WIN32
    /* The Windows file system is case-insensitive, and the backslash is
     * a separator there rather than a quoting character. */
    expect_native("*.TXT", "one.txt,two.txt");
    expect_native("ONE.TXT", "ONE.TXT");
    expect_native("sub\\*.txt", "sub/nested.txt");
#else
    expect_native("*.TXT", "");
#endif
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "globtest-work";

    test_matching();
    test_escaping();
    test_abort();
    test_errfunc();
    test_native(root);

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
