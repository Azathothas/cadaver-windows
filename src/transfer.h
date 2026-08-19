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

#ifndef CAD_TRANSFER_H
#define CAD_TRANSFER_H

#include <ne_basic.h>
#include <ne_session.h>

/* GET and PUT that Ctrl-C can stop.
 *
 * neon reads or writes a whole body inside one call and has no cancel,
 * so ne_get() and ne_put() can only be interrupted by ending the
 * process -- which during an upload leaves the server holding a partial
 * resource.  These are the same dispatch loops with the interrupt flag
 * checked between blocks: the first Ctrl-C abandons the transfer and
 * returns to the prompt, and a second one ends the session the way it
 * always did, which is the way out of a transfer that has stalled
 * completely and so is not reaching the check at all.
 *
 * Each returns an NE_* code and each closes the connection if it did
 * not finish, because a body that was only partly read leaves nothing
 * useful on the socket.  An interrupted transfer reports NE_ERROR with
 * the session error set, so a caller that already handles a failure
 * needs no new case. */

/* GET `uri_path' into `fd'. */
int cad_get(const char *uri_path, int fd);

/* GET the range `range' of `uri_path' and append it to `fd'.  As
 * ne_get_range(): a 2xx that is not 206 means the server ignored the
 * Range header, which is an error here because the caller asked for
 * part of the resource and would otherwise append the whole of it. */
int cad_get_range(const char *uri_path, ne_content_range *range, int fd);

/* PUT the rest of `fd', from wherever it is now, to `uri_path'. */
int cad_put(const char *uri_path, int fd);

/* PUT `length' bytes of `buffer'.  For `bench', which measures what the
 * connection does and has no reason to put a local disk in the way. */
int cad_put_buffer(const char *uri_path, const char *buffer, size_t length);

/* GET `uri_path' and throw the body away, counting it into `*bytes'.
 * Also for `bench': the point is how fast the bytes arrive. */
int cad_get_discard(const char *uri_path, ne_off_t *bytes);

/* Whether a transfer in this command stopped because of Ctrl-C and
 * not because of the server.  A command that performs several
 * transfers reads it to decide whether to go on to the next.
 *
 * cad_transfer_forget() clears it, and src/cadaver.c calls that at
 * the start of every command: the flag has to outlive the transfer
 * that set it, so something has to clear it, and the command that
 * was interrupted is the last one it means anything to. */
int cad_transfer_interrupted(void);
void cad_transfer_forget(void);

#endif /* CAD_TRANSFER_H */
