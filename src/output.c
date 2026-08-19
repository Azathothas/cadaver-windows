/*
   cadaver, command-line DAV client -- output routing and results
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
#include <stdarg.h>

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <time.h>

#include <ne_alloc.h>
#include <ne_string.h>
#include <ne_basic.h>  /* for NE_DEPTH_INFINITE */
#include <ne_redirect.h>
#include <ne_uri.h>

#include "i18n.h"
#include "cadaver.h"
#include "commands.h"
#include "output.h"
#include "utils.h"

int out_json;

/* C99, and every compiler this builds with has it; the fallback is for
 * one that does not, where a va_list is a plain object and copying it
 * is the assignment below. */
#ifndef va_copy
#ifdef __va_copy
#define va_copy(dst, src) __va_copy(dst, src)
#else
#define va_copy(dst, src) ((dst) = (src))
#endif
#endif

/* Where the trace was aimed, so that --json can refuse to share
 * standard output with it.  Set by out_trace_claims_stdout(). */
static int trace_on_stdout;

/* --- The run and its commands -------------------------------------
 *
 * The counters are kept in every mode, because the exit status depends
 * on them.  The records below them are built only with --json: an
 * interactive session has no use for them and no reason to grow. */

static int run_failures;      /* commands that failed */
static int run_commands;      /* commands executed */

/* One operation: one out_start()...out_result() pair. */
struct operation {
    char *method, *path;      /* the last request, or NULL for none */
    int status;               /* its response status, 0 if none arrived */
    int failed;
    char *context;            /* why, when it failed */
    double duration;
    struct operation *next;
};

/* One command line that was executed. */
struct command_rec {
    char *name;
    char **args;
    int nargs;
    double started, duration;
    struct operation *ops, **op_tail;
    int nops, nfailed;
    char *context;            /* a failure outside any operation */
    ne_buffer *output;        /* what it printed */
    /* Structured detail, each a comma-separated run of JSON values. */
    ne_buffer *listing, *properties, *headers, *locks, *options;
    ne_buffer *benchmark;     /* one object, not a run of values */
    char *path;               /* pwd and lpwd */
    int http_status;          /* head */
    struct command_rec *next;
};

static struct command_rec *cmd_list, **cmd_tail = &cmd_list;
static int cur_number;                  /* how many commands have run */
static char cur_label[512];             /* the label the trace uses */
static struct command_rec *cur;         /* the command in progress */
static struct operation *cur_op;        /* the operation in progress */

static char *run_target;
static char run_started[40];
static double run_start_time;
static ne_buffer *run_output;   /* what was printed outside any command */

/* The last request made since the current operation began.  Filled in
 * by the hooks on the neon session; see req_started(). */
static char *req_method, *req_path;
static int req_code, req_made;

/* --- Time ----------------------------------------------------------
 *
 * Durations are wall clock, so they include server and network time.
 * There is nothing else worth reporting for a command whose work is a
 * request.  The two readings themselves are in src/utils.c, because
 * `bench' needs the same clock. */

#define now_seconds() cad_now_seconds()
#define now_iso8601(buf, len) cad_now_iso8601((buf), (len))

/* --- Where output goes --------------------------------------------
 *
 * With --json the text goes into the command that produced it, or into
 * the run itself for anything printed between commands -- the
 * connection banner, the closing message.  Nothing is dropped. */

static ne_buffer *capture(void)
{
    if (cur) return cur->output;
    if (!run_output) run_output = ne_buffer_create();
    return run_output;
}

/* Formats into a buffer grown to fit, which the caller frees with
 * ne_free().  Nothing cadaver prints has a length it decides: a
 * property value is as long as the server made it, so a fixed buffer
 * here would truncate under --json what the same message prints in
 * full without it.  Returns NULL only if the format could not be
 * rendered at all. */
static char *format_alloc(const char *fmt, va_list ap)
{
    va_list measure;
    char *buf;
    int len;

    va_copy(measure, ap);
    len = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);

    if (len < 0) return NULL;

    buf = ne_malloc((size_t)len + 1);
    vsnprintf(buf, (size_t)len + 1, fmt, ap);

    return buf;
}

