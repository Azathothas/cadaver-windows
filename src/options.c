/* 
   cadaver, command-line DAV client
   Copyright (C) 1999-2001, Joe Orton <joe@manyfish.co.uk>
                                                                     
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

/* Options handling */

#include "config.h"

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <stdio.h>

#include <ne_request.h>
#include <ne_utils.h>
#include <ne_alloc.h>
#include <ne_string.h> /* ne_strncasecmp */
#include <ne_basic.h> /* NE_DEPTH_* */

#include "common.h"
#include "cadaver.h"
#include "options.h"
#include "output.h"
#include "i18n.h"

static void set_debug(const char *new);
static void unset_debug(const char *new);
static void disp_debug(char *buf, size_t len);

static void set_lockscope(const char *new);
static void unset_lockscope(const char *new);
static void disp_lockscope(char *buf, size_t len);

static void set_lockdepth(const char *new);
static void unset_lockdepth(const char *new);
static void disp_lockdepth(char *buf, size_t len);

static void set_clobber_option(const char *new);
static void unset_clobber(const char *new);
static void disp_clobber(char *buf, size_t len);

static void set_searchdepth(const char *new);
static void unset_searchdepth(const char *new);
static void disp_searchdepth(char *buf, size_t len);

/* Option holders */

static int enable_expect, presume_utf8, overwrite, quiet, searchall, system_proxy;

enum ne_lock_scope lockscope;
int lockdepth;
enum clobber_mode clobber;

/* search option global */
int searchdepth;
static int keepalive = 1;

static struct option {
    const char *name;
    enum option_id id;
    void *holder; /* for bool + string options */
    enum {
	opt_bool,
	opt_string,
	opt_handled
    } type;
    /* for handled options.  display() fills a buffer rather than
     * printing, so that `set' can record the value for --json as well
     * as show it. */
    void (*set)(const char *);
    void (*unset)(const char *);
    void (*display)(char *buf, size_t len);
    /* for all options */
    const char *help;
    /* for handled options */
    const char *handle_help;
} options[] = {
#define B(x,v,h) { #x, opt_##x, v, opt_bool, NULL, NULL, NULL, h, NULL }
    /* Booleans */
    B(tolerant, &tolerant, "Tolerate non-WebDAV collections"),
    B(overwrite, &overwrite, "Enable overwrite (e.g. on copy/move operations"),
    B(expect100, &enable_expect, "Enable use of 'Expect: 100-continue' header"),
    B(utf8, &presume_utf8, "Presume filenames etc are UTF-8 encoded"),
    B(quiet, &quiet, "Whether to display connection status messages"),
    B(searchall, &searchall, "Whether to search and display all props including dead props"),
    B(keepalive, &keepalive, "Whether to enable persistent connections when opening a connection"),
#define S2(x,y, h) { x, y, NULL, opt_string, NULL, NULL, NULL, h, NULL }
#define S(x,h) S2(#x, opt_##x, h)
    S(lockowner, "Lock owner URI"),
    S(lockstore, "Persistent lock storage file"),
    S(editor, "Editor to use with `edit' command"),
    S2("client-cert", opt_clicert,
       "Client certificate to use for SSL connections."),
    S2("client-cert-uri", opt_clicert_uri,
       "Client certificate URI to use for SSL connections."),
    S(namespace, "Namespace to use for propset/propget commands."),
    S(pager, "Command to run for less/more commands."),
    { "lsformat", opt_lsformat, NULL, opt_string, NULL, NULL, NULL,
      "Layout of one line of an `ls' listing",
      "A conversion is a %, an optional - for left alignment, an optional\n"
      "field width, and one letter:\n"
      "  %n  name          %h  full path        %%  a literal %\n"
      "  %s  size in bytes %S  size rounded, powers of 1024\n"
      "  %d  modified, local time    %D  modified, ISO 8601 UTC\n"
      "  %t  Coll: or Ref: %T  collection, resource, reference\n"
      "  %e  * if executable         %v  > or < if version-controlled\n"
      "Anything else is written as it stands.  A member the server could\n"
      "not report on keeps its own line, because it has a status and a\n"
      "reason and none of these.  The default is\n"
      "  %5t %v%e%-29n %10s  %d"
    },
    S(proxy, "Hostname of proxy server"),
    { "proxy-port", opt_proxy_port, NULL, opt_string, NULL, NULL, NULL,
      "Port to use on proxy server", NULL },
    B(systemproxy, &system_proxy, "Use system-wide proxy configuration"),
    S(searchorder,  "Search ascending props options"),
    S(searchdorder, "Search descending props options"),
#undef S
#undef B
    { "debug", opt_debug, NULL, opt_handled,
      set_debug, unset_debug, disp_debug, "Debugging options",
      "The debug value is a list of comma-separated keywords.\n"
      "Valid keywords are: socket, http, xml, httpauth, cleartext."
    },
    { "clobber", opt_clobber, NULL, opt_handled,
      set_clobber_option, unset_clobber, disp_clobber,
      "What `get' does when the local file exists",
      "The clobber value must be ask, which prompts for another name and is\n"
      "the default, yes to overwrite the file, or no to leave it and fail.\n"
      "Asking cannot work in a script: with the input at end of file the\n"
      "prompt reads nothing and the resource is not downloaded."
    },
    { "lockscope", opt_lockscope, NULL, opt_handled,
      set_lockscope, unset_lockscope, disp_lockscope, "Lock scope options",
      "The lockscope value must be one of two valid keywords: exclusive or shared."
    },
    { "lockdepth", opt_lockdepth, NULL, opt_handled,
      set_lockdepth, unset_lockdepth, disp_lockdepth, "Lock depth options",
      "The lockdepth value must be 0 or infinity."
    },

    /* Several options for search */
    { "searchdepth", opt_searchdepth, NULL, opt_handled,
      set_searchdepth, unset_searchdepth, disp_searchdepth, "Search depth options",
      "The searchdepth value must be 0, 1 or infinity."
    },

    { NULL, 0 }
};

