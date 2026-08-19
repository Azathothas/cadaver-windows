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

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h> /* _mkdir */
#include <fcntl.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <ne_alloc.h>
#include <ne_string.h>

#include "system.h"

/* Platform-independent, but they belong with the rest of the
 * environment handling rather than in a file of their own. */

char *cad_home_path(const char *name)
{
    const char *home = cad_home_dir();

    if (home == NULL) return NULL;

    return ne_concat(home, CAD_DIR_SEPARATOR, name, NULL);
}

const char *cad_user_name(void)
{
    const char *u = getenv("USER");

#ifdef _WIN32
    if (u == NULL || !*u) u = getenv("USERNAME");
#else
    if (u == NULL || !*u) u = getenv("LOGNAME");
#endif

    return (u && *u) ? u : NULL;
}

const char *cad_host_name(void)
{
    const char *h = getenv("HOSTNAME");

#ifdef _WIN32
    if (h == NULL || !*h) h = getenv("COMPUTERNAME");
#endif

    return (h && *h) ? h : NULL;
}

char *cad_program_name(const char *argv0)
{
    const char *base = argv0, *p;
    size_t len;

    for (p = argv0; *p; p++) {
#ifdef _WIN32
        if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;
#else
        if (*p == '/') base = p + 1;
#endif
    }

    len = strlen(base);

#ifdef _WIN32
    /* argv[0] is the full path of the image, suffix and all, so
     * "Usage:" would otherwise print C:\...\cadaver.exe. */
    if (len > 4 && ne_strcasecmp(base + len - 4, ".exe") == 0) len -= 4;
#endif

    return len ? ne_strndup(base, len) : ne_strdup(argv0);
}

#ifdef _WIN32

/* The code pages in force when cadaver started, restored at exit so
 * that a console left behind by the process still shows the user's own
 * encoding.  Zero means "there was no console", which is the normal
 * case when output is redirected to a file. */
static UINT saved_input_cp, saved_output_cp;

static void restore_console(void)
{
    if (saved_output_cp) SetConsoleOutputCP(saved_output_cp);
    if (saved_input_cp) SetConsoleCP(saved_input_cp);
}

/* Replaces argv with a UTF-8 copy decoded from the wide command line.
 * The CRT hands main() an argv encoded in the active ANSI code page, in
 * which a path that the file system can represent may not be
 * representable at all; the wide command line always is.  Everything
 * inside cadaver treats a native string as being in the terminal's
 * encoding, which cad_codeset() reports as UTF-8 once the console has
 * been switched, so the two agree. */
static void utf8_argv(int *argc, char ***argv)
{
    LPWSTR *wargv;
    char **out;
    int n, count = 0;

    wargv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wargv == NULL || count <= 0) return;

    out = ne_malloc((count + 1) * sizeof *out);

    for (n = 0; n < count; n++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[n], -1,
                                      NULL, 0, NULL, NULL);

        if (len <= 0) {
            /* Nothing sensible to substitute, and dropping the argument
             * would silently shift the ones after it, so leave the
             * CRT's argv in place and carry on with that. */
            while (n-- > 0) ne_free(out[n]);
            LocalFree(wargv);
            ne_free(out);
            return;
        }

        out[n] = ne_malloc((size_t)len);
        WideCharToMultiByte(CP_UTF8, 0, wargv[n], -1, out[n], len, NULL, NULL);
    }

    out[count] = NULL;
    LocalFree(wargv);

    *argc = count;
    *argv = out;
}

void cad_system_init(int *argc, char ***argv)
{
    saved_output_cp = GetConsoleOutputCP();
    saved_input_cp = GetConsoleCP();

    if (saved_output_cp || saved_input_cp) {
        atexit(restore_console);
        if (saved_output_cp && saved_output_cp != CP_UTF8)
            SetConsoleOutputCP(CP_UTF8);
        if (saved_input_cp && saved_input_cp != CP_UTF8)
            SetConsoleCP(CP_UTF8);
    }

    utf8_argv(argc, argv);
}

