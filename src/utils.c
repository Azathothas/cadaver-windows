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

#include <string.h>
#include <time.h>
#include <sys/types.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <ne_alloc.h>
#include <ne_basic.h>
#include <ne_string.h>
#include <ne_uri.h>
#include <ne_utils.h>

#include "i18n.h"
#include "cadaver.h"
#include "utils.h"

/* --- What one command has learned ---------------------------------
 *
 * A linked list rather than a hash: one command asks about a handful of
 * paths, or about the members of one collection, and the list is thrown
 * away when the command ends.  The cap is there so that a wildcard
 * matching a very wide collection cannot make it unbounded. */

#define RESTYPE_CACHE_MAX 4096

struct restype_entry {
    char *uri;
    enum resource_type type;
    struct restype_entry *next;
};

static struct restype_entry *restype_cache;
static int restype_count;

void restype_remember(const char *uri_path, enum resource_type type)
{
    struct restype_entry *entry;

    if (type == resr_error || uri_path == NULL) return;
    if (restype_count >= RESTYPE_CACHE_MAX) return;

    for (entry = restype_cache; entry; entry = entry->next) {
        if (ne_path_compare(entry->uri, uri_path) == 0) {
            entry->type = type;
            return;
        }
    }

    entry = ne_malloc(sizeof *entry);
    entry->uri = ne_strdup(uri_path);
    entry->type = type;
    entry->next = restype_cache;
    restype_cache = entry;
    restype_count++;
}

void restype_forget_all(void)
{
    struct restype_entry *entry = restype_cache, *next;

    while (entry) {
        next = entry->next;
        ne_free(entry->uri);
        ne_free(entry);
        entry = next;
    }

    restype_cache = NULL;
    restype_count = 0;
}

/* Forgets `uri_path' and everything under it. */
static void restype_forget_under(const char *uri_path)
{
    size_t len = strlen(uri_path);
    struct restype_entry **link = &restype_cache, *entry;

    if (len == 0) {
        restype_forget_all();
        return;
    }

    while ((entry = *link) != NULL) {
        int under = ne_path_compare(entry->uri, uri_path) == 0
            || (strncmp(entry->uri, uri_path, len) == 0
                && (uri_path[len - 1] == '/' || entry->uri[len] == '/'));

        if (under) {
            *link = entry->next;
            ne_free(entry->uri);
            ne_free(entry);
            restype_count--;
        }
        else {
            link = &entry->next;
        }
    }
}

void restype_note_request(const char *method, const char *target)
{
    /* Nothing about a resource changes because it was read. */
    static const char *const safe[] = {
        "PROPFIND", "GET", "HEAD", "OPTIONS", "REPORT", "SEARCH", NULL
    };
    /* These change the request target and what is under it, and reach
     * no further, so the rest of what has been remembered still holds.
     * That matters: a wildcard delete used to make one PROPFIND per
     * file after the first, because each DELETE threw away what the
     * listing had just said about all the others. */
    static const char *const local[] = {
        "PUT", "DELETE", "MKCOL", "PROPPATCH", "LOCK", "UNLOCK", NULL
    };
    int n;

    if (method == NULL) return;

    for (n = 0; safe[n] != NULL; n++)
        if (strcmp(method, safe[n]) == 0) return;

    for (n = 0; local[n] != NULL; n++) {
        if (strcmp(method, local[n]) == 0) {
            if (target) restype_forget_under(target);
            else restype_forget_all();
            return;
        }
    }

    /* COPY and MOVE name their destination in a header rather than in
     * the request target, and the DeltaV methods act on resources this
     * has no way to name, so everything goes. */
    restype_forget_all();
}

/* The remembered type of `uri_path', or -1 if it is not remembered.
 * Not resr_error: that is a type of its own and is never cached. */
static int restype_lookup(const char *uri_path)
{
    const struct restype_entry *entry;

    for (entry = restype_cache; entry; entry = entry->next)
        if (ne_path_compare(entry->uri, uri_path) == 0)
            return (int)entry->type;

    return -1;
}

/* Returns non-zero if given resource is not a collection resource.
 * This function MAY make a request to the server. */
enum resource_type getrestype(const char *uri_path)
{
    struct resource *res = NULL;
    int ret = restype_lookup(uri_path);

    if (ret >= 0) {
        NE_DEBUG(DEBUG_FILES, "cadaver: %s is a %s, already asked\n",
                 uri_path, ret == resr_collection ? "collection" : "resource");
        return (enum resource_type)ret;
    }

    /* TODO: just request resourcetype here. */
    ret = fetch_resource_list(session.sess, uri_path, NE_DEPTH_ZERO, 1, &res);
    if (ret != NE_OK) {
        ret = resr_error;
    }
    else if (res != NULL && ne_path_compare(uri_path, res->uri) == 0) {
        ret = res->type;
    }
    else if (res != NULL) {
        /* This happens when you open /foo and get the response for
         * /foo/ back. */
        ne_set_error(session.sess,
                     _("Unknown resource found at '%s' without WebDAV support"),
                     res->uri);
        ret = resr_error;
    }
    else {
        /* A multistatus that named nothing at all.  Reading res->uri
         * here, as this used to, is a null dereference. */
        ne_set_error(session.sess,
                     _("The server reported nothing about '%s'"), uri_path);
        ret = resr_error;
    }

    free_resource_list(res);
    restype_remember(uri_path, (enum resource_type)ret);

    return (enum resource_type)ret;
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