void out_vprintf(const char *fmt, va_list ap)
{
    if (out_json) {
        char *buf = format_alloc(fmt, ap);

        if (buf) {
            ne_buffer_zappend(capture(), buf);
            ne_free(buf);
        }
    }
    else {
        vfprintf(stdout, fmt, ap);
    }
}

void out_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    out_vprintf(fmt, ap);
    va_end(ap);
}

void out_putchar(int c)
{
    if (out_json) {
        char buf[2];
        buf[0] = (char)c;
        buf[1] = '\0';
        ne_buffer_zappend(capture(), buf);
    }
    else {
        putchar(c);
    }
}

void out_puts(const char *str)
{
    if (out_json) {
        ne_buffer_zappend(capture(), str);
    }
    else {
        fputs(str, stdout);
    }
}

void out_puts_line(const char *str)
{
    out_puts(str);
    out_putchar('\n');
}

void out_flush(void)
{
    if (!out_json) fflush(stdout);
}

/* The message both directions of the conflict print. */
static void say_conflict(void)
{
    fprintf(stderr, _("cadaver: --json and --trace=- both want standard "
                      "output; send the trace somewhere else.\n"));
}

int out_trace_claims_stdout(void)
{
    if (out_json) {
        say_conflict();
        return 1;
    }

    trace_on_stdout = 1;
    return 0;
}

int out_set_json(void)
{
    if (trace_on_stdout) {
        say_conflict();
        return 1;
    }

    out_json = 1;
    return 0;
}

/* --- Recording ---------------------------------------------------- */

static void forget_request(void)
{
    if (req_method) ne_free(req_method);
    if (req_path) ne_free(req_path);
    req_method = req_path = NULL;
    req_code = 0;
    req_made = 0;
}

void req_started(const char *method, const char *target)
{
    restype_note_request(method, target);
    forget_request();
    req_method = ne_strdup(method ? method : "");
    req_path = ne_strdup(target ? target : "");
    req_made = 1;
}

void req_status(int status)
{
    req_code = status;
}

int req_last_status(void)
{
    return req_made ? req_code : 0;
}

void run_begin(const char *target)
{
    run_start_time = now_seconds();
    now_iso8601(run_started, sizeof run_started);
    if (target) run_target = ne_strdup(target);
}

/* The number of operations and failures of the command in progress,
 * which have to be tracked outside the record because the record only
 * exists with --json. */
static int cur_nops, cur_nfailed;

/* Closes cur_label with the bracket cmd_trace_label() promises,
 * truncating the text if there is no room for it. */
static void label_close(void)
{
    size_t len = strlen(cur_label);

    if (len + 2 >= sizeof cur_label) len = sizeof cur_label - 2;
    cur_label[len] = ')';
    cur_label[len + 1] = '\0';
}

void cmd_begin(const char *line)
{
    /* The number is claimed here rather than in cmd_named(), so that
     * the requests a wildcard expansion makes carry it too, tagged
     * with the line as typed until there is something better.
     * cmd_discard() gives it back for a line that held no command. */
    cur_number++;
    ne_snprintf(cur_label, sizeof cur_label, "%d (%s", cur_number,
                line ? line : "");
    label_close();

    if (!out_json) return;

    cur = ne_calloc(sizeof *cur);
    cur->started = now_seconds();
    cur->op_tail = &cur->ops;
    cur->output = ne_buffer_create();
    cur->http_status = -1;

    *cmd_tail = cur;
    cmd_tail = &cur->next;
}

void cmd_named(const char *name, int argc, const char **argv)
{
    size_t len;
    int n;

    run_commands++;

    /* The label the trace tags this command's requests with.  It is
     * built in every mode, because --trace does not need --json. */
    ne_snprintf(cur_label, sizeof cur_label, "%d (%s", cur_number, name);
    for (n = 0; n < argc && argv[n]; n++) {
        len = strlen(cur_label);
        if (len + 2 >= sizeof cur_label) break;
        ne_snprintf(cur_label + len, sizeof cur_label - len, " %s", argv[n]);
    }
    label_close();

    if (!cur) return;

    cur->name = ne_strdup(name);
    cur->nargs = argc;
    if (argc > 0) {
        cur->args = ne_calloc(argc * sizeof *cur->args);
        for (n = 0; n < argc; n++)
            cur->args[n] = ne_strdup(argv[n] ? argv[n] : "");
    }
}