const char *cad_home_dir(void)
{
    static char buf[MAX_PATH * 2];
    const char *home = getenv("HOME");
    const char *drive, *path;

    if (home && *home) return home;

    home = getenv("USERPROFILE");
    if (home && *home) return home;

    /* Domain-joined machines from before USERPROFILE existed, and the
     * odd cut-down environment, still set these two. */
    drive = getenv("HOMEDRIVE");
    path = getenv("HOMEPATH");
    if (drive && *drive && path && *path
        && strlen(drive) + strlen(path) < sizeof buf) {
        strcpy(buf, drive);
        strcat(buf, path);
        return buf;
    }

    return NULL;
}

const char *cad_tmp_dir(void)
{
    static char buf[MAX_PATH + 1];
    static int done;
    DWORD len;

    if (done) return buf;
    done = 1;

    len = GetTempPathA((DWORD)sizeof buf, buf);
    if (len == 0 || len > sizeof buf - 1) {
        strcpy(buf, ".");
        return buf;
    }

    /* GetTempPath always ends in a backslash; the callers add their own
     * separator. */
    while (len > 1 && (buf[len - 1] == '\\' || buf[len - 1] == '/'))
        buf[--len] = '\0';

    return buf;
}

const char *cad_default_editor(void)
{
    const char *e = getenv("VISUAL");

    if (e == NULL || !*e) e = getenv("EDITOR");
    if (e == NULL || !*e) e = "notepad";

    return e;
}

const char *cad_default_pager(void)
{
    const char *p = getenv("PAGER");

    /* more.com ships with Windows and reads its standard input, which
     * is how cadaver drives a pager. */
    if (p == NULL || !*p) p = "more";

    return p;
}

const char *cad_codeset(void)
{
    static char buf[16];
    UINT cp = GetConsoleOutputCP();

    /* No console: output is going to a file or a pipe, so there is no
     * code page to honour and the ANSI one is the best guess for what
     * the rest of the system will make of the bytes. */
    if (cp == 0) cp = GetACP();

    if (cp == CP_UTF8) return "UTF-8";

    ne_snprintf(buf, sizeof buf, "CP%u", (unsigned int)cp);
    return buf;
}

FILE *cad_popen_write(const char *cmd)
{
    return _popen(cmd, "wb");
}

int cad_pclose(FILE *f)
{
    return _pclose(f);
}

int cad_set_binary(int fd)
{
    return _setmode(fd, _O_BINARY);
}

void cad_set_mode(int fd, int mode)
{
    if (mode != -1) _setmode(fd, mode);
}

int cad_truncate(int fd, ne_off_t length)
{
    /* _chsize() takes a long, which is 32 bits here; the _s form takes
     * a 64-bit length and returns an errno value rather than setting
     * it. */
    int err = _chsize_s(fd, (__int64) length);

    if (err != 0) {
        errno = err;
        return -1;
    }

    return 0;
}

int cad_rename_over(const char *from, const char *to)
{
    /* The narrow entry point, to match the rest of the local file
     * layer: cadaver opens and stats local paths with the CRT's narrow
     * calls, so a name this one accepted and those did not would be
     * worse than one neither accepts. */
    if (!MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) {
        /* There is no general mapping from a Windows error to an errno
         * value; these are the two a rename actually hits. */
        errno = GetLastError() == ERROR_FILE_NOT_FOUND ? ENOENT : EACCES;
        return -1;
    }

    return 0;
}

int cad_file_info(const char *path, struct cad_finfo *info)
{
    struct _stati64 st;

    if (_stati64(path, &st) != 0) return -1;

    info->size = (ne_off_t) st.st_size;
    info->mtime = st.st_mtime;
    info->is_dir = (st.st_mode & _S_IFMT) == _S_IFDIR;
    info->is_reg = (st.st_mode & _S_IFMT) == _S_IFREG;

    return 0;
}

int cad_mkdir(const char *path)
{
    return _mkdir(path);
}