static const struct {
    const char *name;
    int val;
} debug_map[] = {
    { "xml", NE_DBG_XML },
    { "xmlparse", NE_DBG_XMLPARSE },
    { "http", NE_DBG_HTTP },
    { "socket", NE_DBG_SOCKET },
    { "ssl", NE_DBG_SSL },
    { "httpauth", NE_DBG_HTTPAUTH },
    { "httpbody", NE_DBG_HTTPBODY },
    { "cleartext", NE_DBG_HTTPPLAIN },
    { "files", DEBUG_FILES },
    { "locks", NE_DBG_LOCKS },
    { NULL, 0 }
};

static void display_options(void) 
{
    int n;
    out_printf("Options:\n");
    for (n = 0; options[n].name != NULL; n++) {
	int *val = (int *)options[n].holder;
	switch (options[n].type) {
	case opt_bool:
	    out_printf(" %15s: %s\n", options[n].name, *val?"on":"off");
            res_option(options[n].name, *val ? "on" : "off");
	    break;
	case opt_string:
	    if (options[n].holder == NULL) {
		out_printf(" %15s: unset\n", options[n].name);
                res_option(options[n].name, NULL);
	    } else {
		out_printf(" %15s: %s\n", options[n].name,
		       (char *)options[n].holder);
                res_option(options[n].name, (char *)options[n].holder);
	    }
	    break;
	case opt_handled: {
            char buf[512];

            (*options[n].display)(buf, sizeof buf);
	    out_printf(" %15s: %s\n", options[n].name, buf);
            res_option(options[n].name, buf);
	    break;
        }
	}
    }
}

static void do_debug(const char *set, int setit)
{
    char *opts, *pnt;

    if (!setit && !set) {
	ne_debug_mask = 0;
	return;
    }
    
    pnt = opts = ne_strdup(set);

    do {
	int d, got = 0;
	char *opt = ne_token(&pnt, ',');

	for (d = 0; debug_map[d].name != NULL; d++) {
	    if (strcasecmp(opt, debug_map[d].name) == 0) {
		if (setit) {
		    ne_debug_mask |= debug_map[d].val ;
		} else {
		    ne_debug_mask &= ~debug_map[d].val;
		}
		got = 1;
	    }
	}

	if (!got) {
	    out_printf("Debug option %s unknown.\n", opt);
            cmd_failed(_("no such debug option"));
	}
    } while (pnt != NULL);
    
    free(opts);
}

static void set_debug(const char *set)
{
    do_debug(set, 1);
}

static void unset_debug(const char *s)
{
    do_debug(s, 0);
}

static void disp_debug(char *buf, size_t len)
{
    int n, flag = 0;
    size_t used = 1;

    ne_strnzcpy(buf, "{", len);
    for (n = 0; debug_map[n].name != NULL; n++) {
	if (ne_debug_mask & debug_map[n].val) {
            ne_snprintf(buf + used, len - used, "%s%s",
                        flag++ ? "," : "", debug_map[n].name);
            used = strlen(buf);
	}
    }
    ne_snprintf(buf + used, len - used, "}");
}