void cmd_discard(void)
{
    struct command_rec *c;

    /* The line held no command, so it gets no number. */
    cur_number--;
    cur_label[0] = '\0';

    if (!cur) return;

    /* Unlink the record: nothing ran, so nothing is reported. */
    if (cmd_list == cur) {
        cmd_list = NULL;
        cmd_tail = &cmd_list;
    }
    else {
        for (c = cmd_list; c && c->next != cur; c = c->next)
            /* nothing */;
        if (c) {
            c->next = NULL;
            cmd_tail = &c->next;
        }
    }

    ne_buffer_destroy(cur->output);
    ne_free(cur);
    cur = NULL;
    cur_op = NULL;
    cur_nops = cur_nfailed = 0;
}

const char *cmd_trace_label(void)
{
    return cur_label[0] ? cur_label : "no command";
}

void cmd_end(void)
{
    if (cur_nfailed) run_failures++;

    if (cur) {
        cur->duration = now_seconds() - cur->started;
        cur->nops = cur_nops;
        cur->nfailed = cur_nfailed;
        cur = NULL;
    }

    cur_op = NULL;
    cur_nops = cur_nfailed = 0;
    forget_request();
    restype_forget_all();
}

void cmd_failed(const char *context)
{
    cur_nfailed++;
    if (cur && context && !cur->context)
        cur->context = ne_strdup(context);
}

/* Starts an operation.  The request slot is cleared, so that a command
 * which resolves a path with a PROPFIND and then refuses to go on does
 * not report that PROPFIND as the request its failure was about. */
static void op_begin(void)
{
    forget_request();
    cur_nops++;

    if (!cur) return;

    cur_op = ne_calloc(sizeof *cur_op);
    cur_op->duration = now_seconds();      /* start, replaced in op_end */
    *cur->op_tail = cur_op;
    cur->op_tail = &cur_op->next;
}

/* Ends an operation.  `context' is the reason it failed, or NULL. */
static void op_end(const char *context)
{
    trace_body_restore();

    if (context) cur_nfailed++;

    if (!cur_op) {
        forget_request();
        return;
    }

    cur_op->duration = now_seconds() - cur_op->duration;
    cur_op->failed = context != NULL;
    if (context) cur_op->context = ne_strdup(context);

    if (req_made) {
        cur_op->method = ne_strdup(req_method ? req_method : "");
        cur_op->path = ne_strdup(req_path ? req_path : "");
        cur_op->status = req_code;
    }

    cur_op = NULL;
    forget_request();
}

/* --- Command feedback --------------------------------------------- */

void out_start(const char *verb, const char *noun)
{
    op_begin();
    output(o_start, "%s `%s':", verb, noun);
}

void out_start_uri(const char *verb, const char *path)
{
    char *native_path = native_path_from_uri(path);
    op_begin();
    output(o_start, "%s `%s':", verb, native_path);
    ne_free(native_path);
}

void out_start_2uri(const char *verb, const char *path1, const char *path2)
{
    char *native1 = native_path_from_uri(path1);
    char *native2 = native_path_from_uri(path2);
    op_begin();
    output(o_start, _("%s `%s' to `%s':"), verb, native1, native2);
    ne_free(native1);
    ne_free(native2);
}

void out_start_raw(const char *fmt, ...)
{
    va_list ap;
    char *buf;

    va_start(ap, fmt);
    buf = format_alloc(fmt, ap);
    va_end(ap);

    op_begin();
    output(o_start, "%s", buf ? buf : "");
    if (buf) ne_free(buf);
}

