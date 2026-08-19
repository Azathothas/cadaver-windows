/*
   Temporary file creation for cadaver
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
#include <time.h>
#include <errno.h>
#include <fcntl.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#endif

#include "common.h"

/* The platform mkstemp() is not used directly for two reasons: it opens
 * the file in text mode on Windows, where cadaver then writes a
 * downloaded resource into it and gets a carriage return in front of
 * every newline; and the file has to be private to the user, which the
 * mode argument of open() only expresses on POSIX.  What is left is
 * short enough to be worth having in one place. */

#define LETTERS "abcdefghijklmnopqrstuvwxyz0123456789"
#define NLETTERS (sizeof LETTERS - 1)
#define NSUFFIX 6      /* the "XXXXXX" that gets replaced */
#define ATTEMPTS 4096

/* A value that differs between two runs started in the same second, and
 * between two cadavers running at once. */
static unsigned long seed_value(void)
{
    unsigned long v;

#ifdef _WIN32
    v = (unsigned long) GetTickCount() ^ ((unsigned long) _getpid() << 16);
#else
    v = (unsigned long) time(NULL) ^ ((unsigned long) getpid() << 16);
#endif

    return v ? v : 1;
}

int cad_mkstemp(char *template)
{
    static unsigned long state;
    char *x;
    int n;

    /* The placeholder does not have to be at the end: the `edit'
     * command puts the resource's extension after it, so that the
     * editor can guess the content type. */
    x = strstr(template, "XXXXXX");
    if (x == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (state == 0) state = seed_value();

    for (n = 0; n < ATTEMPTS; n++) {
        int fd, i;

        for (i = 0; i < NSUFFIX; i++) {
            /* xorshift, so that consecutive names do not differ in one
             * character and collide with another cadaver's sequence. */
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            x[i] = LETTERS[state % NLETTERS];
        }

        fd = open(template, O_RDWR | O_CREAT | O_EXCL | OPEN_BINARY_FLAGS,
                  0600);
        if (fd >= 0) return fd;

        if (errno != EEXIST) return -1;
    }

    errno = EEXIST;
    return -1;
}
