/* 
   cadaver, command-line DAV client
   Copyright (C) 1999-2001, Joe Orton <joe@manyfish.co.uk>, 
   Portions are:
   Copyright (C) 85, 88, 90, 91, 1995-1999 Free Software Foundation, Inc.
                                                                     
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

#include <time.h>
#include <sys/types.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <ne_basic.h>
#include <ne_string.h>
#include <ne_uri.h>

#include "i18n.h"
#include "cadaver.h"
#include "utils.h"

/* Returns non-zero if given resource is not a collection resource.
 * This function MAY make a request to the server. */
enum resource_type getrestype(const char *uri_path)
{
    struct resource *res = NULL;
    int ret = 0;
    /* TODO: just request resourcetype here. */
    ret = fetch_resource_list(session.sess, uri_path, NE_DEPTH_ZERO, 1, &res);
    if (ret == NE_OK) {
	if (res != NULL && ne_path_compare(uri_path, res->uri) == 0) {
	    ret = res->type;
	} else {
	    /* FIXME: this error occurs when you do open /foo and get
	     * the response for /foo/ back. */
	    ne_set_error(session.sess, 
                         _("Unknown resource found at '%s' without WebDAV support"),
                         res->uri);
	    ret = resr_error;
	}
    }
    else {
	ret = resr_error;
    }
    free_resource_list(res);
    return ret;
}


char *format_time(time_t when)
{
    const char *fmt;
    static char ret[256];
    struct tm *local;
    time_t current_time;
    
    if (when == (time_t)-1) {
	/* Happens on lock-null resources */
	return "  (unknown) ";
    }

    /* from GNU fileutils... this section is 
     *  
     */
    current_time = time(NULL);
    if (current_time > when + 6L * 30L * 24L * 60L * 60L	/* Old. */
	|| current_time < when - 60L * 60L) {
	/* The file is fairly old or in the future.  POSIX says the
	   cutoff is 6 months old; approximate this by 6*30 days.
	   Allow a 1 hour slop factor for what is considered "the
	   future", to allow for NFS server/client clock disagreement.
	   Show the year instead of the time of day.  */
	fmt = "%b %e  %Y";
    } else {
	fmt = "%b %e %H:%M";
    }

    local = localtime(&when);
    if (local != NULL) {
	if (strftime(ret, 256, fmt, local)) {
	    return ret;
	}
    }
    return "???";
}

/* Writes `when' into `buf' as an ISO 8601 UTC timestamp with second
 * precision and a trailing Z. */
int iso8601_utc(time_t when, char *buf, size_t buflen)
{
    struct tm *utc = gmtime(&when);

    if (utc == NULL
        || strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", utc) == 0) {
        if (buflen) buf[0] = '\0';
        return 0;
    }

    return 1;
}

double cad_now_seconds(void)
{
#if defined(HAVE_GETTIMEOFDAY) && defined(HAVE_SYS_TIME_H)
    struct timeval tv;

    if (gettimeofday(&tv, NULL) == 0)
        return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
#endif
    return 0.0;
}

void cad_now_iso8601(char *buf, size_t buflen)
{
#if defined(HAVE_GETTIMEOFDAY) && defined(HAVE_SYS_TIME_H)
    struct timeval tv;
    struct tm *utc;
    time_t secs;
    char stamp[32];

    if (gettimeofday(&tv, NULL) == 0
        && (secs = (time_t)tv.tv_sec, (utc = gmtime(&secs)) != NULL)
        && strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", utc) != 0) {
        /* tv_usec is microseconds; truncated rather than rounded. */
        if (ne_snprintf(buf, buflen, "%s.%03ldZ", stamp,
                        (long)(tv.tv_usec / 1000)) != 0)
            return;
    }
#endif
    if (buflen) buf[0] = '\0';
}

void xml_escape(ne_buffer *buf, const char *str)
{
    for (; str && *str; str++) {
        switch (*str) {
        case '&': ne_buffer_czappend(buf, "&amp;"); break;
        case '<': ne_buffer_czappend(buf, "&lt;"); break;
        case '>': ne_buffer_czappend(buf, "&gt;"); break;
        case '"': ne_buffer_czappend(buf, "&quot;"); break;
        case '\'': ne_buffer_czappend(buf, "&apos;"); break;
        default: ne_buffer_append(buf, str, 1); break;
        }
    }
}