void out_start_transfer(enum output_type dir, const char *fmt, ...)
{
    va_list ap;
    char *buf;

    va_start(ap, fmt);
    buf = format_alloc(fmt, ap);
    va_end(ap);

    op_begin();
    /* The body of this one is the resource, not a protocol document. */
    trace_body_suppress();
    output(dir, "%s", buf ? buf : "");
    if (buf) ne_free(buf);
}

void out_success(void)
{
    out_transfer_report();
    output(o_finish, _("succeeded.\n"));
    op_end(NULL);
}

void out_success_as(const char *text)
{
    out_transfer_report();
    output(o_finish, "%s", text);
    op_end(NULL);
}

void out_done(void)
{
    op_end(NULL);
    out_state_reset();
}

void out_failed(const char *context)
{
    op_end(context ? context : "");
    out_state_reset();
}

void out_result(int ret)
{
    switch (ret) {
    case NE_OK:
        out_success();
        break;
    case NE_AUTH:
    case NE_PROXYAUTH:
        output(o_finish, _("authentication failed.\n"));
        op_end(_("authentication failed"));
        break;
    case NE_CONNECT:
        output(o_finish, _("could not connect to server.\n"));
        op_end(_("could not connect to server"));
        break;
    case NE_TIMEOUT:
        output(o_finish, _("connection timed out.\n"));
        op_end(_("connection timed out"));
        break;
    default:
        if (ret == NE_REDIRECT) {
            const ne_uri *dest = ne_redirect_location(session.sess);
            if (dest) {
                char *uri = ne_uri_unparse(dest);
                char *context = ne_concat(_("redirect to "), uri, NULL);
                output(o_finish, _("redirect to %s\n"), uri);
                op_end(context);
                ne_free(context);
                ne_free(uri);
                break;
            }
        }
        output(o_finish, _("failed:\n%s\n"), ne_get_error(session.sess));
        op_end(ne_get_error(session.sess));
        break;
    }
}

int out_handle(int ret)
{
    out_result(ret);
    return (ret == NE_OK);
}

void out_fail(const char *fmt, ...)
{
    va_list ap;
    char *buf;

    va_start(ap, fmt);
    buf = format_alloc(fmt, ap);
    va_end(ap);

    output(o_finish, _("failed:\n%s"), buf ? buf : "");
    /* ne_shave() trims in place, which is why the message is printed
     * first: the reason is the same text without its line ending. */
    op_end(buf ? ne_shave(buf, " \r\n") : "");
    if (buf) ne_free(buf);
}

/* --- Structured detail --------------------------------------------
 *
 * Each of these appends one JSON value to a comma-separated run, so
 * that output.c does not need its own copy of the data structures the
 * commands already have. */

static void json_string(ne_buffer *buf, const char *str);

static void detail_sep(ne_buffer **buf)
{
    if (*buf == NULL)
        *buf = ne_buffer_create();
    else
        ne_buffer_czappend(*buf, ",");
}

void res_listing(const char *uri_path, enum resource_type type,
                 dav_size_t size, time_t modtime, int is_executable,
                 int error_status, const char *error_reason)
{
    char num[64];
    char *name, *sep;
    ne_buffer *b;

    if (!cur) return;

    detail_sep(&cur->listing);
    b = cur->listing;

    /* The last non-empty segment of the path, which is the name a
     * person sees in the listing, in native form. */
    name = native_path_from_uri(uri_path);
    sep = name + strlen(name);
    while (sep > name && sep[-1] == '/') *--sep = '\0';
    sep = strrchr(name, '/');

    ne_buffer_czappend(b, "{\"name\":");
    json_string(b, sep ? sep + 1 : name);
    ne_buffer_czappend(b, ",\"href\":");
    json_string(b, uri_path);
    ne_buffer_czappend(b, ",\"type\":");
    json_string(b, type == resr_collection ? "collection" :
                   type == resr_reference ? "reference" :
                   type == resr_error ? "error" : "resource");

    if (type == resr_error) {
        ne_snprintf(num, sizeof num, ",\"status\":%d,\"reason\":",
                    error_status);
        ne_buffer_zappend(b, num);
        json_string(b, error_reason ? error_reason : "");
    }
    else {
        char stamp[40];

        ne_snprintf(num, sizeof num, ",\"size\":%" FMT_DAV_SIZE_T "u", size);
        ne_buffer_zappend(b, num);
        ne_buffer_czappend(b, ",\"modified\":");
        if (modtime == (time_t)-1 || !iso8601_utc(modtime, stamp, sizeof stamp))
            ne_buffer_czappend(b, "null");
        else
            json_string(b, stamp);
        /* Not ne_buffer_czappend(): that takes sizeof of what it is
         * given, which for anything but a literal is the size of a
         * pointer. */
        ne_buffer_zappend(b, is_executable ? ",\"executable\":true"
                                           : ",\"executable\":false");
    }

    ne_buffer_czappend(b, "}");
    ne_free(name);
}