int cad_fd_info(int fd, struct cad_finfo *info)
{
    struct _stati64 st;

    if (_fstati64(fd, &st) != 0) return -1;

    info->size = (ne_off_t) st.st_size;
    info->mtime = st.st_mtime;
    info->is_dir = (st.st_mode & _S_IFMT) == _S_IFDIR;
    info->is_reg = (st.st_mode & _S_IFMT) == _S_IFREG;

    return 0;
}

/* cmd.exe takes a double-quoted argument literally apart from the
 * quotes themselves, and a Windows path cannot contain one, so quoting
 * the path is enough.  The command is left alone: it may legitimately
 * carry options, as "code --wait" does. */
char *cad_command_with_path(const char *cmd, const char *path)
{
    return ne_concat(cmd, " \"", path, "\"", NULL);
}

#else /* !_WIN32 */

void cad_system_init(int *argc, char ***argv)
{
    (void) argc;
    (void) argv;
}

const char *cad_home_dir(void)
{
    const char *home = getenv("HOME");

    return (home && *home) ? home : NULL;
}

const char *cad_tmp_dir(void)
{
    const char *tmp = getenv("TMPDIR");

    return (tmp && *tmp) ? tmp : "/tmp";
}

const char *cad_default_editor(void)
{
    const char *e = getenv("VISUAL");

    if (e == NULL || !*e) e = getenv("EDITOR");
    if (e == NULL || !*e) e = "vi";

    return e;
}

const char *cad_default_pager(void)
{
    const char *p = getenv("PAGER");

    if (p == NULL || !*p) p = "less";

    return p;
}

const char *cad_codeset(void)
{
#if defined(HAVE_NL_LANGINFO) && defined(HAVE_LANGINFO_H)
    const char *cs = nl_langinfo(CODESET);

    return (cs && *cs) ? cs : "UTF-8";
#else
    const char *tmp;

    if ((tmp = getenv("LC_ALL")) == NULL
        && (tmp = getenv("LC_CTYPE")) == NULL
        && (tmp = getenv("LANG")) == NULL)
        return "UTF-8";

    return strstr(tmp, "UTF-8") ? "UTF-8" : "ANSI_X3.4-1968";
#endif
}

FILE *cad_popen_write(const char *cmd)
{
    return popen(cmd, "w");
}

int cad_pclose(FILE *f)
{
    return pclose(f);
}

int cad_set_binary(int fd)
{
    (void) fd;
    return -1;
}

void cad_set_mode(int fd, int mode)
{
    (void) fd;
    (void) mode;
}

int cad_truncate(int fd, ne_off_t length)
{
    return ftruncate(fd, (off_t) length);
}

int cad_rename_over(const char *from, const char *to)
{
    /* rename(2) replaces the destination, which is what is wanted. */
    return rename(from, to);
}

int cad_file_info(const char *path, struct cad_finfo *info)
{
    struct stat st;

    if (stat(path, &st) != 0) return -1;

    info->size = (ne_off_t) st.st_size;
    info->mtime = st.st_mtime;
    info->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    info->is_reg = S_ISREG(st.st_mode) ? 1 : 0;

    return 0;
}

int cad_mkdir(const char *path)
{
    return mkdir(path, 0777);
}

int cad_fd_info(int fd, struct cad_finfo *info)
{
    struct stat st;

    if (fstat(fd, &st) != 0) return -1;

    info->size = (ne_off_t) st.st_size;
    info->mtime = st.st_mtime;
    info->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    info->is_reg = S_ISREG(st.st_mode) ? 1 : 0;

    return 0;
}

/* Single quotes so that a space or a shell metacharacter in the path is
 * passed through unchanged; an embedded single quote is closed, escaped
 * and reopened. */
char *cad_command_with_path(const char *cmd, const char *path)
{
    ne_buffer *buf = ne_buffer_create();
    const char *p;

    ne_buffer_zappend(buf, cmd);
    ne_buffer_zappend(buf, " '");
    for (p = path; *p; p++) {
        if (*p == '\'')
            ne_buffer_zappend(buf, "'\\''");
        else
            ne_buffer_append(buf, p, 1);
    }
    ne_buffer_zappend(buf, "'");

    return ne_buffer_finish(buf);
}

#endif /* _WIN32 */
