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

#ifndef CAD_OUTPUT_H
#define CAD_OUTPUT_H

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <ne_locks.h>
#include <ne_session.h>

#include "cadaver.h" /* for enum output_type, dav_size_t */

/* Everything cadaver prints for a person goes through this file, and
 * so does everything it records about how a command went.  The two
 * belong together because --json turns the first into the second: with
 * --json, standard output carries one JSON object and nothing else, so
 * the text a command would have printed is captured and appears in that
 * command's "output" array instead.
 *
 * Nothing in src/ may call printf(), putchar() or puts() directly.
 * tests/offline.sh checks the source for it, because one stray write to
 * standard output would corrupt the JSON document. */

/* --- Where output goes ------------------------------------------- */

void out_printf(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__ ((format (printf, 1, 2)))
#endif
;
void out_vprintf(const char *fmt, va_list ap);
void out_putchar(int c);

/* Writes `str' as it stands.  Unlike puts(3) it appends no newline;
 * out_puts_line() is the one that does. */
void out_puts(const char *str);
void out_puts_line(const char *str);

void out_flush(void);

/* Non-zero once --json has been given. */
extern int out_json;

/* Says that the trace is about to be aimed at standard output.  Fails,
 * printing to stderr and returning non-zero, if --json has already
 * claimed it; otherwise records the claim so that a later --json can
 * refuse in the same way.  The two orders have to behave alike. */
int out_trace_claims_stdout(void);

/* Turns on --json.  Fails, printing to stderr and returning non-zero,
 * if the trace has already been aimed at standard output. */
int out_set_json(void);

/* --- The run ------------------------------------------------------
 *
 * One cadaver process is one run.  It holds the sequence of commands
 * that were executed, in order. */

/* Starts the run.  `target' is the URL from the command line, or NULL
 * when cadaver was started without one. */
void run_begin(const char *target);

/* Ends the run: writes the JSON document if --json was given, and
 * returns the exit status.  That is the number of commands that failed,
 * capped at CAD_EXIT_MAX so that it can never be confused with the
 * range a shell uses to report a signalled exit. */
int run_finish(void);

#define CAD_EXIT_MAX 125

/* A command line that could not be understood at all.  A session that
 * got as far as running commands reports how many of them failed
 * instead, so 2 from cadaver means either two failed commands or a
 * usage error -- and a usage error executes nothing, which is what
 * tells the two apart. */
#define CAD_EXIT_USAGE 2

/* --- One command --------------------------------------------------
 *
 * Every command line that is executed produces one record, whether it
 * touched the network or not.
 *
 * cmd_begin() opens the record before the line has been parsed, because
 * expanding a wildcard prints as it goes, and makes a request per
 * collection it looks inside, and both belong to the command that
 * caused them.  `line' is the command line as typed, which is all
 * there is to label those requests with until it has been parsed;
 * NULL where there is no line, as for the URL on the command line.
 *
 * cmd_named() then says what the command turned out to be: `argv' is
 * the argument vector without the command name and `argc' its length.
 * A line that held no command at all -- blank, or nothing but a
 * comment -- is dropped with cmd_discard() rather than recorded as
 * having run. */
void cmd_begin(const char *line);
void cmd_named(const char *name, int argc, const char **argv);
void cmd_discard(void);
void cmd_end(void);

/* The command in progress as "N (name args...)", for tagging its
 * requests in the trace.  Never NULL.  Not cmd_label(): that name
 * belongs to the `label' command in enum command_id. */
const char *cmd_trace_label(void);

/* Records a failure of the command in progress that is not the outcome
 * of a request: an argument that was wrong, a local file that could not
 * be opened, a refusal to act.  `context' is the human-readable reason,
 * which the caller has usually just printed. */
void cmd_failed(const char *context);

/* --- One operation within a command --------------------------------
 *
 * Most commands perform exactly one operation, but mput, mget, and copy
 * and move with several sources perform one per resource.  out_start*()
 * begins an operation and exactly one of out_success(), out_result()
 * and out_fail() ends it. */

/* Tell them we are doing VERB to NOUN, where NOUN is a native path. */
void out_start(const char *verb, const char *noun);

/* The same, for a noun that is a URI path. */
void out_start_uri(const char *verb, const char *uri_path);

/* The same, for a source and a destination, both URI paths. */
void out_start_2uri(const char *verb, const char *uri_path1,
                    const char *uri_path2);

/* Begins an operation that announces itself in some other shape:
 * `propnames' names the resource its own way. */
void out_start_raw(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__ ((format (printf, 1, 2)))
#endif
;

/* Begins an operation that transfers a body, so that the progress
 * indicator knows which way the bytes are going.  `dir' is o_upload or
 * o_download. */
void out_start_transfer(enum output_type dir, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__ ((format (printf, 2, 3)))
#endif
;

void out_success(void);

/* Ends the operation as a success, printing `text' where "succeeded."
 * would have gone: `ls' on a collection with no members reports that it
 * is empty, which is an answer rather than a failure. */
void out_success_as(const char *text);

/* Ends the operation as a success without printing anything, for a
 * command whose answer is the output it has already produced. */
void out_done(void);

/* Ends the operation as a failure without printing anything, for a
 * command that has already said why in its own words. */
void out_failed(const char *context);

/* Ends the operation according to a neon error code, printing the
 * matching message. */
void out_result(int ret);

/* out_result(), returning non-zero if the operation succeeded. */
int out_handle(int ret);

/* Ends the operation as a failure.  `fmt' is the reason alone: the
 * "failed:" that out_result() prints is written here, so that every
 * failure in a transcript ends its first line the same way and the
 * reason recorded for --json is the reason rather than the word.  Give
 * it a trailing newline; it is removed from the recorded reason. */
void out_fail(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__ ((format (printf, 1, 2)))
#endif
;

/* --- What a request did -------------------------------------------
 *
 * Recorded by hooks on the neon session, so that a consumer of the JSON
 * can classify a failure by method and status rather than by matching
 * the prose in "context". */
void req_started(const char *method, const char *target);
void req_status(int status);

/* The status of the last response received since the operation in
 * progress began, or 0 if none arrived.  Valid until the operation is
 * ended, which forgets it, so a caller that wants to say something
 * about a particular status has to read it first. */
int req_last_status(void);

/* --- Structured detail --------------------------------------------
 *
 * The commands that produce data rather than just an outcome hand it
 * over here as well as printing it, so that --json carries it as
 * something other than lines of text.  Each attaches to the command in
 * progress and does nothing when --json was not given. */

/* One member of an `ls' listing. */
void res_listing(const char *uri_path, enum resource_type type,
                 dav_size_t size, time_t modtime, int is_executable,
                 int error_status, const char *error_reason);

/* One property from `propget' or `propnames'.  `value' is NULL for
 * propnames, which asks for the names alone. */
void res_property(const char *nspace, const char *name, const char *value,
                  int status);

/* The status and headers of the response to `head'. */
void res_http_status(int status);
void res_header(const char *name, const char *value);

/* One lock, from `lock', `discover', `steal' or `showlocks'. */
void res_lock(const struct ne_lock *lock);

/* A path the command was asked to report: `pwd' and `lpwd'. */
void res_path(const char *path);

/* One option and its value, from `set' with no argument. */
void res_option(const char *name, const char *value);

/* What `bench' measured.  Byte counts are exact; durations are
 * wall-clock seconds, reported to millisecond resolution; rates are
 * MiB/s, powers of 1024.  Every field is filled in before this is
 * called, because a benchmark that did not finish reports the failure
 * instead of a partial measurement. */
struct bench_result {
    const char *target;         /* the collection measured, a URI path */
    const char *started;        /* ISO 8601 UTC, or "" if unreadable */
    const char *latency_op;     /* the method the samples timed */
    int iterations;
    ne_off_t payload_bytes;
    int latency_samples;
    double latency_min, latency_median, latency_max;   /* seconds */
    ne_off_t upload_bytes, download_bytes;
    double upload_seconds, download_seconds;
};

void res_benchmark(const struct bench_result *result);

#endif /* CAD_OUTPUT_H */