void res_property(const char *nspace, const char *name, const char *value,
                  int status)
{
    ne_buffer *b;
    char num[32];

    if (!cur) return;

    detail_sep(&cur->properties);
    b = cur->properties;

    ne_buffer_czappend(b, "{\"namespace\":");
    json_string(b, nspace ? nspace : "");
    ne_buffer_czappend(b, ",\"name\":");
    json_string(b, name ? name : "");
    ne_buffer_czappend(b, ",\"value\":");
    if (value) json_string(b, value);
    else ne_buffer_czappend(b, "null");
    if (status) {
        ne_snprintf(num, sizeof num, ",\"status\":%d", status);
        ne_buffer_zappend(b, num);
    }
    ne_buffer_czappend(b, "}");
}

void res_http_status(int status)
{
    if (cur) cur->http_status = status;
}

void res_header(const char *name, const char *value)
{
    ne_buffer *b;

    if (!cur) return;

    detail_sep(&cur->headers);
    b = cur->headers;

    json_string(b, name ? name : "");
    ne_buffer_czappend(b, ":");
    json_string(b, value ? value : "");
}

void res_lock(const struct ne_lock *lock)
{
    ne_buffer *b;
    char num[64], *uri;

    if (!cur) return;

    detail_sep(&cur->locks);
    b = cur->locks;

    uri = ne_uri_unparse(&lock->uri);

    ne_buffer_czappend(b, "{\"token\":");
    json_string(b, lock->token ? lock->token : "");
    ne_buffer_czappend(b, ",\"href\":");
    json_string(b, uri);
    ne_buffer_czappend(b, ",\"scope\":");
    json_string(b, lock->scope == ne_lockscope_exclusive ? "exclusive" :
                   lock->scope == ne_lockscope_shared ? "shared" : "unknown");
    ne_buffer_czappend(b, ",\"depth\":");
    if (lock->depth == NE_DEPTH_INFINITE) {
        ne_buffer_czappend(b, "\"infinity\"");
    }
    else {
        ne_snprintf(num, sizeof num, "%d", lock->depth);
        ne_buffer_zappend(b, num);
    }
    ne_buffer_czappend(b, ",\"timeout\":");
    /* An infinite lock is named the way an infinite depth is; anything
     * else negative is NE_TIMEOUT_INVALID, meaning the server said
     * nothing usable.  Zero is a duration a server actually named. */
    if (lock->timeout == NE_TIMEOUT_INFINITE) {
        ne_buffer_czappend(b, "\"infinity\"");
    }
    else if (lock->timeout < 0) {
        ne_buffer_czappend(b, "null");
    }
    else {
        ne_snprintf(num, sizeof num, "%ld", lock->timeout);
        ne_buffer_zappend(b, num);
    }
    ne_buffer_czappend(b, ",\"owner\":");
    if (lock->owner) json_string(b, lock->owner);
    else ne_buffer_czappend(b, "null");
    ne_buffer_czappend(b, "}");

    ne_free(uri);
}

void res_path(const char *path)
{
    if (cur && path && !cur->path) cur->path = ne_strdup(path);
}

/* Seconds to millisecond resolution, the way put_duration() writes
 * every other duration in the document. */
