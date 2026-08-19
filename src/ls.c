/* 
   'ls' for cadaver
   Copyright (C) 2000-2004, 2006, 2008, Joe Orton <joe@manyfish.co.uk>, 
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

#include "config.h"

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <time.h>

#include <ne_request.h>
#include <ne_props.h>
#include <ne_uri.h>
#include <ne_alloc.h>
#include <ne_dates.h>

#include "i18n.h"
#include "commands.h"
#include "cadaver.h"
#include "options.h"
#include "utils.h"
#include "basename.h"
#include "utils.h"

struct fetch_context {
    struct resource **list;
    const char *target; /* Request-URI of the PROPFIND */
    unsigned int include_target; /* Include resource at href */
};    

static const ne_propname ls_props[] = {
    { "DAV:", "getcontentlength" },
    { "DAV:", "getlastmodified" },
    { "http://apache.org/dav/props/", "executable" },
    { "DAV:", "resourcetype" },
    { "DAV:", "checked-in" },
    { "DAV:", "checked-out" },
    { NULL }
};

#define ELM_resourcetype (NE_PROPS_STATE_TOP + 1)
#define ELM_collection (NE_PROPS_STATE_TOP + 2)

static const struct ne_xml_idmap ls_idmap[] = {
    { "DAV:", "resourcetype", ELM_resourcetype },
    { "DAV:", "collection", ELM_collection }
};

static int compare_resource(const struct resource *r1, 
			    const struct resource *r2)
{
    /* Sort errors first, then collections, then alphabetically */
    if (r1->type == resr_error) {
	return -1;
    } else if (r2->type == resr_error) {
	return 1;
    } else if (r1->type == resr_collection) {
	if (r2->type != resr_collection) {
	    return -1;
	} else {
	    return strcmp(r1->uri, r2->uri);
	}
    } else {
	if (r2->type != resr_collection) {
	    return strcmp(r1->uri, r2->uri);
	} else {
	    return 1;
	}
    }
}

/* Writes `text' in a field `width' wide, padded on the right when
 * `left' and on the left otherwise.  A field narrower than the text is
 * widened rather than truncating a name. */
static void put_field(const char *text, int width, int left)
{
    int pad = width - (int)strlen(text);

    if (!left) while (pad-- > 0) out_putchar(' ');
    out_puts(text);
    if (left) while (pad-- > 0) out_putchar(' ');
}

/* The rounded binary form of a byte count: 1.0 MiB, powers of 1024. */
static const char *rounded_size(dav_size_t bytes, char *buf, size_t buflen)
{
    static const char *const unit[] = { "KiB", "MiB", "GiB", "TiB" };
    double value = (double)bytes;
    int n;

    if (bytes < 1024) {
        ne_snprintf(buf, buflen, "%" FMT_DAV_SIZE_T "u B", bytes);
        return buf;
    }

    for (n = 0; n < 3 && value >= 1024.0 * 1024.0; n++)
        value /= 1024.0;
    value /= 1024.0;

    ne_snprintf(buf, buflen, "%.1f %s", value, unit[n]);
    return buf;
}

/* What one conversion in `lsformat' stands for.  `res' is the member,
 * `name' its last path segment in native form.  Returns the text, using
 * `buf' where it has to build one. */
static const char *ls_field(char letter, const struct resource *res,
                            const char *name, char *buf, size_t buflen)
{
    char stamp[40];

    switch (letter) {
    case 'n': return name;
    case 'h': return res->uri;
    case 's':
        ne_snprintf(buf, buflen, "%" FMT_DAV_SIZE_T "u", res->size);
        return buf;
    case 'S': return rounded_size(res->size, buf, buflen);
    case 'd': return format_time(res->modtime);
    case 'D':
        if (res->modtime == (time_t)-1
            || !iso8601_utc(res->modtime, stamp, sizeof stamp))
            return "-";
        ne_strnzcpy(buf, stamp, buflen);
        return buf;
    case 't':
        switch (res->type) {
        case resr_reference: return _("Ref:");
        case resr_collection: return _("Coll:");
        case resr_normal: return "";
        default: return "???";
        }
    case 'T':
        switch (res->type) {
        case resr_reference: return "reference";
        case resr_collection: return "collection";
        case resr_normal: return "resource";
        default: return "error";
        }
    case 'e': return res->is_executable ? "*" : " ";
    /* 0: no vcr, 1: checkin, 2: checkout */
    case 'v': return res->is_vcr == 0 ? " " : (res->is_vcr == 1 ? ">" : "<");
    default: return NULL;
    }
}

