/*
   Unit tests for lib/netrc.c
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

/* Built and run by tests/netrc.sh, which is where the compiler
 * invocation lives.  A separate program rather than part of cadaver, so
 * that the shipped executable stays the only thing the build produces.
 *
 * The parser is worth testing on its own because what it gets wrong is
 * invisible in use: a password it mangles produces an authentication
 * failure that names neither the file nor the character.  Upstream
 * issue #75 was exactly that -- a generated password with an apostrophe
 * in it, which worked with every other WebDAV client.
 *
 * Each case writes a .netrc, parses it, and compares what came back
 * with what the file said.  Nothing here touches the network.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "netrc.h"

static int failures, checks;

static const char *tmpdir = ".";
static char netrc_path[1024];

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

/* Writes `content' to the scratch .netrc and parses it. */
static netrc_entry *parse(const char *content)
{
    FILE *f = fopen(netrc_path, "w");

    if (f == NULL) {
        fprintf(stderr, "netrctest: could not write %s\n", netrc_path);
        exit(1);
    }

    fputs(content, f);
    fclose(f);

    return parse_netrc(netrc_path);
}

/* The one shape almost every case has: one machine entry, whose login
 * and password are compared with what was expected.  A NULL expectation
 * means the field must be absent. */
static void check(const char *test, const char *content, const char *host,
                  const char *account, const char *password)
{
    netrc_entry *list = parse(content), *found;

    checks++;

    if (list == NULL) {
        fail(test, "the file parsed to no entries at all");
        return;
    }

    found = search_netrc(list, host);
    if (found == NULL) {
        fail(test, "no entry for host `%s'", host);
        return;
    }

    if (account == NULL) {
        if (found->account != NULL)
            fail(test, "login is `%s', expected none", found->account);
    }
    else if (found->account == NULL) {
        fail(test, "no login, expected `%s'", account);
    }
    else if (strcmp(found->account, account) != 0) {
        fail(test, "login is `%s', expected `%s'", found->account, account);
    }

    if (password == NULL) {
        if (found->password != NULL)
            fail(test, "password is `%s', expected none", found->password);
    }
    else if (found->password == NULL) {
        fail(test, "no password, expected `%s'", password);
    }
    else if (strcmp(found->password, password) != 0) {
        fail(test, "password is `%s', expected `%s'",
             found->password, password);
    }
}

/* Checks that a host has no entry at all. */
static void check_absent(const char *test, const char *content,
                         const char *host)
{
    netrc_entry *list = parse(content);

    checks++;

    if (list != NULL && search_netrc(list, host) != NULL)
        fail(test, "found an entry for `%s', expected none", host);
}

int main(int argc, char **argv)
{
    if (argc > 1) tmpdir = argv[1];

    snprintf(netrc_path, sizeof netrc_path, "%s/netrc-test", tmpdir);

    /* --- the ordinary shapes ------------------------------------- */

    check("a plain entry",
          "machine dav.example.com\nlogin alice\npassword s3cret\n",
          "dav.example.com", "alice", "s3cret");

    check("all on one line",
          "machine dav.example.com login alice password s3cret\n",
          "dav.example.com", "alice", "s3cret");

    check("user is a synonym for login",
          "machine dav.example.com user alice password s3cret\n",
          "dav.example.com", "alice", "s3cret");

    check("passwd is a synonym for password",
          "machine dav.example.com login alice passwd s3cret\n",
          "dav.example.com", "alice", "s3cret");

    check("a default entry matches any host",
          "default login alice password s3cret\n",
          "anything.example.net", "alice", "s3cret");

    check("a named entry wins over default",
          "default login nobody password nothing\n"
          "machine dav.example.com login alice password s3cret\n",
          "dav.example.com", "alice", "s3cret");

    check("the host match is case-insensitive",
          "machine DAV.example.com login alice password s3cret\n",
          "dav.EXAMPLE.com", "alice", "s3cret");

    check_absent("an unrelated host has no entry",
                 "machine dav.example.com login alice password s3cret\n",
                 "other.example.com");

    /* --- quoting, which is upstream issue #75 --------------------- */

    check("an apostrophe inside a password is kept",
          "machine dav.example.com\nlogin alice\npassword p@ss'word\n",
          "dav.example.com", "alice", "p@ss'word");

    check("a double quote inside a password is kept",
          "machine dav.example.com\nlogin alice\npassword p@ss\"word\n",
          "dav.example.com", "alice", "p@ss\"word");

    check("a password that is nothing but quotes is kept",
          "machine dav.example.com\nlogin alice\npassword a''b\n",
          "dav.example.com", "alice", "a''b");

    check("a quote at the end of a password is kept",
          "machine dav.example.com\nlogin alice\npassword s3cret'\n",
          "dav.example.com", "alice", "s3cret'");

    check("a quote inside a login is kept",
          "machine dav.example.com\nlogin o'brien\npassword s3cret\n",
          "dav.example.com", "o'brien", "s3cret");

    /* A quote that opens the token still quotes, which is what lets a
     * value contain a space. */
    check("a quoted value may contain spaces",
          "machine dav.example.com\nlogin alice\npassword \"two words\"\n",
          "dav.example.com", "alice", "two words");

    check("a single-quoted value may contain spaces",
          "machine dav.example.com\nlogin alice\npassword 'two words'\n",
          "dav.example.com", "alice", "two words");

    check("a quoted value may contain the other quote",
          "machine dav.example.com\nlogin alice\npassword \"it's here\"\n",
          "dav.example.com", "alice", "it's here");

    check("text after a closing quote joins the token",
          "machine dav.example.com\nlogin alice\npassword \"ab\"cd\n",
          "dav.example.com", "alice", "abcd");

    check("an unterminated quote runs to the end of the line",
          "machine dav.example.com\nlogin alice\npassword 'abc\n",
          "dav.example.com", "alice", "abc");

    check("a hash inside a quoted value is not a comment",
          "machine dav.example.com\nlogin alice\npassword \"a#b\"\n",
          "dav.example.com", "alice", "a#b");

    /* --- partial entries, which is upstream issue #25 ------------- */

    check("a login with no password is still an entry",
          "machine dav.example.com\nlogin alice\n",
          "dav.example.com", "alice", NULL);

    /* --- the rest of the format ----------------------------------- */

    check("a comment line is ignored",
          "# a comment\nmachine dav.example.com\nlogin alice\n"
          "password s3cret\n",
          "dav.example.com", "alice", "s3cret");

    check("a trailing comment is ignored",
          "machine dav.example.com login alice password s3cret # hi\n",
          "dav.example.com", "alice", "s3cret");

    check("blank lines are ignored",
          "\n\nmachine dav.example.com\n\nlogin alice\n\npassword s3cret\n\n",
          "dav.example.com", "alice", "s3cret");

    check("the second of two machines",
          "machine one.example.com login bob password bobs\n"
          "machine dav.example.com login alice password s3cret\n",
          "dav.example.com", "alice", "s3cret");

    check("the first of two machines",
          "machine dav.example.com login alice password s3cret\n"
          "machine two.example.com login bob password bobs\n",
          "dav.example.com", "alice", "s3cret");

    check("a file with no trailing newline",
          "machine dav.example.com login alice password s3cret",
          "dav.example.com", "alice", "s3cret");

    remove(netrc_path);

    putchar('\n');
    if (failures == 0) {
        printf("%d checks, all passed\n", checks);
        return 0;
    }

    printf("%d checks, %d failed\n", checks, failures);
    return 1;
}