int set_clobber(const char *what)
{
    if (what == NULL) {
        clobber = clobber_ask;
    }
    else if (strcasecmp(what, "ask") == 0) {
        clobber = clobber_ask;
    }
    else if (strcasecmp(what, "yes") == 0 || strcasecmp(what, "on") == 0) {
        clobber = clobber_yes;
    }
    else if (strcasecmp(what, "no") == 0 || strcasecmp(what, "off") == 0) {
        clobber = clobber_no;
    }
    else {
        fprintf(stderr, _("cadaver: clobber must be ask, yes or no, "
                          "not `%s'.\n"), what);
        return 1;
    }

    return 0;
}

static void set_clobber_option(const char *set)
{
    if (set_clobber(set)) {
        out_printf(_("Invalid value for clobber. Try `set clobber' for more "
                     "info.\n"));
        cmd_failed(_("invalid value"));
    }
}

static void unset_clobber(const char *s)
{
    clobber = clobber_ask;
}

static void disp_clobber(char *buf, size_t len)
{
    ne_strnzcpy(buf, clobber == clobber_yes ? "yes" :
                     clobber == clobber_no ? "no" : "ask", len);
}

static void set_lockscope(const char *set)
{
    if (strcasecmp(set,"exclusive") == 0)
	lockscope = ne_lockscope_exclusive;
    else if (strcasecmp(set,"shared") == 0)
	lockscope = ne_lockscope_shared;
    else {
	out_printf("Invalid value for lockscope. Try `set lockscope' for more info.\n");
        cmd_failed(_("invalid value"));
    }
}

static void unset_lockscope(const char *s)
{
    lockscope = ne_lockscope_exclusive;
}

static void disp_lockscope(char *buf, size_t len)
{
    ne_strnzcpy(buf, lockscope == ne_lockscope_exclusive ? "exclusive" :
                     lockscope == ne_lockscope_shared ? "shared" :
                     "illegal value", len);
}

static void set_lockdepth(const char *set)
{
    if (strcmp(set, "0") == 0 ||
	strcasecmp(set, "zero") == 0)
	lockdepth = NE_DEPTH_ZERO;
    else if (strcasecmp(set, "infinite") == 0 ||
	     strcasecmp(set, "infinity") == 0)
	lockdepth = NE_DEPTH_INFINITE;
    else {
	out_printf("Invalid value for lockdepth. Try `set lockdepth' for more info.\n");
        cmd_failed(_("invalid value"));
    }
}

static void unset_lockdepth(const char *s)
{
    lockdepth = NE_DEPTH_INFINITE;
}

static void disp_lockdepth(char *buf, size_t len)
{
    ne_strnzcpy(buf, lockdepth == NE_DEPTH_ZERO ? "zero" :
                     lockdepth == NE_DEPTH_INFINITE ? "infinite" :
                     "illegal value", len);
}


static void set_searchdepth(const char *set)
{
    if (strcmp(set, "0") == 0 ||
	strcasecmp(set, "zero") == 0)
	searchdepth = NE_DEPTH_ZERO;
    else if (strcmp(set, "1") == 0 ||
	     strcasecmp(set, "one") == 0)
	searchdepth = NE_DEPTH_ONE;
    else if (strcasecmp(set, "infinite") == 0 ||
	     strcasecmp(set, "infinity") == 0)
	searchdepth = NE_DEPTH_INFINITE;
    else {
	/* Anything unrecognised used to become infinity, so a typo set
	 * the widest possible search and said nothing.  Every other
	 * option with a fixed set of values reports one it does not
	 * know. */
	out_printf(_("Invalid value for searchdepth. Try `set searchdepth' "
		     "for more info.\n"));
	cmd_failed(_("invalid value"));
    }
}

static void unset_searchdepth(const char *s)
{
    searchdepth = NE_DEPTH_INFINITE;
}

static void disp_searchdepth(char *buf, size_t len)
{
    ne_strnzcpy(buf, searchdepth == NE_DEPTH_ZERO ? "0" :
                     searchdepth == NE_DEPTH_ONE ? "1" : "infinity", len);
}

/* readline's generator convention: `state' is zero on the first call
 * for a given prefix and non-zero on the ones after it, and each call
 * returns the next match or NULL when there are none left.  The command
 * table says which arguments are option names -- `set', `unset' and
 * `describe' -- and this is what turns that into completions. */
char *option_generator(const char *text, int state)
{
    static int n;
    static size_t len;
    const char *name;

    if (!state) {
        n = 0;
        len = strlen(text);
    }

    while ((name = options[n].name) != NULL) {
        n++;
        if (ne_strncasecmp(name, text, len) == 0)
            return ne_strdup(name);
    }

    return NULL;
}

