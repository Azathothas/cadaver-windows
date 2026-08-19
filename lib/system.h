/*
   Platform-specific helpers for cadaver
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

#ifndef CAD_SYSTEM_H
#define CAD_SYSTEM_H

#include <stdio.h>
#include <time.h>

#include <ne_defs.h> /* for ne_off_t */

/* Everything the rest of cadaver needs to know about the operating
 * system underneath it lives here, so that the command implementations
 * stay free of #ifdef.  The Windows and POSIX definitions are both in
 * lib/system.c. */

/* The separator to build a local path with, as a string so that it can
 * be concatenated in place.  Windows accepts a forward slash in every
 * call cadaver makes, but printing one back in a path the user is meant
 * to recognise looks wrong. */
#ifdef _WIN32
#define CAD_DIR_SEPARATOR "\\"
#else
#define CAD_DIR_SEPARATOR "/"
#endif

/* Prepares the terminal and the process for use.  On Windows this puts
 * the console into UTF-8 for the lifetime of the process, restoring the
 * previous code pages at exit, and replaces argv with a UTF-8 copy
 * decoded from the wide command line so that non-ASCII arguments
 * survive.  Elsewhere it does nothing.  Called once from main() before
 * anything reads argv or writes output. */
void cad_system_init(int *argc, char ***argv);

/* The user's home directory: $HOME if set, else the Windows profile
 * directory.  NULL if neither is available.  cadaver keeps .cadaverrc,
 * .netrc and .cadaver-locks there. */
const char *cad_home_dir(void);

/* cad_home_dir() with `name' appended, using the platform's own path
 * separator so that what cadaver prints back is a path the user can
 * paste into a shell.  NULL if there is no home directory.  The result
 * is allocated with ne_malloc() and is the caller's to free. */
char *cad_home_path(const char *name);

/* The account name and the machine name, used to build the default
 * lock owner.  Either may be NULL. */
const char *cad_user_name(void);
const char *cad_host_name(void);

/* The name to print for the running program: argv[0] with any
 * directory and any .exe suffix removed.  The result is allocated with
 * ne_malloc(). */
char *cad_program_name(const char *argv0);

/* The directory for temporary files, without a trailing separator.
 * Never NULL: falls back to the current directory. */
const char *cad_tmp_dir(void);

/* The editor the `edit' command should run when the editor option is
 * unset: $VISUAL, then $EDITOR, then a platform default.  Never NULL. */
const char *cad_default_editor(void);

/* The pager the `less' command should run when the pager option is
 * unset: $PAGER, then a platform default.  Never NULL. */
const char *cad_default_pager(void);

/* The character encoding of the terminal, named the way iconv_open()
 * wants it -- "UTF-8", or "CP1252" and the like on a Windows console
 * that could not be switched.  Never NULL. */
const char *cad_codeset(void);

/* Runs `cmd' with its standard input connected to the returned stream,
 * as popen(cmd, "w") does.  Separate because Windows spells it
 * _popen(), and because the stream must be in binary mode: cadaver
 * writes a downloaded resource through it and CRLF translation would
 * corrupt anything that is not text. */
FILE *cad_popen_write(const char *cmd);
int cad_pclose(FILE *f);

/* Puts the descriptor into binary mode, so that no CRLF translation
 * happens on it, and returns the mode it was in for cad_set_mode() to
 * put back.  Returns -1 where the distinction does not exist, which
 * cad_set_mode() then ignores.  cadaver switches its own standard
 * output for the length of a `cat', so that a resource arrives on the
 * far end of a pipe byte for byte. */
int cad_set_binary(int fd);
void cad_set_mode(int fd, int mode);

/* What cadaver needs to know about a local file.  Separate from stat()
 * because a default Windows build gets a 32-bit st_size, which would
 * silently truncate the size of anything over 2 GiB and give `resumeget'
 * the wrong offset to resume from. */
struct cad_finfo {
    ne_off_t size;
    time_t mtime;
    int is_dir;
    int is_reg;
};

/* Fills in `info' for `path'.  Returns zero, or -1 with errno set. */
int cad_file_info(const char *path, struct cad_finfo *info);

/* Quotes `path' for the platform's command interpreter and appends it
 * to `cmd', returning a newly allocated command line.  Used to build
 * the editor invocation, where the temporary file's directory routinely
 * contains a space on Windows. */
char *cad_command_with_path(const char *cmd, const char *path);

#endif /* CAD_SYSTEM_H */
