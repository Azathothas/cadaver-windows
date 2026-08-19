/*
   cadaver, command-line DAV client -- the `bench' command
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

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <ne_basic.h>
#include <ne_props.h>
#include <ne_string.h>
#include <ne_alloc.h>
#include <ne_uri.h>

#include "i18n.h"
#include "cadaver.h"
#include "commands.h"
#include "options.h"
#include "output.h"
#include "transfer.h"
#include "utils.h"
#include "bench.h"

/* The resource the payload is written to, in the current collection.
 * One fixed name rather than a random one, so that a transcript of one
 * run reads the same as the next; `bench' refuses to run if something
 * is already there under it, because deleting it afterwards is part of
 * what this does. */
#define BENCH_NAME "cadaver-bench.dat"

#define BENCH_DEFAULT_SIZE (1024 * 1024)
#define BENCH_DEFAULT_COUNT 3

/* Enough to measure a fast link and small enough to hold twice over
 * without thinking about it.  A payload this size on a slow one is the
 * user's decision, not a mistake to guard against. */
#define BENCH_MAX_SIZE ((ne_off_t)1024 * 1024 * 1024)

#define ONE_MIB (1024.0 * 1024.0)

/* "1048576 bytes (1.0 MiB)": the exact count, with the rounded binary
 * figure in brackets after it.  Powers of 1024 throughout, as
 * everything here is. */
static char *human_bytes(ne_off_t bytes, char *buf, size_t buflen)
{
    static const char *const unit[] = { "KiB", "MiB", "GiB", "TiB" };
    double value = (double)bytes;
    int n;

    if (bytes < 1024) {
        ne_snprintf(buf, buflen, "%" NE_FMT_NE_OFF_T " bytes", bytes);
        return buf;
    }

    for (n = 0; n < 3 && value >= 1024.0 * 1024.0; n++)
        value /= 1024.0;
    value /= 1024.0;

    ne_snprintf(buf, buflen, "%" NE_FMT_NE_OFF_T " bytes (%.1f %s)",
                bytes, value, unit[n]);
    return buf;
}

/* A size with an optional K, M or G suffix, all powers of 1024.  Returns
 * -1 if `arg' is not one.
 *
 * Every step is bounded before it is taken.  Multiplying first and
 * checking afterwards overflows a signed 64-bit value on an argument a
 * user can type -- `bench 9000000000G' -- and signed overflow is
 * undefined, not a large number. */
static ne_off_t parse_size(const char *arg)
{
    ne_off_t value = 0, multiplier = 1;
    const char *p = arg;

    if (*p < '0' || *p > '9') return -1;

    for (; *p >= '0' && *p <= '9'; p++) {
        int digit = *p - '0';

        if (value > (BENCH_MAX_SIZE - digit) / 10) return -1;
        value = value * 10 + digit;
    }

    switch (*p) {
    case 'k': case 'K': multiplier = 1024; p++; break;
    case 'm': case 'M': multiplier = 1024 * 1024; p++; break;
    case 'g': case 'G': multiplier = 1024 * 1024 * 1024; p++; break;
    default: break;
    }

    /* "4M", "4MiB" and "4MB" all mean the same here, because every unit
     * in this program is a power of 1024 and offering a spelling that
     * meant something else would be a trap. */
    if (strcasecmp(p, "b") == 0 || strcasecmp(p, "ib") == 0) p += strlen(p);

    if (*p != '\0' || value <= 0) return -1;
    if (value > BENCH_MAX_SIZE / multiplier) return -1;

    return value * multiplier;
}

/* A count between 1 and 1000, or -1.  Not atoi(): that reads "3x" as 3
 * and everything that is not a number as 0, and a size argument beside
 * it is already strict. */
static int parse_count(const char *arg)
{
    int value = 0;
    const char *p = arg;

    if (*p < '0' || *p > '9') return -1;

    for (; *p >= '0' && *p <= '9'; p++) {
        value = value * 10 + (*p - '0');
        if (value > 1000) return -1;
    }

    if (*p != '\0' || value < 1) return -1;

    return value;
}

/* A payload that does not compress, so that a server or a proxy with
 * gzip turned on is measured moving the bytes rather than moving a
 * description of them.  xorshift32, seeded fixed: the same run twice
 * sends the same bytes. */
static void fill_payload(char *buf, size_t length)
{
    unsigned int state = 0x2545F491u;
    size_t n;

    for (n = 0; n < length; n++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        buf[n] = (char)(state & 0xFFu);
    }
}

/* The rate, or "-" where the duration is under the millisecond the
 * report can express: a rate worked out from a duration that reads as
 * 0.000 seconds is one the line beside it cannot support.  Reach for a
 * larger payload rather than a smaller unit. */
static const char *rate_str(ne_off_t bytes, double seconds,
                            char *buf, size_t buflen)
{
    if (seconds < 0.0005) return "-";

    ne_snprintf(buf, buflen, "%.2f", (double)bytes / ONE_MIB / seconds);
    return buf;
}

