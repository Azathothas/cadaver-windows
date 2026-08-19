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
 * means "resource not found." */
enum resource_type getrestype(const char *uri);

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