/* A truth word, or -1 if it is not one.  `set' used to refuse a value
 * for a boolean outright, so `set quiet off' failed and the only way to
 * turn one off was `unset quiet' -- although `set' with no argument
 * displays every boolean as on or off, so what it printed could not be
 * typed back in. */
static int parse_bool(const char *value)
{
    static const struct { const char *word; int truth; } words[] = {
        { "on", 1 }, { "off", 0 },
        { "yes", 1 }, { "no", 0 },
        { "true", 1 }, { "false", 0 },
        { "1", 1 }, { "0", 0 },
        { NULL, 0 }
    };
    int n;

    for (n = 0; words[n].word != NULL; n++)
        if (strcasecmp(value, words[n].word) == 0)
            return words[n].truth;

    return -1;
}

static const struct option *find_option(const char *name)
{
    int n;
    
    for (n = 0; options[n].name != NULL; n++)
        if (strcasecmp(options[n].name, name) == 0)
            return &options[n];
    
    return NULL;
}

void execute_set(const char *opt, const char *newv)
{
    if (opt == NULL) {
	display_options();
    } else {
	int n;
	for (n = 0; options[n].name != NULL; n++) {
	    if (strcasecmp(options[n].name, opt) == 0) {
		switch (options[n].type) {
		case opt_bool:
		    if (newv) {
			int truth = parse_bool(newv);

			if (truth < 0) {
			    out_printf(_("%s is a boolean option: give it on "
					 "or off, or no value at all.\n"), opt);
			    cmd_failed(_("that option takes on or off"));
			} else {
			    *(int *)options[n].holder = truth;
			}
		    } else {
			*(int *)options[n].holder = 1;
		    }
		    break;
		case opt_string:
		    if (newv == NULL) {
			out_printf("You must give a new value for %s\n", opt);
                        cmd_failed(_("that option needs a value"));
		    } else {
			char *val = options[n].holder;
			if (val != NULL) {
			    free(val);
			}
			options[n].holder = ne_strdup(newv);
		    }
		    break;
		case opt_handled:
		    if (!newv) {
			out_printf("%s must be given a value:\n%s\n", opt,
				options[n].handle_help);
                        cmd_failed(_("that option needs a value"));
		    } else {
			(*options[n].set)(newv);
		    }
		    break;
		}
		return;
	    }
	}
	out_printf("Unknown option: %s.\n", opt);
        cmd_failed(_("no such option"));
    }
}

void execute_unset(const char *opt, const char *newv)
{
    int n;
    for (n = 0; options[n].name != NULL; n++) {
	if (strcasecmp(options[n].name, opt) == 0) {
	    switch (options[n].type) {
	    case opt_bool:
		if (newv != NULL) {
		    out_printf(_("%s is a boolean option: `unset %s' takes no "
				 "value.\n"), opt, opt);
		    cmd_failed(_("that option takes no value"));
		} else {
		    *(int *)options[n].holder = 0;
		}
		break;
	    case opt_string:
		if (newv != NULL) {
		    out_printf(_("%s cannot take a value to unset.\n"), opt);
		    cmd_failed(_("that option takes no value"));
		} else {
		    char *v = options[n].holder;
		    free(v);
		    options[n].holder = NULL;
		}
		break;
	    case opt_handled:
		(*options[n].unset)(newv);
		break;
	    }
	    return;
	}
    }
    /* `set' on an option that does not exist reports a failure, and
     * this used to print the same message and succeed. */
    out_printf(_("Unknown option: %s.\n"), opt);
    cmd_failed(_("no such option"));
}

void execute_describe(const char *name)
{
    const struct option *opt = find_option(name);

    if (opt == NULL) {
        out_printf("Option `%s' not known.\n", name);
        cmd_failed(_("no such option"));
        return;
    }

    out_printf("Option `%s': %s\n", opt->name, opt->help);
    if (opt->handle_help) out_puts_line(opt->handle_help);
}

void *get_option(enum option_id id)
{
    int n;
    for (n = 0; options[n].name != NULL; n++) {
	if (options[n].id == id) {
	    return options[n].holder;
	}
    }
    return NULL;
}

int get_bool_option(enum option_id id)
{
    int *ret = get_option(id);
    if (ret == NULL)
	return 0;
    return *ret;
}

void set_bool_option(enum option_id id, int truth)
{
    int *opt = get_option(id);
    if (opt != NULL) {
	*opt = truth;
    }
}

void set_option(enum option_id id, void *newval)
{
    int n;
    for (n = 0; options[n].name != NULL; n++) {
	if (options[n].id == id) {
	    options[n].holder = newval;
	    return;
	}
    }
}
