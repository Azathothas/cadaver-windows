/*
   cadaver, command-line DAV client -- interruptible transfers
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
#include <signal.h>
#include <errno.h>

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <ne_request.h>
#include <ne_locks.h>
#include <ne_string.h>
#include <ne_utils.h>

#include "i18n.h"
#include "system.h"
#include "cadaver.h"
#include "common.h"
#include "transfer.h"

/* The block size the loops below move at a time.  neon uses 4096 for
 * its own response buffer; a larger one here means fewer checks of the
 * interrupt flag rather than a faster transfer, since the socket is
 * what sets the pace. */
#define TRANSFER_BLOCK 8192

/* Set from the signal handler installed for the length of a transfer,
 * and read between blocks.  sig_atomic_t because that is the only type
 * a handler may write and the rest of the program read. */
static volatile sig_atomic_t interrupted;
static void (*old_sigint)(int);

/* Ctrl-C during a transfer.
 *
 * The first one asks the loops below to stop, which they do when the
 * next block arrives.  A transfer that has stalled completely delivers
 * no next block, so a second one falls back to whatever handler was
 * installed before -- ending the session, as it did before there was
 * any of this. */
static void transfer_interrupt(int sig)
{
    signal(sig, transfer_interrupt);

    if (interrupted && old_sigint != SIG_DFL && old_sigint != SIG_IGN
        && old_sigint != SIG_ERR) {
        (*old_sigint)(sig);
        return;
    }

    interrupted = 1;
}

static void interrupt_begin(void)
{
    interrupted = 0;
    old_sigint = signal(SIGINT, transfer_interrupt);
}

static void interrupt_end(void)
{
    signal(SIGINT, old_sigint);
}

int cad_transfer_interrupted(void)
{
    return interrupted != 0;
}

/* Says the transfer was abandoned.  The caller reports it the way it
 * reports any other failure; nothing else needs a new case. */
static int say_interrupted(void)
{
    ne_set_error(session.sess, _("Interrupted."));
    ne_close_connection(session.sess);
    return NE_ERROR;
}

/* Writes `len' bytes of `block' to `fd', retrying a short write. */
static int write_block(int fd, const char *block, ssize_t len)
{
    while (len > 0) {
        ssize_t ret = write(fd, block, (unsigned int)len);

        if (ret < 0) {
            if (errno == EINTR) continue;
            ne_set_error(session.sess, _("Could not write to file: %s"),
                         strerror(errno));
            return NE_ERROR;
        }

        len -= ret;
        block += ret;
    }

    return NE_OK;
}

/* Where a response body is counted, for the callers that want the size
 * rather than the bytes.  NULL when nobody is counting. */
static ne_off_t *body_counter;

/* The body of a response, block by block, into `fd' or discarded when
 * `fd' is negative.  Returns an NE_* code. */
static int read_body(ne_request *req, int fd)
{
    char buf[TRANSFER_BLOCK];
    ssize_t len;

    while ((len = ne_read_response_block(req, buf, sizeof buf)) > 0) {
        if (interrupted) return say_interrupted();
        if (body_counter) *body_counter += len;
        if (fd >= 0 && write_block(fd, buf, len) != NE_OK) return NE_ERROR;
    }

    return len == 0 ? NE_OK : NE_ERROR;
}

/* One GET, dispatched the way neon's own dispatch_to_fd() does it, with
 * the interrupt check in the read loop.  `brange' is the Range header
 * that was sent, or NULL; a 206 has to come back with a Content-Range
 * that matches it, or the bytes written are not the bytes asked for. */
static int dispatch_get(ne_request *req, int fd, const char *brange)
{
    const ne_status *const st = ne_get_status(req);
    size_t rlen = brange ? strlen(brange + 6) : 0; /* past "bytes=" */
    int ret;

    do {
        const char *value;

        ret = ne_begin_request(req);
        if (ret != NE_OK) break;

        value = ne_get_response_header(req, "Content-Range");

        if (brange && st->code == 206
            && (value == NULL || strncmp(value, "bytes ", 6) != 0
                || strncmp(brange + 6, value + 6, rlen)
                || (brange[5 + rlen] != '-' && value[6 + rlen] != '/'))) {
            ne_set_error(session.sess,
                         _("Response did not include requested range"));
            return NE_ERROR;
        }

        if ((brange && st->code == 206) || (!brange && st->klass == 2))
            ret = read_body(req, fd);
        else
            ret = read_body(req, -1);

        if (ret == NE_OK) ret = ne_end_request(req);
    } while (ret == NE_RETRY);

    return ret;
}

int cad_get(const char *uri_path, int fd)
{
    ne_request *req;
    int ret;

    interrupt_begin();
    req = ne_request_create(session.sess, "GET", uri_path);

    ret = dispatch_get(req, fd, NULL);

    if (ret == NE_OK && ne_get_status(req)->klass != 2)
        ret = NE_ERROR;

    if (ret != NE_OK) ne_close_connection(session.sess);

    ne_request_destroy(req);
    interrupt_end();

    return ret;
}