static int compare_doubles(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* A PROPFIND that asks for one property and keeps nothing: the request
 * exists to be timed. */
static void discard_props(void *userdata, const ne_uri *uri,
                          const ne_prop_result_set *set)
{
}

static int latency_probe(const char *uri_path)
{
    static const ne_propname props[] = {
        { "DAV:", "resourcetype" },
        { NULL, NULL }
    };

    return ne_simple_propfind(session.sess, uri_path, NE_DEPTH_ZERO, props,
                              discard_props, NULL);
}

void execute_bench(const char *arg_size, const char *arg_count)
{
    struct bench_result result;
    char *uri_coll, *uri_res, *payload = NULL;
    double *samples = NULL;
    char started[40], num[64], num2[64], rate1[32], rate2[32];
    ne_off_t size = BENCH_DEFAULT_SIZE;
    int count = BENCH_DEFAULT_COUNT, n, ret = NE_OK;
    int in_the_way;
    double begin;

    uri_coll = ne_strdup(session.uri.path);
    uri_res = ne_concat(uri_coll, BENCH_NAME, NULL);

    if (arg_size && (size = parse_size(arg_size)) < 0) {
        out_start_uri(_("Benchmarking"), uri_coll);
        out_fail(_("`%s' is not a size; give a number of bytes, optionally "
                   "with a K, M or G suffix.\n"), arg_size);
        goto done;
    }

    if (arg_count && (count = parse_count(arg_count)) < 0) {
        out_start_uri(_("Benchmarking"), uri_coll);
        out_fail(_("`%s' is not a count between 1 and 1000.\n"), arg_count);
        goto done;
    }

    /* Deleting the payload afterwards is part of this, so it must not
     * be something that was already there.  Asked before the operation
     * is opened, so that the PROPFIND behind it is not reported as the
     * request the refusal was about: it succeeded, and the command
     * never got as far as doing anything. */
    in_the_way = getrestype(uri_res) != resr_error;

    out_start_uri(_("Benchmarking"), uri_coll);

    if (in_the_way) {
        out_fail(_("`%s' already exists; `bench' writes and deletes that "
                   "name and will not touch one it did not create.\n"),
                 BENCH_NAME);
        goto done;
    }

    payload = malloc((size_t)size);
    if (payload == NULL) {
        out_fail(_("could not allocate a payload of %" NE_FMT_NE_OFF_T
                   " bytes.\n"), size);
        goto done;
    }

    fill_payload(payload, (size_t)size);
    samples = ne_malloc((size_t)count * sizeof *samples);

    memset(&result, 0, sizeof result);
    cad_now_iso8601(started, sizeof started);
    result.target = uri_coll;
    result.started = started;
    result.latency_op = "PROPFIND";
    result.iterations = count;
    result.payload_bytes = size;

    /* Latency first, and on the collection rather than on the payload:
     * it is the round trip that is being measured, and it should not
     * depend on what the uploads left behind. */
    for (n = 0; n < count; n++) {
        double at = cad_now_seconds();

        ret = latency_probe(uri_coll);
        if (ret != NE_OK) goto failed;

        samples[n] = cad_now_seconds() - at;
        if (cad_transfer_interrupted()) goto failed;
    }

    /* The upper middle for an even number of samples, which is what
     * three or a handful of round trips is; there is no averaging here,
     * because every figure reported is one the clock produced. */
    qsort(samples, (size_t)count, sizeof *samples, compare_doubles);
    result.latency_samples = count;
    result.latency_min = samples[0];
    result.latency_median = samples[count / 2];
    result.latency_max = samples[count - 1];

    begin = cad_now_seconds();
    for (n = 0; n < count; n++) {
        ret = cad_put_buffer(uri_res, payload, (size_t)size);
        if (ret != NE_OK) goto failed;
        result.upload_bytes += size;
    }
    result.upload_seconds = cad_now_seconds() - begin;

    begin = cad_now_seconds();
    for (n = 0; n < count; n++) {
        ne_off_t got = 0;

        ret = cad_get_discard(uri_res, &got);
        if (ret != NE_OK) goto failed;

        if (got != size) {
            ne_set_error(session.sess,
                         _("The server returned %" NE_FMT_NE_OFF_T " bytes "
                           "for a resource of %" NE_FMT_NE_OFF_T),
                         got, size);
            ret = NE_ERROR;
            goto failed;
        }

        result.download_bytes += got;
    }
    result.download_seconds = cad_now_seconds() - begin;

    ret = ne_delete(session.sess, uri_res);
    if (ret != NE_OK) goto failed;

    out_success();
    res_benchmark(&result);

    out_printf(_("  started    %s\n"), started[0] ? started : _("unknown"));
    out_printf(_("  payload    %s, %d iteration%s\n"),
               human_bytes(size, num, sizeof num), count,
               count == 1 ? "" : "s");
    out_printf(_("  %-9s  min %.3f ms, median %.3f ms, max %.3f ms over "
                 "%d samples\n"),
               result.latency_op, result.latency_min * 1000.0,
               result.latency_median * 1000.0, result.latency_max * 1000.0,
               count);
    out_printf(_("  upload     %s in %.3f s wall clock, %s MiB/s\n"),
               human_bytes(result.upload_bytes, num, sizeof num),
               result.upload_seconds,
               rate_str(result.upload_bytes, result.upload_seconds,
                        rate1, sizeof rate1));
    out_printf(_("  download   %s in %.3f s wall clock, %s MiB/s\n"),
               human_bytes(result.download_bytes, num2, sizeof num2),
               result.download_seconds,
               rate_str(result.download_bytes, result.download_seconds,
                        rate2, sizeof rate2));
    goto done;

failed:
    out_result(ret);
    /* The payload may be on the server whatever went wrong after the
     * first PUT, and leaving it there would make the next run refuse to
     * start.  Its outcome is not the command's: the failure above is. */
    (void) ne_delete(session.sess, uri_res);

done:
    if (payload) free(payload);
    if (samples) ne_free(samples);
    ne_free(uri_res);
    ne_free(uri_coll);
}