/* Writes one member according to `lsformat'.  A conversion is a %, an
 * optional `-' for left alignment, an optional field width, and one
 * letter; anything else is written as it stands. */
static void display_ls_formatted(const struct resource *res, const char *name)
{
    const char *fmt = get_option(opt_lsformat);
    const char *p;
    char buf[128];

    if (fmt == NULL) fmt = LS_DEFAULT_FORMAT;

    for (p = fmt; *p != '\0'; p++) {
        int left = 0, width = 0;
        const char *text;

        if (*p != '%') {
            out_putchar(*p);
            continue;
        }

        p++;
        if (*p == '%') {
            out_putchar('%');
            continue;
        }
        if (*p == '-') {
            left = 1;
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        if (*p == '\0') {
            /* A trailing % stands for itself rather than eating the
             * newline below. */
            out_putchar('%');
            break;
        }

        text = ls_field(*p, res, name, buf, sizeof buf);
        if (text == NULL) {
            /* A letter that means nothing is written as it stands, so
             * an unknown conversion is visible rather than silently
             * dropped. */
            out_putchar('%');
            out_putchar(*p);
        }
        else {
            put_field(text, width, left);
        }
    }

    out_putchar('\n');
}

static void display_ls_line(struct resource *res)
{
    const char *path = res->uri;
    char *native_path;

    /* Before the name is cut down to its last segment below. */
    res_listing(res->uri, res->type, res->size, res->modtime,
                res->is_executable, res->error_status, res->error_reason);

    if (ne_path_has_trailing_slash(path)) {
        res->uri[strlen(path)-1] = '\0';
    }
    path = strrchr(path, '/');
    if (path != NULL && strlen(path+1) > 0) {
        path++;
    }
    else {
        path = res->uri;
    }

    native_path = native_path_from_uri(path);

    if (res->type == resr_error) {
        /* A member the server could not report on has a status and a
         * reason and none of the fields `lsformat' names, so it keeps
         * its own line. */
	out_printf(_("Error: %-30s %d %s\n"), native_path, res->error_status,
	       res->error_reason?res->error_reason:_("unknown"));
        cmd_failed(res->error_reason);
    } else {
        display_ls_formatted(res, native_path);
    }

    ne_free(native_path);
}

void execute_ls(const char *native_path)
{
    int ret;
    char *uri_path;
    struct resource *reslist = NULL, *current, *next;

    if (native_path)
        uri_path = uri_resolve_native_coll(native_path);
    else
        uri_path = ne_strdup(session.uri.path);

    out_start_uri(_("Listing collection"), uri_path);
    ret = fetch_resource_list(session.sess, uri_path, 1, 0, &reslist);
    if (ret == NE_OK) {
	/* Easy this, eh? */
	if (reslist == NULL) {
            /* An empty collection is an answer, not a failure. */
	    out_success_as(_("collection is empty.\n"));
	} else {
	    out_success();
	    for (current = reslist; current!=NULL; current = next) {
		next = current->next;
		if (strlen(current->uri) > strlen(uri_path)) {
		    display_ls_line(current);
		}
		free_resource(current);
	    }
	}
    } else {
	out_result(ret);
    }
    ne_free(uri_path);
}

static void results(void *userdata, 
                    const ne_uri *uri,
		    const ne_prop_result_set *set)
{
    struct fetch_context *ctx = userdata;
    struct resource *current, *previous, *newres;
    const char *clength, *modtime, *isexec;
    const char *checkin, *checkout;
    const ne_status *status = NULL;
    const char *path = uri->path;

    newres = ne_propset_private(set);

    if (ne_path_compare(ctx->target, path) == 0 && !ctx->include_target) {
	/* This is the target URI */
	NE_DEBUG(NE_DBG_HTTP, "Skipping target resource.\n");
	/* Free the private structure. */
	ne_free(newres);
	return;
    }

    newres->uri = ne_strdup(uri->path);

    clength = ne_propset_value(set, &ls_props[0]);    
    modtime = ne_propset_value(set, &ls_props[1]);
    isexec = ne_propset_value(set, &ls_props[2]);
    checkin = ne_propset_value(set, &ls_props[4]);
    checkout = ne_propset_value(set, &ls_props[5]);

    
    if (clength == NULL)
	status = ne_propset_status(set, &ls_props[0]);
    if (modtime == NULL)
	status = ne_propset_status(set, &ls_props[1]);

    if (newres->type == resr_normal && status) {
        /* It's an error! */
        newres->error_status = status->code;
        newres->error_reason = ne_strdup(status->reason_phrase);
        newres->type = resr_error;
    }

    if (isexec && strcasecmp(isexec, "T") == 0) {
	newres->is_executable = 1;
    } else {
	newres->is_executable = 0;
    }

    if (modtime)
	newres->modtime = ne_httpdate_parse(modtime);

    if (clength) {
        char *p;

        newres->size = DAV_STRTOL(clength, &p, 10);
        if (*p) {
            newres->size = 0;
        }
    }

    /* is vcr */
    if (checkin) {
	newres->is_vcr = 1;
    } else if (checkout) {
	newres->is_vcr = 2;
    } else {
	newres->is_vcr = 0;
    }

    NE_DEBUG(NE_DBG_HTTP, "End resource %s\n", newres->uri);

    for (current = *ctx->list, previous = NULL; current != NULL; 
	 previous = current, current=current->next) {
	if (compare_resource(current, newres) >= 0) {
	    break;
	}
    }
    if (previous) {
	previous->next = newres;
    } else {
	*ctx->list = newres;
    }
    newres->next = current;
}

static int ls_startelm(void *userdata, int parent, 
                       const char *nspace, const char *name, const char **atts)
{
    ne_propfind_handler *pfh = userdata;
    struct resource *r = ne_propfind_current_private(pfh);
    int state = ne_xml_mapid(ls_idmap, NE_XML_MAPLEN(ls_idmap),
                             nspace, name);

    if (r == NULL || 
        !((parent == NE_207_STATE_PROP && state == ELM_resourcetype) ||
          (parent == ELM_resourcetype && state == ELM_collection)))
        return NE_XML_DECLINE;

    if (state == ELM_collection) {
	NE_DEBUG(NE_DBG_HTTP, "This is a collection.\n");
	r->type = resr_collection;
    }

    return state;
}

void free_resource(struct resource *res)
{
    if (res->uri) ne_free(res->uri);
    if (res->error_reason) ne_free(res->error_reason);
    free(res);
}

void free_resource_list(struct resource *res)
{
    struct resource *next;
    for (; res != NULL; res = next) {
	next = res->next;
	free_resource(res);
    }
}

static void *create_private(void *userdata, const ne_uri *uri)
{
    return ne_calloc(sizeof(struct resource));
}

int fetch_resource_list(ne_session *sess, const char *uri,
			 int depth, int include_target,
			 struct resource **reslist)
{
    ne_propfind_handler *pfh = ne_propfind_create(sess, uri, depth);
    int ret;
    struct fetch_context ctx = {0};
    
    *reslist = NULL;
    ctx.list = reslist;
    ctx.target = uri;
    ctx.include_target = include_target;

    ne_xml_push_handler(ne_propfind_get_parser(pfh), 
                        ls_startelm, NULL, NULL, pfh);

    ne_propfind_set_private(pfh, create_private, NULL, NULL);

    ret = ne_propfind_named(pfh, ls_props, results, &ctx);

    ne_propfind_destroy(pfh);

    /* The listing says what each member is, and something is
     * usually about to ask.  Expanding a remote wildcard used to
     * throw this away and then make one PROPFIND per member to
     * learn it again. */
    if (ret == NE_OK) {
        const struct resource *res;

        for (res = *reslist; res != NULL; res = res->next)
            restype_remember(res->uri, res->type);
    }

    return ret;
}