int cad_get_discard(const char *uri_path, ne_off_t *bytes)
{
    ne_off_t counted = 0;
    int ret;

    body_counter = &counted;
    ret = cad_get(uri_path, -1);
    body_counter = NULL;

    if (bytes) *bytes = counted;

    return ret;
}

int cad_put_buffer(const char *uri_path, const char *buffer, size_t length)
{
    ne_request *req;
    int ret;

    interrupt_begin();
    req = ne_request_create(session.sess, "PUT", uri_path);

    ne_lock_using_resource(req, uri_path, 0);
    ne_lock_using_parent(req, uri_path);

    ne_set_request_body_buffer(req, buffer, length);

    ret = ne_request_dispatch(req);

    if (ret == NE_OK && ne_get_status(req)->klass != 2)
        ret = NE_ERROR;

    if (interrupted) ret = say_interrupted();
    else if (ret != NE_OK) ne_close_connection(session.sess);

    ne_request_destroy(req);
    interrupt_end();

    return ret;
}

int cad_get_range(const char *uri_path, ne_content_range *range, int fd)
{
    ne_request *req;
    const ne_status *status;
    char brange[64];
    int ret;

    if (range->end == -1)
        ne_snprintf(brange, sizeof brange, "bytes=%" NE_FMT_NE_OFF_T "-",
                    range->start);
    else
        ne_snprintf(brange, sizeof brange,
                    "bytes=%" NE_FMT_NE_OFF_T "-%" NE_FMT_NE_OFF_T,
                    range->start, range->end);

    interrupt_begin();
    req = ne_request_create(session.sess, "GET", uri_path);
    ne_add_request_header(req, "Range", brange);
    ne_add_request_header(req, "Accept-Ranges", "bytes");

    ret = dispatch_get(req, fd, brange);
    status = ne_get_status(req);

    /* Apache 1.3 cuts the connection short on a 416, so the status is
     * worth looking at even where the dispatch failed. */
    if (ret == NE_OK && status->code == 416) {
        ne_set_error(session.sess, _("Range is not satisfiable"));
        ret = NE_ERROR;
    }
    else if (ret == NE_OK && status->klass == 2 && status->code != 206) {
        ne_set_error(session.sess,
                     _("Resource does not support ranged GET requests"));
        ret = NE_ERROR;
    }
    else if (ret == NE_OK && status->klass != 2) {
        ret = NE_ERROR;
    }

    if (ret != NE_OK) ne_close_connection(session.sess);

    ne_request_destroy(req);
    interrupt_end();

    return ret;
}

/* The request body, block by block, from a file descriptor.  neon calls
 * this with buflen == 0 before each attempt at the body, which is where
 * the descriptor has to go back to the beginning: a request may be sent
 * more than once for an authentication retry. */
struct put_body {
    int fd;
    ne_off_t offset;
};

static ssize_t provide_body(void *userdata, char *buffer, size_t buflen)
{
    struct put_body *body = userdata;
    ssize_t len;

    if (buflen == 0) {
        if (lseek(body->fd, (off_t)body->offset, SEEK_SET) < 0) {
            ne_set_error(session.sess, _("Could not seek in file: %s"),
                         strerror(errno));
            return -1;
        }
        return 0;
    }

    if (interrupted) {
        ne_set_error(session.sess, _("Interrupted."));
        return -1;
    }

    do {
        len = read(body->fd, buffer, (unsigned int)buflen);
    } while (len < 0 && errno == EINTR);

    if (len < 0) {
        ne_set_error(session.sess, _("Could not read from file: %s"),
                     strerror(errno));
        return -1;
    }

    return len;
}

int cad_put(const char *uri_path, int fd)
{
    struct put_body body;
    ne_request *req;
    ne_off_t length;
    struct cad_finfo info;
    int ret;

    /* The length has to be known before the request: without it neon
     * would have to use chunked encoding, which needs the server to
     * have advertised HTTP/1.1 first. */
    if (cad_fd_info(fd, &info) != 0) {
        ne_set_error(session.sess, _("Could not determine file size: %s"),
                     strerror(errno));
        return NE_ERROR;
    }

    body.fd = fd;
    body.offset = lseek(fd, 0, SEEK_CUR);
    if (body.offset < 0) body.offset = 0;

    length = info.size - body.offset;
    if (length < 0) length = 0;

    interrupt_begin();
    req = ne_request_create(session.sess, "PUT", uri_path);

    /* What ne_put() does, and the reason `edit' can write back a
     * resource it holds a lock on. */
    ne_lock_using_resource(req, uri_path, 0);
    ne_lock_using_parent(req, uri_path);

    ne_set_request_body_provider(req, length, provide_body, &body);

    ret = ne_request_dispatch(req);

    if (ret == NE_OK && ne_get_status(req)->klass != 2)
        ret = NE_ERROR;

    if (interrupted) ret = say_interrupted();
    else if (ret != NE_OK) ne_close_connection(session.sess);

    ne_request_destroy(req);
    interrupt_end();

    return ret;
}
