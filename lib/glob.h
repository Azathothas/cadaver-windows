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

#ifndef CAD_GLOB_H
#define CAD_GLOB_H

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stddef.h>

/* A glob() for cadaver, which expands a pattern either against the
 * local file system or -- through the gl_* callbacks -- against a
 * collection on the server.
 *
 * The interface is the subset of the GNU one that cadaver uses, and
 * only the flags implemented here are defined: passing a flag this
 * does not support is a compile error rather than a silent no-op.
 *
 * A pattern is matched one path segment at a time.  Within a segment:
 *
 *   *          any run of characters, including none
 *   ?          any one character
 *   [abc]      any one of the characters listed
 *   [a-z]      any one character in the range
 *   [!abc]     any one character not listed; [^abc] means the same
 *   [[:alpha:]]  any one character in the named POSIX class
 *
 * None of them match a path separator, and none match a leading dot
 * unless the pattern has one too, unless GLOB_PERIOD is given.
 *
 * A backslash quotes the character after it, unless GLOB_NOESCAPE is
 * given -- or unless the backslash is a path separator, which it is
 * when matching the local file system on Windows.  A remote pattern is
 * always separated by forward slashes, so a backslash always quotes
 * there.
 */

/* Flags for the second argument to glob(). */
#define GLOB_ERR        (1 << 0) /* stop on a directory that cannot be read */
#define GLOB_MARK       (1 << 1) /* append a separator to each directory */
#define GLOB_NOSORT     (1 << 2) /* return in enumeration order, unsorted */
#define GLOB_NOCHECK    (1 << 3) /* if nothing matches, return the pattern */
#define GLOB_NOESCAPE   (1 << 4) /* backslash is an ordinary character */
#define GLOB_PERIOD     (1 << 5) /* a wildcard may match a leading dot */
#define GLOB_ALTDIRFUNC (1 << 6) /* use the gl_* callbacks below */

/* Return values. */
#define GLOB_NOSPACE 1  /* out of memory */
#define GLOB_ABORTED 2  /* GLOB_ERR was set, errfunc asked to stop, or the
                         * caller's gl_abort flag went non-zero */
#define GLOB_NOMATCH 3  /* nothing matched and GLOB_NOCHECK was not set */

typedef struct {
    size_t gl_pathc;    /* number of paths matched */
    char **gl_pathv;    /* the paths, NULL-terminated; NULL if none */
    int gl_flags;       /* the flags the call was made with */

    /* With GLOB_ALTDIRFUNC these replace opendir(), readdir(),
     * closedir() and stat().  All four must be set; the expansion
     * never falls back to the real ones for part of a pattern.  Only
     * d_name is read from the dirent, and only st_mode from the stat
     * buffer.
     *
     * There is no gl_lstat: the distinction between a symbolic link
     * and what it points at has no counterpart in a collection on a
     * server, which is the only thing these callbacks describe. */
    void *(*gl_opendir)(const char *);
    struct dirent *(*gl_readdir)(void *);
    void (*gl_closedir)(void *);
    int (*gl_stat)(const char *, struct stat *);

    /* If not NULL, checked before every directory is read and after
     * every entry.  glob() returns GLOB_ABORTED as soon as it is
     * non-zero, so cadaver can interrupt an expansion that is
     * making requests to a slow server.  Set it from a signal handler;
     * nothing else is safe to do from one. */
    volatile int *gl_abort;
} glob_t;

/* Expands `pattern' into `pglob'.  `errfunc', if not NULL, is called
 * with the path and errno of any directory that could not be read; if
 * it returns non-zero the expansion stops with GLOB_ABORTED.  Returns
 * zero on success, or one of the GLOB_* values above.  `pglob' must be
 * zeroed by the caller and is left safe to pass to globfree() whatever
 * the outcome. */
int glob(const char *pattern, int flags,
         int (*errfunc)(const char *, int), glob_t *pglob);

/* Releases everything glob() allocated in `pglob' and zeroes the result
 * fields, so calling it twice is harmless. */
void globfree(glob_t *pglob);

#endif /* CAD_GLOB_H */
