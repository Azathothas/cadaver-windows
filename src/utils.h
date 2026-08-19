/* 
   cadaver, command-line DAV client
   Copyright (C) 1999-2001, Joe Orton <joe@manyfish.co.uk>, 
   except where otherwise indicated.
                                                                     
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

#include <ne_string.h> /* for ne_buffer */

#include "cadaver.h"

/* Returns resource type of resource with given URI; where resr_error
 * means "resource not found."  Answers from what the command in
 * progress has already been told, where it has been told; see
 * restype_remember() below. */
enum resource_type getrestype(const char *uri);

/* What one command has learned about the resources it is working on.
 *
 * A command asks what a path is before doing anything with it, and one
 * PROPFIND per question is the whole cost of a command over a slow
 * link.  Expanding a remote wildcard was the worst of it: the depth-1
 * PROPFIND that lists a collection already says what each member is,
 * and lib/glob.c then asked again, once per member, through
 * davglob_stat().
 *
 * Only positive answers are kept.  A caller that got resr_error wants
 * the session error that goes with it, and that answer is also the one
 * a command changes when it creates something.
 */

/* Remembers that `uri_path' is `type'.  A no-op for resr_error. */
void restype_remember(const char *uri_path, enum resource_type type);

/* Forgets everything.  Called at the end of every command, and from
 * req_started() as soon as a method is sent that could change what any
 * of it means: answering from memory about a server other people are
 * writing to is what this must not do. */
void restype_forget_all(void);

/* Drops whatever a request is about to make untrue.  A safe method
 * -- PROPFIND, GET, HEAD, OPTIONS, REPORT, SEARCH -- leaves the cache
 * alone; one that changes the request target and nothing else drops
 * that target and what is under it; anything else drops the lot. */
void restype_note_request(const char *method, const char *target);

/* Returns time to display */
char *format_time(time_t when);

/* Writes `when' into `buf' as an ISO 8601 UTC timestamp with second
 * precision and a trailing Z, e.g. 2026-08-19T13:04:54Z.  Returns
 * non-zero on success.  Used for the timestamps in --json output, which
 * carry no sub-second part because the servers this reads them from do
 * not either. */
int iso8601_utc(time_t when, char *buf, size_t buflen);

/* Seconds since an arbitrary fixed point, or 0.0 where the clock could
 * not be read.  Only differences between two readings mean anything.
 * Wall-clock, so a duration measured with it includes server and
 * network time, which for a request is most of it. */
double cad_now_seconds(void);

/* Writes the current time into `buf' as an ISO 8601 UTC timestamp with
 * millisecond precision and a trailing Z, sub-second digits truncated
 * rather than rounded so the stamp never names a moment that had not
 * happened yet.  Leaves `buf' empty if the clock could not be read. */
void cad_now_iso8601(char *buf, size_t buflen);

/* Appends `str' to `buf' with the five characters that are not
 * themselves in XML character data replaced by entity references.  neon
 * has no such helper: ne_locks.c concatenates the lock owner into the
 * request body verbatim, so cadaver has to hand it something that is
 * already well-formed. */
void xml_escape(ne_buffer *buf, const char *str);
