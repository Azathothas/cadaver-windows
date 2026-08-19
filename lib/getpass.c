/*
   Password prompt for cadaver
   Copyright (C) 1997-2001 Carl Harris and others (from fetchmail)
   Copyright (C) 2026 the cadaver-windows authors

   For license terms, see the file COPYING at the top of the tree.
*/

/* Reads a password without echoing it.  The terminal is a platform
 * matter: POSIX turns off ECHO with termios, Windows clears
 * ENABLE_ECHO_INPUT on the console handle.  Both leave line editing on,
 * so backspace still works, and both restore what they found -- on the
 * way out and from the SIGINT handler, because a terminal left with
 * echo off is unusable after cadaver has gone.
 *
 * When the input is not a terminal the password is read as an ordinary
 * line, which is what lets a session be scripted.
 */

#include "config.h"

#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#if defined(HAVE_TERMIOS_H) && defined(HAVE_TCSETATTR)
#include <termios.h>
#define USE_TERMIOS
#endif

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

#include "i18n.h"
#include "getpass.h"

#define INPUT_BUF_SIZE PASSWORDLEN

/* The stream the password is read from, and whether echo was turned off
 * on it, so that the SIGINT handler can put it back. */
static FILE *input;
static int echo_disabled;

#ifdef _WIN32

static HANDLE console;
static DWORD saved_mode;

static int disable_echo(void)
{
    intptr_t handle = _get_osfhandle(fileno(input));
    DWORD mode;

    console = (HANDLE) handle;
    if (console == INVALID_HANDLE_VALUE
        || !GetConsoleMode(console, &mode))
        return 0;

    saved_mode = mode;
    if (!SetConsoleMode(console, mode & ~(DWORD) ENABLE_ECHO_INPUT))
        return 0;

    return 1;
}

static void restore_echo(void)
{
    if (echo_disabled) {
        SetConsoleMode(console, saved_mode);
        echo_disabled = 0;
    }
}

#elif defined(USE_TERMIOS)

static struct termios saved_term;

static int disable_echo(void)
{
    struct termios term;

    if (tcgetattr(fileno(input), &term) != 0) return 0;

    saved_term = term;
    term.c_lflag &= ~(tcflag_t) ECHO;

    if (tcsetattr(fileno(input), TCSAFLUSH, &term) != 0) return 0;

    return 1;
}

static void restore_echo(void)
{
    if (echo_disabled) {
        tcsetattr(fileno(input), TCSAFLUSH, &saved_term);
        echo_disabled = 0;
    }
}

#else

/* No way to turn echo off: the password would be typed in the clear,
 * which is worse than not offering the prompt at all. */
static int disable_echo(void)
{
    return 0;
}

static void restore_echo(void)
{
}

#endif

static void sigint_handler(int signum)
{
    (void) signum;
    restore_echo();
    fputs(_("\nCaught SIGINT... bailing out.\n"), stderr);
    exit(1);
}

char *fm_getpassword(const char *prompt)
{
    static char pbuf[INPUT_BUF_SIZE];
    void (*sig)(int) = SIG_DFL;
    int istty, c;
    char *p, *ret;
    FILE *tty = NULL;

    istty = isatty(fileno(stdin));

#ifndef _WIN32
    /* Reading from the terminal rather than from standard input means
     * the prompt still works when cadaver's own input is a script. */
    if (istty) {
        int fd = open("/dev/tty", O_RDWR);

        if (fd >= 0) {
            tty = fdopen(fd, "r");
            if (tty == NULL)
                close(fd);
            else
                setbuf(tty, NULL);
        }
    }
#endif

    input = tty ? tty : stdin;

    if (istty) {
        echo_disabled = disable_echo();
        sig = signal(SIGINT, sigint_handler);
        fputs(prompt, stderr);
        fflush(stderr);
    }

    for (p = pbuf; (c = getc(input)) != '\n' && c != EOF;) {
        if (c == '\r') continue; /* a console line ends CR LF */
        if (p < &pbuf[INPUT_BUF_SIZE - 1]) *p++ = (char) c;
    }
    *p = '\0';

    ret = (c == EOF && p == pbuf) ? NULL : pbuf;

    if (istty) {
        /* The newline the user typed was not echoed, so the cursor is
         * still sitting after the prompt. */
        fputc('\n', stderr);
        restore_echo();
        signal(SIGINT, sig);
    }

    if (tty) fclose(tty);
    input = NULL;

    return ret;
}