static void detail_seconds(ne_buffer *b, const char *key, double seconds)
{
    char num[64];

    if (seconds < 0) seconds = 0;
    ne_snprintf(num, sizeof num, ",\"%s\":%.3f", key, seconds);
    ne_buffer_zappend(b, num);
}

static void detail_rate(ne_buffer *b, ne_off_t bytes, double seconds)
{
    char num[64];
    double rate = seconds > 0.0
        ? (double)bytes / (1024.0 * 1024.0) / seconds : 0.0;

    ne_snprintf(num, sizeof num, ",\"mib_per_second\":%.2f", rate);
    ne_buffer_zappend(b, num);
}

void res_benchmark(const struct bench_result *result)
{
    ne_buffer *b;
    char num[128];

    if (!cur || cur->benchmark) return;

    b = cur->benchmark = ne_buffer_create();

    ne_buffer_czappend(b, "{\"target\":");
    json_string(b, result->target ? result->target : "");
    ne_buffer_czappend(b, ",\"started\":");
    if (result->started && result->started[0]) json_string(b, result->started);
    else ne_buffer_czappend(b, "null");

    ne_snprintf(num, sizeof num,
                ",\"iterations\":%d,\"payload_bytes\":%" NE_FMT_NE_OFF_T,
                result->iterations, result->payload_bytes);
    ne_buffer_zappend(b, num);

    ne_buffer_czappend(b, ",\"latency\":{\"op\":");
    json_string(b, result->latency_op ? result->latency_op : "");
    ne_snprintf(num, sizeof num, ",\"samples\":%d", result->latency_samples);
    ne_buffer_zappend(b, num);
    /* Milliseconds here rather than seconds: a round trip is the one
     * measurement in this document that is routinely under a
     * millisecond, and %.3f seconds would report it as zero. */
    ne_snprintf(num, sizeof num,
                ",\"min_ms\":%.3f,\"median_ms\":%.3f,\"max_ms\":%.3f}",
                result->latency_min * 1000.0, result->latency_median * 1000.0,
                result->latency_max * 1000.0);
    ne_buffer_zappend(b, num);

    ne_snprintf(num, sizeof num, ",\"upload\":{\"bytes\":%" NE_FMT_NE_OFF_T,
                result->upload_bytes);
    ne_buffer_zappend(b, num);
    detail_seconds(b, "seconds", result->upload_seconds);
    detail_rate(b, result->upload_bytes, result->upload_seconds);
    ne_buffer_czappend(b, "}");

    ne_snprintf(num, sizeof num, ",\"download\":{\"bytes\":%" NE_FMT_NE_OFF_T,
                result->download_bytes);
    ne_buffer_zappend(b, num);
    detail_seconds(b, "seconds", result->download_seconds);
    detail_rate(b, result->download_bytes, result->download_seconds);
    ne_buffer_czappend(b, "}");

    ne_buffer_czappend(b, "}");
}

void res_option(const char *name, const char *value)
{
    ne_buffer *b;

    if (!cur) return;

    detail_sep(&cur->options);
    b = cur->options;

    json_string(b, name ? name : "");
    ne_buffer_czappend(b, ":");
    if (value) json_string(b, value);
    else ne_buffer_czappend(b, "null");
}

/* --- Writing the document ----------------------------------------- */

/* The length of the well-formed UTF-8 sequence beginning at `s', or 0
 * if there is not one there.  Overlong forms, surrogates and anything
 * above U+10FFFF are rejected, because a consumer's parser will reject
 * them too. */
static size_t utf8_length(const unsigned char *s)
{
    unsigned long cp;
    size_t len, n;

    if (s[0] < 0x80) return 1;
    else if ((s[0] & 0xE0) == 0xC0) { len = 2; cp = s[0] & 0x1FU; }
    else if ((s[0] & 0xF0) == 0xE0) { len = 3; cp = s[0] & 0x0FU; }
    else if ((s[0] & 0xF8) == 0xF0) { len = 4; cp = s[0] & 0x07U; }
    else return 0;

    for (n = 1; n < len; n++) {
        if ((s[n] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (unsigned long)(s[n] & 0x3FU);
    }

    if (cp < (len == 2 ? 0x80UL : len == 3 ? 0x800UL : 0x10000UL))
        return 0;                       /* overlong */
    if (cp > 0x10FFFFUL) return 0;
    if (cp >= 0xD800UL && cp <= 0xDFFFUL) return 0;   /* surrogate */

    return len;
}

/* Appends `str' to `buf' as a quoted JSON string. */
static void json_string(ne_buffer *buf, const char *str)
{
    const unsigned char *s = (const unsigned char *)str;
    char esc[8];

    ne_buffer_czappend(buf, "\"");
    while (s && *s) {
        unsigned char c = *s;

        switch (c) {
        case '"': ne_buffer_czappend(buf, "\\\""); s++; continue;
        case '\\': ne_buffer_czappend(buf, "\\\\"); s++; continue;
        case '\b': ne_buffer_czappend(buf, "\\b"); s++; continue;
        case '\f': ne_buffer_czappend(buf, "\\f"); s++; continue;
        case '\n': ne_buffer_czappend(buf, "\\n"); s++; continue;
        case '\r': ne_buffer_czappend(buf, "\\r"); s++; continue;
        case '\t': ne_buffer_czappend(buf, "\\t"); s++; continue;
        default: break;
        }

        if (c < 0x20) {
            ne_snprintf(esc, sizeof esc, "\\u%04x", c);
            ne_buffer_zappend(buf, esc);
            s++;
        }
        else {
            size_t len = utf8_length(s);

            if (len == 0) {
                /* Not UTF-8, so it cannot go in a JSON document as
                 * itself.  This happens where the terminal's encoding
                 * is not UTF-8 and a name or a property value was
                 * converted into it; the document stays parseable and
                 * the character is marked as lost. */
                ne_buffer_czappend(buf, "\\ufffd");
                s++;
            }
            else {
                /* Valid UTF-8 goes through byte for byte. */
                ne_buffer_append(buf, (const char *)s, len);
                s += len;
            }
        }
    }
    ne_buffer_czappend(buf, "\"");
}

static void put_string(FILE *fp, const char *str)
{
    ne_buffer *buf = ne_buffer_create();
    json_string(buf, str);
    fputs(buf->data, fp);
    ne_buffer_destroy(buf);
}

/* Seconds to millisecond resolution, which is all a wall-clock
 * measurement of a network round trip can honestly claim. */
static void put_duration(FILE *fp, double seconds)
{
    if (seconds < 0) seconds = 0;
    fprintf(fp, "%.3f", seconds);
}

/* Writes the captured output as an array of lines.  The trailing
 * newline of the last line does not make an extra empty one. */
static void put_output(FILE *fp, const char *text)
{
    const char *p = text;
    int first = 1;

    fputs("[", fp);
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line;

        if (len > 0 && p[len - 1] == '\r') len--;

        line = ne_strndup(p, len);
        if (!first) fputs(",", fp);
        put_string(fp, line);
        ne_free(line);
        first = 0;

        if (!eol) break;
        p = eol + 1;
    }
    fputs("]", fp);
}

static void put_error(FILE *fp, const struct operation *op)
{
    fputs("\"error\":{\"op\":", fp);
    put_string(fp, op->method);
    fputs(",\"path\":", fp);
    put_string(fp, op->path);
    if (op->status)
        fprintf(fp, ",\"status\":%d}", op->status);
    else
        fputs(",\"status\":null}", fp);
}

static void put_operation(FILE *fp, const struct operation *op)
{
    fputs("{\"target\":", fp);
    if (op->method) put_string(fp, op->path);
    else fputs("null", fp);
    fputs(",\"status\":", fp);
    fputs(op->failed ? "\"failed\"" : "\"ok\"", fp);
    fputs(",\"duration\":", fp);
    put_duration(fp, op->duration);
    if (op->context) {
        fputs(",\"context\":", fp);
        put_string(fp, op->context);
    }
    if (op->failed && op->method) {
        fputs(",", fp);
        put_error(fp, op);
    }
    fputs("}", fp);
}

/* The operation a command's own target, context and error describe:
 * the first one that failed, or the last one if none did. */
static const struct operation *summary_op(const struct command_rec *c)
{
    const struct operation *op, *last = NULL;

    for (op = c->ops; op; op = op->next) {
        if (op->failed) return op;
        last = op;
    }
    return last;
}

static void put_detail(FILE *fp, const char *key, ne_buffer *buf,
                       const char *open, const char *close)
{
    if (!buf) return;
    fprintf(fp, ",\"%s\":%s%s%s", key, open, buf->data, close);
}

static void put_command(FILE *fp, const struct command_rec *c)
{
    const struct operation *op = summary_op(c);
    int n;

    fputs("{\"command\":", fp);
    put_string(fp, c->name);

    fputs(",\"args\":[", fp);
    for (n = 0; n < c->nargs; n++) {
        if (n) fputs(",", fp);
        put_string(fp, c->args[n]);
    }
    fputs("]", fp);

    fputs(",\"target\":", fp);
    if (op && op->method) put_string(fp, op->path);
    else fputs("null", fp);

    fputs(",\"status\":", fp);
    fputs(c->nfailed ? "\"failed\"" : "\"ok\"", fp);

    fputs(",\"duration\":", fp);
    put_duration(fp, c->duration);

    if (c->context) {
        fputs(",\"context\":", fp);
        put_string(fp, c->context);
    }
    else if (op && op->context) {
        fputs(",\"context\":", fp);
        put_string(fp, op->context);
    }

    if (c->nfailed && op && op->failed && op->method) {
        fputs(",", fp);
        put_error(fp, op);
    }

    if (c->nops > 1) {
        const struct operation *o;
        fputs(",\"operations\":[", fp);
        for (o = c->ops, n = 0; o; o = o->next, n++) {
            if (n) fputs(",", fp);
            put_operation(fp, o);
        }
        fputs("]", fp);
    }

    put_detail(fp, "listing", c->listing, "[", "]");
    put_detail(fp, "properties", c->properties, "[", "]");
    put_detail(fp, "headers", c->headers, "{", "}");
    put_detail(fp, "locks", c->locks, "[", "]");
    put_detail(fp, "options", c->options, "{", "}");
    /* Already an object, so it is written as it stands rather
     * than wrapped the way the comma-separated runs above are. */
    if (c->benchmark)
        fprintf(fp, ",\"benchmark\":%s", c->benchmark->data);

    if (c->path) {
        fputs(",\"path\":", fp);
        put_string(fp, c->path);
    }
    if (c->http_status >= 0)
        fprintf(fp, ",\"http_status\":%d", c->http_status);

    fputs(",\"output\":", fp);
    put_output(fp, c->output->data);

    fputs("}", fp);
}

static void write_json(FILE *fp)
{
    const struct command_rec *c;
    int n, ok = 0;

    for (c = cmd_list; c; c = c->next)
        if (!c->nfailed) ok++;

    fputs("{\"tool\":\"cadaver\",\"version\":", fp);
    put_string(fp, PACKAGE_VERSION);

    fputs(",\"target\":", fp);
    if (run_target) put_string(fp, run_target);
    else fputs("null", fp);

    fputs(",\"started\":", fp);
    if (run_started[0]) put_string(fp, run_started);
    else fputs("null", fp);

    fputs(",\"duration\":", fp);
    put_duration(fp, now_seconds() - run_start_time);

    fputs(",\"commands\":[", fp);
    for (c = cmd_list, n = 0; c; c = c->next, n++) {
        if (n) fputs(",", fp);
        put_command(fp, c);
    }
    fputs("]", fp);

    fprintf(fp, ",\"summary\":{\"total\":%d,\"ok\":%d,\"failed\":%d}}\n",
            run_commands, ok, run_failures);

    fflush(fp);
}

int run_finish(void)
{
    if (out_json) write_json(stdout);

    return run_failures > CAD_EXIT_MAX ? CAD_EXIT_MAX : run_failures;
}
