/* 
   cadaver, command-line DAV client
   Copyright (C) 1999-2025, Joe Orton <joe@manyfish.co.uk>,
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

/* Some UI guidelines:
 *  1. Use dispatch, or out_* to do UI. This makes it CONSISTENT.
 *  2. Get some feedback on the screen before making any requests
 *     to the server. Tell them what is going on: remember, on a slow
 *     link or a loaded server,5~ a request can take AGES to return.
 */

#include "config.h"

#include <sys/types.h>

#include <sys/stat.h>
#include <dirent.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <fcntl.h>
#include <errno.h>

/* readline requires FILE *, silly thing */
#include <stdio.h>

#ifdef HAVE_READLINE_H
#include <readline.h>
#elif defined(HAVE_READLINE_READLINE_H)
#include <readline/readline.h>
#endif

#ifdef HAVE_ICONV
#include <iconv.h>
#endif

#include <ne_request.h>
#include <ne_basic.h>
#include <ne_auth.h> /* for http_forget_auth */
#include <ne_redirect.h>
#include <ne_props.h>
#include <ne_string.h>
#include <ne_uri.h>
#include <ne_locks.h>
#include <ne_alloc.h>
#include <ne_dates.h>

#include "i18n.h"
#include "basename.h"
#include "system.h"
#include "cadaver.h"
#include "commands.h"
#include "options.h"
#include "utils.h"

/* Local variables */
int child_running; /* true when we have a child running */

/* Command alias mappings */
const static struct {
    enum command_id id;
    const char *name;
} command_names[] = {
    /* The direct mappings */
#define C(n) { cmd_##n, #n }
    C(ls), C(cd), C(quit), C(open), C(logout), C(close), C(set), C(unset), 
    C(pwd), C(help), C(put), C(get), C(mkcol), C(delete), C(move), C(copy),
    C(less), C(cat), C(lpwd), C(lcd), C(lls), C(echo), C(quit), C(about),
    C(rename), C(head), C(resumeget),
    C(mget), C(mput), C(rmcol), C(lock), C(unlock), C(discover), C(steal),
    C(chexec), C(showlocks), C(version), C(propget), C(propset), C(propdel),
    C(describe), C(search),
    C(version), C(checkin), C(checkout), C(uncheckout), C(history),
    C(label), 
#if 0
C(propedit), 
#endif
C(propnames), C(edit),
#undef C
    /* And now the real aliases */
    { cmd_less, "more" }, { cmd_mkcol, "mkdir" }, 
    { cmd_delete, "rm" }, { cmd_copy, "cp"}, { cmd_move, "mv" }, 
    { cmd_help, "h" }, { cmd_help, "?" },
    { cmd_quit, "exit" }, { cmd_quit, "bye" },
    { cmd_unknown, NULL }
};    

extern const struct command commands[]; /* prototype */

/* Output character encoding from the locale. */
const char *out_charset;

#ifdef HAVE_ICONV

enum conv_mode { TO_UTF8, FROM_UTF8 };

static char *run_iconv(const char *instr, enum conv_mode mode)
{
    static iconv_t from_cd, to_cd;
    iconv_t cd;
    char outbuf[BUFSIZ], *inptr = (char *)instr, *outptr = outbuf;
    size_t inbytes = strlen(instr), outbytes = sizeof outbuf, ret;

    cd = mode == TO_UTF8 ? to_cd : from_cd;
    if (cd) {
        (void) iconv(cd, NULL, NULL, NULL, NULL);
    }
    else {
        if (mode == TO_UTF8)
            cd = to_cd = iconv_open("UTF-8", out_charset);
        else
            cd = from_cd = iconv_open(out_charset, "UTF-8");

        if (cd == (iconv_t)-1) {
            fprintf(stderr, _("cadaver: Cannot convert from %s to UTF-8: %s\n"),
                    out_charset, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    ret = iconv(cd, &inptr, &inbytes, &outptr, &outbytes);
    if (ret == (size_t) -1) {
        if (!in_completion)
            fprintf(stderr, _("cadaver: Warning: Character(s) could not "
                              "be translated to %s\n"),
                    mode == TO_UTF8 ? "UTF-8" : out_charset);
        /* Make space for trailing "[?]". */
        while (outbytes < 3) {
            outptr -= 1;
            outbytes += 1;
        }

        *outptr++ = '[';
        *outptr++ = '?';
        *outptr++ = ']';
    }

    return ne_strndup(outbuf, outptr-outbuf);
}
#endif

/* Convert native string to UTF-8, returns malloc-allocated. */
static char *utf8_from_native(const char *native)
{
#ifdef HAVE_ICONV
    if (!get_bool_option(opt_utf8)) {
        return run_iconv(native, TO_UTF8);
    }
#endif

    return ne_strdup(native);
}

/* Convert UTF-8 to native string, malloc-allocated. */
static char *native_from_utf8(const char *native)
{
#ifdef HAVE_ICONV
    if (!get_bool_option(opt_utf8)) {
        return run_iconv(native, FROM_UTF8);
    }
#endif

    return ne_strdup(native);
}

/* Return a string for bullet lists. */
static const char *bullet_str(void)
{
    return get_bool_option(opt_utf8) ? N_("•") : "-";
}

/* The actual commands */
#ifdef HAVE_LIBREADLINE

/* Command name generator for readline.
 * Copied almost verbatim from the info doc */
char *command_generator(const char *text, int state)
{
    static int i, len;
    const char *name;

    if (!state) {
	i = 0;
	len = strlen(text);
    }

    while ((name = command_names[i].name) != NULL) {
	i++;
	if (strncmp(name, text, len) == 0) {
	    return ne_strdup(name);
	}
    }
    return NULL;
}

#endif

/* Which argument the point is in: 0 while still on the command name, 1
 * for the first argument.  Counts the runs of non-space before it. */
int argument_index(const char *line, int start)
{
    int n = 0, in_word = 0, i;

    for (i = 0; i < start && line[i]; i++) {
        if (line[i] == ' ' || line[i] == '\t')
            in_word = 0;
        else if (!in_word) {
            in_word = 1;
            n++;
        }
    }

    return n;
}

/* What argument `argno' of `cmd' completes to. */
enum command_scope completion_scope(const struct command *cmd, int argno)
{
    size_t len, at;

    if (cmd == NULL || cmd->completes == NULL || argno < 1)
        return parmscope_none;

    len = strlen(cmd->completes);
    if (len == 0) return parmscope_none;

    at = (size_t)argno - 1;
    if (at >= len) at = len - 1;

    switch (cmd->completes[at]) {
    case 'r': return parmscope_remote;
    case 'l': return parmscope_local;
    case 'o': return parmscope_option;
    default:  return parmscope_none;
    }
}

static void execute_logout(void)
{
    ne_forget_auth(session.sess);
}

const struct command *get_command(const char *name)
{
    int n, m;
    for(n = 0; command_names[n].name != NULL; n++) {
	if (strcasecmp(command_names[n].name, name) == 0) {
	    for(m = 0; commands[m].id != cmd_unknown; m++) {
		if (commands[m].id == command_names[n].id)
		    return &commands[m];
	    }
	    return NULL;
	}
    }
    return NULL;
}


static void dispatch(const char *verb, const char *filename, 
		     int (*func)(ne_session *, const char *), const char *arg)
{
    out_start(verb, filename);
    out_result((*func)(session.sess, arg));
}

/* The lock owner, wrapped in the <href> element neon puts in the LOCK
 * body.  neon concatenates lock->owner into that body verbatim -- there
 * is no ne_xml_escape to call -- so an owner containing an ampersand or
 * an angle bracket would make the request body ill-formed and the
 * server answer 400.  Escaping it here is what keeps `set lockowner
 * "foo&bar"; lock' working. */
char *getowner(void)
{
    char *owner = get_option(opt_lockowner);
    ne_buffer *buf;

    if (!owner) return NULL;

    buf = ne_buffer_create();
    ne_buffer_czappend(buf, "<href>");
    xml_escape(buf, owner);
    ne_buffer_czappend(buf, "</href>");

    return ne_buffer_finish(buf);
}

/* Resolve path, appending a trailing slash if the resource is a
 * collection.  If 'type' is non-NULL, *type is set to what the resource
 * turned out to be -- resr_error meaning it could not be found, which
 * is not the same as its being a plain resource. */
static char *uri_resolve_native_true(const char *path,
                                     enum resource_type *type)
{
    char *uri_path = uri_resolve_native(path);
    enum resource_type restype = getrestype(uri_path);
    int is_coll = restype == resr_collection;
    const ne_uri *redir = ne_redirect_location(session.sess);

    NE_DEBUG(DEBUG_FILES, "cadaver: Resolve true [%s] -> [%s]\n", path, uri_path);

    /* Special case: if the destination redirects to a location with a
     * trailing slash on the same origin server, we follow the
     * redirection and use the destination location here since this is
     * what will typically happen for collections. */
    if (!is_coll && redir && redir->path
        && ne_path_has_trailing_slash(redir->path)) {
        ne_uri uri;

        memset(&uri, 0, sizeof uri);
        ne_fill_server_uri(session.sess, &uri);
        uri.path = redir->path;

        /* ### FIXME: this is a neon bug, ne_uri_cmp() fails to follow
         * normalisation/comparison rules */
        if (uri.port == ne_uri_defaultport(uri.scheme))
            uri.port = 0;

        if (ne_uri_cmp(redir, &uri) == 0) {
            ne_free(uri_path);
            uri_path = ne_strdup(redir->path);
            restype = getrestype(uri_path);
            is_coll = restype == resr_collection;
            NE_DEBUG(DEBUG_FILES, "cadaver: Redirected to %s, a %scollection.\n",
                     uri_path, is_coll ? "" : "non-");
        }

        uri.path = NULL;
        ne_uri_free(&uri);
    }

    if (type) *type = restype;

    if (is_coll && !ne_path_has_trailing_slash(uri_path)) {
        char *tmp = ne_concat(uri_path, "/", NULL);
        ne_free(uri_path);
        uri_path = tmp;
    }

    return uri_path;
}

static const char *get_lockscope(enum ne_lock_scope s)
{
    switch (s) {
    case ne_lockscope_exclusive: return _("exclusive");
    case ne_lockscope_shared: return _("shared");
    default: return _("unknown");
    }
}

static const char *get_locktype(enum ne_lock_type t)
{
    if (t == ne_locktype_write) {
	return _("write");
    } else {
	return _("unknown");
    }
}

static const char *get_timeout(long t)
{
    static char buf[128];
    switch (t) {
    case NE_TIMEOUT_INFINITE: return _("infinite");
    case NE_TIMEOUT_INVALID: return _("invalid");
    default:
	sprintf(buf, _("%ld seconds"), t);
	return buf;
    }
}

static const char *get_depth(int d)
{
    switch (d) {
    case NE_DEPTH_INFINITE:
	return _("infinity");
    case 0:
	/* TODO: errr... do I need to i18n'ize numeric strings??? */
	return "0";
    case 1:
	return "1";
    default:
	return _("invalid");
    }
}

static void print_uri(const ne_uri *uri)
{
    char *str = ne_uri_unparse(uri);
    out_printf("%s", str);
    free(str);
}

static void print_lock(const struct ne_lock *lock)
{
    char *uri = ne_uri_unparse(&lock->uri);

    res_lock(lock);

    out_printf(_("Lock token <%s>:\n"
	     "  Depth %s on `%s'\n"
	     "  Scope: %s  Type: %s  Timeout: %s\n"
	     "  Owner: %s\n"), 
	   lock->token, get_depth(lock->depth), uri,
	   get_lockscope(lock->scope),
	   get_locktype(lock->type), get_timeout(lock->timeout),
	   lock->owner?lock->owner:_("(none)"));
    free(uri);
}

/* What a lock discovery turned up.  The two counts are kept apart
 * because a resource the server could not report on is not the same
 * answer as a resource with no locks on it. */
struct discovery {
    int locks;
    int failures;
};

static void discover_result(void *userdata, const struct ne_lock *lock,
                            const ne_uri *uri,
                            const ne_status *status)
{
    struct discovery *found = userdata;
    if (lock) {
	if (found->locks == 0) {
	    out_printf("\n");
	}
	print_lock(lock);
	found->locks += 1;
    } else {
	out_printf(_("Failed on %s: %d %s\n"), uri->path,
	       status->code, status->reason_phrase);
        cmd_failed(status->reason_phrase);
        found->failures += 1;
    }
}

static void steal_result(void *userdata, const struct ne_lock *lock,
			 const ne_uri *uri,
                         const ne_status *status)
{
    struct discovery *found = userdata;
    if (lock != NULL) {
	if (found->locks == 0) {
	    out_printf("\n");
	}
        res_lock(lock);
	print_uri(&lock->uri);
	out_printf(": <%s>\n", lock->token);
	ne_lockstore_add(session.locks, ne_lock_copy(lock));
	found->locks += 1;
    } else {
	out_printf(_("Failed on %s: %d %s\n"), uri->path,
	       status->code, status->reason_phrase);
        cmd_failed(status->reason_phrase);
        found->failures += 1;
    }
}

static void do_discover(const char *path, const char *mesg,
			ne_lock_result result_cb)
{
    char *uri_path = uri_resolve_native(path);
    struct discovery found = {0, 0};
    int ret;

    out_start(mesg, path);
    ret = ne_lock_discover(session.sess, uri_path, result_cb, &found);
    switch (ret) {
    case NE_OK:
        /* Three outcomes, which used to read as two.  A resource the
         * server could not report on has already said so above and is
         * not an answer of "no locks"; a server that reported an empty
         * lockdiscovery has answered the question. */
        if (found.failures) {
            out_fail(_("%d of the resources asked about could not be "
                       "reported on.\n"), found.failures);
        }
        else if (found.locks == 0) {
	    out_success_as(_("the server reported no locks.\n"));
	}
        else {
            out_done();
        }
	break;
    default:
	out_result(ret);
	break;
    }
    ne_free(uri_path);
}

static void execute_discover(const char *path)
{
    do_discover(path, _("Discovering locks on"), discover_result);
}

static void execute_steal(const char *path)
{
    do_discover(path, _("Stealing locks on"), steal_result);
}

static void execute_showlocks(void)
{
    int count = 0;
    struct ne_lock *lk;
    
    for (lk = ne_lockstore_first(session.locks); lk != NULL;
	 lk = ne_lockstore_next(session.locks), count++) {
        print_lock(lk);
    }

    if (count == 0) {
	out_printf(_("No owned locks.\n"));
    }
}

static void execute_lock(const char *path)
{
    char *uri_path;
    struct ne_lock *lock;
    enum resource_type restype;
    int iscoll;

    uri_path = uri_resolve_native_true(path, &restype);
    iscoll = restype == resr_collection;
    if (iscoll)
        out_start(_("Locking collection"), path);
    else
	out_start(_("Locking"), path);

    lock = ne_lock_create();
    lock->scope = lockscope;
    lock->owner = getowner();
    lock->depth = iscoll ? lockdepth : NE_DEPTH_ZERO;
    ne_fill_server_uri(session.sess, &lock->uri);
    lock->uri.path = uri_path;

    if (out_handle(ne_lock(session.sess, lock))) {
	/* success: remember the lock. */
        /* And report it: the token is what a script needs from this
         * command, and `showlocks' was the only way to get it. */
        res_lock(lock);
	ne_lockstore_add(session.locks, lock);
    }
    else {
	/* Otherwise, throw it away */
	ne_lock_destroy(lock);
    }
}

static void execute_unlock(const char *res)
{
    struct ne_lock *lock;
    ne_uri uri = session.uri; /* shallow copy */

    uri.path = uri_resolve_native_true(res, NULL);

    out_start(_("Unlocking"), res);
    lock = ne_lockstore_findbyuri(session.locks, &uri);
    if (!lock) {
	lock = ne_lock_create();
	lock->token = readline(_("Enter locktoken: "));
	if (!lock->token || strlen(lock->token) == 0) {
            /* This used to end the command in silence: no message, no
             * failure line, nothing in the transcript to tell it from
             * success.  In a script the prompt reads end of input, so
             * it was the usual outcome rather than a rare one. */
            out_fail(_("no lock is held on it, and no lock token was "
                       "given.\n"));
	    goto unlock_fail;
	}
	ne_fill_server_uri(session.sess, &lock->uri);
	lock->uri.path = ne_strdup(uri.path);
    } 
    else {
	/* remove the lock from the lockstore */
	ne_lockstore_remove(session.locks, lock);
    }

    out_result(ne_unlock(session.sess, lock));

unlock_fail:
    ne_free(uri.path);
    ne_lock_destroy(lock);
}

static void execute_mkcol(const char *path)
{
    char *uri_path = uri_resolve_native_coll(path);
    dispatch(_("Creating"), path, ne_mkcol, uri_path);
    ne_free(uri_path);
}

static int all_iterator(void *userdata, const ne_propname *pname,
			 const char *value, const ne_status *status)
{
    char *nnspace = native_from_utf8(pname->nspace);
    char *nname = native_from_utf8(pname->name);
    if (value != NULL) {
	char *nval = native_from_utf8(value);
        char *sval = ne_shave(nval, " \r\n\t");
	out_printf(_("%s %s%s = %s\n"), bullet_str(), nnspace, nname, sval);
        res_property(pname->nspace, pname->name, sval, 0);
	ne_free(nval);
    }
    else if (status) {
	out_printf(_("-- failed for %s%s: %s\n"), nnspace, nname, status->reason_phrase);
        cmd_failed(status->reason_phrase);
    }
    ne_free(nnspace);
    ne_free(nname);
    return 0;
}

static void pget_results(void *userdata, const ne_uri *uri, 
			 const ne_prop_result_set *set)
{
    ne_propname *pname = userdata;
    const char *value;
    char *nname;

    out_printf("\n");

    if (userdata == NULL) {
	/* allprop */
	ne_propset_iterate(set, all_iterator, NULL);
	return;
    }
    
    nname = native_from_utf8(pname->name);

    value = ne_propset_value(set, pname);
    if (value != NULL) {
	char *nval = native_from_utf8(value);
	out_printf(_("Value of %s is: %s\n"), nname, nval);
	ne_free(nval);
    }
    else {
	const ne_status *status = ne_propset_status(set, pname);
	
	if (status) {
	    out_printf(_("Could not fetch property: %d %s\n"),
		   status->code, status->reason_phrase);
            cmd_failed(status->reason_phrase);
	} else {
            cmd_failed(_("the server returned no result for the property"));
	    out_printf(_("Server did not return result for %s\n"),
		   nname);
	}
    } 

    ne_free(nname);
}

/* Change property 'name' on 'uri': if value is non-NULL, set property
 * to have new value, else delete it. */
static void propop(const char *descr, const char *path,
		   const char *name, const char *value)
{
    ne_proppatch_operation ops[2];
    ne_propname pname;
    char *uri_path = uri_resolve_native(path);
    char *val_utf = NULL, *name_utf = utf8_from_native(name);

    ops[0].name = &pname;
    if (value) {
	ops[0].type = ne_propset;
	ops[0].value = val_utf = utf8_from_native(value);
    }
    else {
	ops[0].type = ne_propremove;
    }
    ops[1].name = NULL;
    
    pname.name = name_utf;
    pname.nspace = get_option(opt_namespace);

    out_start(descr, path);
    out_handle(ne_proppatch(session.sess, uri_path, ops));

    if (val_utf) ne_free(val_utf);
    ne_free(name_utf);
    ne_free(uri_path);
}    


static void execute_propset(const char *res, const char *name, const char *value)
{
    propop(_("Setting property on"), res, name, value);
}

static void execute_propdel(const char *res, const char *name)
{
    propop(_("Deleting property on"), res, name, NULL);
}

static void execute_propget(const char *res, const char *name)
{
    ne_propname pnames[2] = {{NULL}, {NULL}};
    char *uri_path = uri_resolve_native(res), *uname = NULL;
    ne_propname *props;
    int ret;
    
    if (name == NULL) {
	props = NULL;
    } else {
	pnames[0].nspace = (const char *)get_option(opt_namespace);
	pnames[0].name = uname = utf8_from_native(name);
	props = pnames;
    }

    out_start(_("Fetching properties for"), res);
    ret = ne_simple_propfind(session.sess, uri_path, NE_DEPTH_ZERO, props,
                             pget_results, props);

    if (ret != NE_OK) {
	out_result(ret);
    }
    else {
        /* The answer has already been printed by pget_results(); this
         * closes the operation so that it carries the request it
         * made. */
        out_done();
    }

    if (uname) ne_free(uname);
    ne_free(uri_path);
}

/* `propget' and `head' mark each line with the bullet; this used to use
 * a plain space, so the three listings did not read alike.  Taken from
 * upstream pull request #74. */
static int propname_iterator(void *userdata, const ne_propname *pname,
			     const char *value, const ne_status *st)
{
    out_printf("\n%s %s%s", bullet_str(), pname->nspace, pname->name);
    res_property(pname->nspace, pname->name, NULL, 0);
    return 0;
}

static void propname_results(void *userdata, const ne_uri *uri, 
			     const ne_prop_result_set *pset)
{
    ne_propset_iterate(pset, propname_iterator, NULL);
}

static void execute_propnames(const char *path)
{
    char *uri_path = uri_resolve_native(path);
    int ret;
    out_start_raw(_("Fetching property names for %s:"), path);
    if ((ret = ne_propnames(session.sess, uri_path, NE_DEPTH_ZERO,
                            propname_results, NULL)) != NE_OK) {
        out_result(ret);
    }
    else {
        out_putchar('\n');
        out_done();
    }
    ne_free(uri_path);
}

static void remove_locks(const char *p, int depth)
{
    struct ne_lock *lk;
    ne_uri sought;
    
    memset(&sought, 0, sizeof(sought));
    ne_fill_server_uri(session.sess, &sought);
    sought.path = ne_strdup(p);

    do {
	lk = ne_lockstore_findbyuri(session.locks, &sought);
	if (lk) {
	    ne_lockstore_remove(session.locks, lk);
	}
    } while (lk);

    ne_uri_free(&sought);
}

static void execute_delete(const char *path)
{
    enum resource_type restype;
    char *uri_path = uri_resolve_native_true(path, &restype);
    int is_coll = restype == resr_collection;

    out_start_uri(_("Deleting"), uri_path);
    if (is_coll) {
	out_fail(
_("it is a collection resource.\n"
"The `rm' command cannot be used to delete a collection.\n"
"Use `rmcol %s' to delete this collection and ALL its contents.\n"),
               path);
    }
    else {
	if (out_handle(ne_delete(session.sess, uri_path))) {
	    remove_locks(uri_path, 0);
	}
    }
    ne_free(uri_path);
}

static void execute_rmcol(const char *path)
{
    enum resource_type restype;
    char *uri_path = uri_resolve_native_true(path, &restype);
    int is_coll = restype == resr_collection;

    out_start_uri(_("Deleting collection"), uri_path);
    if (!is_coll) {
	out_fail(_("it is not a collection.\n"
		 "The `rmcol' command can only be used to delete collections.\n"
		 "Use `rm %s' to delete this resource.\n"), path);
    }
    else {
	if (out_handle(ne_delete(session.sess, uri_path))) {
            remove_locks(uri_path, NE_DEPTH_INFINITE);
        }
    }
    ne_free(uri_path);
}

/* Converts an input path (an unescaped, native charset, relative
 * path) to a URI path. */
static char *path_native_resolver(const char *native_path, int collection)
{
    ne_uri relative, result;
    char *utf_path = utf8_from_native(native_path), *tmp;

    memset(&relative, 0, sizeof relative);
    relative.path = ne_path_escape(utf_path);
    ne_free(utf_path);

    NE_DEBUG(DEBUG_FILES, "cadaver: Convert native [%s] -> URI [%s]\n",
             native_path, relative.path);

    if (collection && !ne_path_has_trailing_slash(relative.path)) {
        tmp = ne_concat(relative.path, "/", NULL);
        ne_free(relative.path);
        relative.path = tmp;
    }

    ne_uri_resolve(&session.uri, &relative, &result);

    ne_uri_free(&relative);
    tmp = result.path;
    result.path = NULL;
    ne_uri_free(&result);

    NE_DEBUG(DEBUG_FILES, "cadaver: Resolved native [%s] -> URI [%s]\n",
             native_path, tmp);

    return tmp;
}

/* Converts a native path to a URI path. */
char *uri_resolve_native(const char *native)
{
    return path_native_resolver(native, 0);
}

/* Converts a native path for a collection to a URI path, ensuring it
 * has a trailing slash. */
char *uri_resolve_native_coll(const char *native)
{
    return path_native_resolver(native, 1);
}

char *native_path_from_uri(const char *uri_path)
{
    char *utf8 = ne_path_unescape(uri_path);

    /* A path with a malformed escape in it cannot be unescaped, and
     * every caller hands what comes back to a "%s".  Showing the
     * escaped form is worse than showing the unescaped one and better
     * than the alternatives. */
    if (!utf8) return ne_strdup(uri_path);

    if (!get_bool_option(opt_utf8)) {
        char *native = native_from_utf8(utf8);
        ne_free(utf8);
        return native;
    }
    else {
        return utf8;
    }
}

static const char *choose_pager(void)
{
    const char *pager = get_option(opt_pager);

    /* $PAGER and the platform default are both cad_default_pager()'s
     * business; the option set inside cadaver wins over either. */
    return pager ? pager : cad_default_pager();
}

static void execute_less(const char *native)
{
    const char *pager;
    char *uri_path;
    FILE *p;
    int ret;

    if (out_json) {
        out_start(_("Displaying"), native);
        out_fail(_("`less' runs a pager, which writes to standard output; "
                   "with --json that carries the result document. "
                   "Use `get'.\n"));
        return;
    }

    pager = choose_pager();
    out_printf(_("Displaying `%s':\n"), native);

    p = cad_popen_write(pager);
    if (p == NULL) {
	out_printf(_("Error! Could not spawn pager `%s':\n%s\n"), pager,
		 strerror(errno));
        cmd_failed(_("could not spawn the pager"));
        return;
    }

    uri_path = uri_resolve_native(native);
    child_running = true;
    ret = ne_get(session.sess, uri_path, fileno(p));
    if (ret) {
        cad_pclose(p);
        out_result(ret);
    }
    else if ((ret = cad_pclose(p)) != 0) {
        out_printf(_("Warning: Abnormal exit from pager (%d).\n"), ret);
    }
    else {
        /* The pager consumed it; nothing else says the GET worked. */
        out_start(_("Displaying"), native);
        out_success();
    }
    child_running = false;
    ne_free(uri_path);
}

static void execute_cat(const char *native_path)
{
    char *uri_path;
    int ret, mode;

    if (out_json) {
        out_start(_("Fetching"), native_path);
        out_fail(_("`cat' writes the resource to standard output, which "
                   "with --json carries the result document. "
                   "Use `get'.\n"));
        return;
    }

    uri_path = uri_resolve_native(native_path);

    /* ne_get() writes to the descriptor rather than through stdio, so
     * anything buffered has to go out first or it would appear after
     * the resource.  The descriptor is put into binary mode for the
     * duration so that the bytes on the far end of a pipe are the bytes
     * the server sent; on Windows it would otherwise gain a carriage
     * return before every newline. */
    out_flush();
    mode = cad_set_binary(STDOUT_FILENO);

    ret = ne_get(session.sess, uri_path, STDOUT_FILENO);

    cad_set_mode(STDOUT_FILENO, mode);

    if (ret != NE_OK) {
        out_start(_("Fetching"), native_path);
        out_result(ret);
    }
    ne_free(uri_path);
}

/* Execute a copy or move via callback 'cb', given the 'argv' array of
 * length 'argc'.  The present participle and root form of the verb
 * are passed as 'v1' and v2 (this likely doesn't international
 * well?). */
static void do_copymove(int argc, const char *argv[],
			const char *v1, const char *v2,
			void (*cb)(const char *, const char *))
{
    /* We are guaranteed that argc > 2... */
    const char *native_dest = argv[argc-1];
    enum resource_type dest_type;
    int n, dest_is_coll, dest_exists, error;
    char *uri_dest = uri_resolve_native_true(native_dest, &dest_type);
    struct {
        char *src, *dest;
    } *ops;

    /* Whether the destination is there at all.  A name that does not
     * exist yet is not a non-collection, which is what refusing to move
     * a collection onto one used to assume: `mv coll/ newname/' failed
     * with "Refusing to move collection to non-collection" although it
     * is a rename and worked before 0.26.  Upstream issue #54. */
    dest_is_coll = dest_type == resr_collection;
    dest_exists = dest_type != resr_error;

    /* Iterate and build up pairs of (source, dest) paths which will
     * be passed to the callback in turn, validating and failing early
     * if there are any errors. */
    ops = ne_calloc(argc * sizeof *ops);

    for (n = 0, error = 0; !error && n < argc-1; n++) {
        enum resource_type src_type;
        int src_is_coll;

        ops[n].src = uri_resolve_native_true(argv[n], &src_type);
        src_is_coll = src_type == resr_collection;

        /* The (src, dest) paths have now been resolved.  There are
         * four valid cases, plus errors:
         *
         * 1. Both source and destination are collections.
         *    (/foo/, /bar/) must translate to (/foo/, /bar/foo/)
         * 2. Only the destination is a collection.
         *    (/foo.txt, /bar/) must translate to (/foo.txt, /bar/foo.txt)
         * 3. Simplest 'mv a b' case, no translation required.
         * 4. A collection source and a destination that does not exist
         *    yet, which is a rename of the collection.
         */
        if (strcmp(ops[n].src, "/") == 0) {
            out_printf(_("Error: Refusing to %s the server root '/'\n"), v2);
            cmd_failed(_("refusing to act on the server root"));
            error = 1;
        }
        else if (dest_is_coll && src_is_coll) {
            /* Case 1. */
            char *tmp = ne_strndup(ops[n].src, strlen(ops[n].src)-1);
            ops[n].dest = ne_concat(uri_dest, base_name(tmp), NULL);
            ne_free(tmp);
        }
        else if (src_is_coll && dest_exists) {
            /* The destination is there and is not a collection, so
             * this would replace a plain resource with a collection. */
            out_printf(_("Error: Refusing to %s collection '%s' to "
                     "non-collection '%s'\n"), v2, ops[n].src, uri_dest);
            cmd_failed(_("the destination is not a collection"));
            error = 1;
        }
        else if (src_is_coll && argc > 2) {
            /* Several sources need somewhere to put them all. */
            out_printf(_("Error: Refusing to %s collection '%s' to '%s', "
                         "which does not exist\n"), v2, ops[n].src, uri_dest);
            cmd_failed(_("the destination collection does not exist"));
            error = 1;
        }
        else if (src_is_coll) {
            /* Case 4: the destination does not exist, so this is a
             * rename of the collection.  The trailing slash goes on
             * both ends, which is what a server expects of a
             * collection URI. */
            ops[n].dest = ne_path_has_trailing_slash(uri_dest)
                ? ne_strdup(uri_dest) : ne_concat(uri_dest, "/", NULL);
        }
        else if (dest_is_coll) {
            /* Case 2. */
            ops[n].dest = ne_concat(uri_dest, base_name(ops[n].src), NULL);
        }
        else {
            /* Case 3. */
            ops[n].dest = ne_strdup(uri_dest);
        }
    }

    if (!error) {
        for (n = 0; n < argc-1; n++)
            (*cb)(ops[n].src, ops[n].dest);
    }

    for (n = 0; n < argc-1; n++) {
        if (ops[n].src) ne_free(ops[n].src);
        if (ops[n].dest) ne_free(ops[n].dest);
    }
    ne_free(uri_dest);
}

static void simple_move(const char *src, const char *dest) 
{
    out_start_2uri(_("Moving"), src, dest);
    out_result(ne_move(session.sess, get_bool_option(opt_overwrite), src, dest));
}

static void simple_copy(const char *src, const char *dest) 
{
    out_start_2uri(_("Copying"), src, dest);
    out_result(ne_copy(session.sess, get_bool_option(opt_overwrite), 
		       NE_DEPTH_INFINITE, src, dest));
}

static void execute_rename(const char *native_src, const char *native_dest)
{
    enum resource_type src_type;
    char *uri_src = uri_resolve_native_true(native_src, &src_type);
    int src_is_coll = src_type == resr_collection;
    char *uri_dest = uri_resolve_native(native_dest);

    out_start_2uri(_("Renaming"), uri_src, uri_dest);
    if (!src_is_coll && ne_path_has_trailing_slash(uri_dest)) {
        out_fail(_("the source path is not a collection.\n"));
    }
    else {
        out_result(ne_move(session.sess, 0, uri_src, uri_dest));
    }

    ne_free(uri_src);
    ne_free(uri_dest);
}

/* Downloads `native_remote'.  A plain download goes to a temporary file
 * beside the destination and is renamed over it once the whole body has
 * arrived, so that a request which fails part way through leaves neither
 * a truncated file nor an empty one: the old behaviour created the
 * destination before making the request, and one 404 then left a
 * zero-length file that made every retry prompt for a name instead.
 *
 * A resumed download has to append to the file that is already there,
 * so it records the size first and truncates back to it on failure. */
static void do_get(const char *native_remote, const char *native_local, int resume)
{
    char *filename = NULL, *tmpname = NULL, *uri_path;
    ne_content_range range;
    struct cad_finfo info;
    ne_off_t resume_from = 0;
    int fd = -1, ret, exists;

    uri_path = uri_resolve_native(native_remote);

    if (native_local) {
        filename = ne_strdup(native_local);
    }
    else {
        filename = ne_strdup(base_name(native_remote));
    }

    exists = cad_file_info(filename, &info) == 0;

    if (resume) {
        if (!exists) {
            int errnum = errno;
            out_start(_("Resuming download of"), native_remote);
            out_fail(_("cannot resume download to `%s': %s\n"),
                     filename, strerror(errnum));
            goto fail;
        }
        if (!info.is_reg) {
            out_start(_("Resuming download of"), native_remote);
            out_fail(_("cannot resume download to `%s': "
                       "not a regular file.\n"), filename);
            goto fail;
        }

        resume_from = info.size;
        range.start = resume_from;
        range.end = -1;
        range.total = 0;

        fd = open(filename, O_APPEND|O_WRONLY|OPEN_BINARY_FLAGS|O_LARGEFILE);
    }
    else {
        if (exists) {
            switch (clobber) {
            case clobber_no:
                out_start(_("Downloading"), native_remote);
                out_fail(_("the local file `%s' exists, and clobber is set "
                           "to no.\n"), filename);
                goto fail;
            case clobber_yes:
                break;
            case clobber_ask:
            default: {
                char buf[BUFSIZ];
                char *answer;

                ne_snprintf(buf, sizeof buf,
                            _("Enter local filename for `%s': "),
                            native_remote);
                answer = readline(buf);
                if (answer == NULL || strlen(answer) == 0) {
                    if (answer) ne_free(answer);
                    out_start(_("Downloading"), native_remote);
                    out_fail(_("cancelled; the local file `%s' exists and "
                               "no other name was given.\n"), filename);
                    goto fail;
                }
                ne_free(filename);
                filename = answer;
                break;
            }
            }
        }

        /* Beside the destination rather than in the temporary
         * directory, so that the rename cannot cross a file system. */
        tmpname = ne_concat(filename, ".cadaver-XXXXXX", NULL);
        fd = cad_mkstemp(tmpname);
    }

    out_start_transfer(o_download, _("Downloading `%s' to `%s':"),
                       native_remote, filename);

    if (fd < 0) {
        out_fail("%s\n", strerror(errno));
        goto fail;
    }

    if (resume) {
        ret = ne_get_range(session.sess, uri_path, &range, fd);
    }
    else {
        ret = ne_get(session.sess, uri_path, fd);
    }

    if (ret != NE_OK && resume) {
        /* ne_get_range() checks that the response was 206 only after
         * the body has gone to the descriptor, so a server that ignored
         * the Range header and answered 200 has just appended a whole
         * second copy of the resource.  Put the file back to the length
         * it had before the request. */
        if (cad_truncate(fd, resume_from) != 0) {
            out_printf(_("Warning: could not truncate `%s' back to "
                         "%" NE_FMT_NE_OFF_T " bytes: %s\n"),
                       filename, resume_from, strerror(errno));
        }
    }

    if (close(fd) && ret == NE_OK) {
        int errnum = errno;
        ret = NE_ERROR;
        ne_set_error(session.sess, _("Could not write to file: %s"),
                     strerror(errnum));
    }
    fd = -1;

    if (tmpname) {
        if (ret == NE_OK && cad_rename_over(tmpname, filename) != 0) {
            ret = NE_ERROR;
            ne_set_error(session.sess, _("Could not rename %s to %s: %s"),
                         tmpname, filename, strerror(errno));
        }
        if (ret != NE_OK) remove(tmpname);
    }

    out_result(ret);

fail:
    if (fd >= 0) (void) close(fd);
    ne_free(uri_path);
    if (tmpname) ne_free(tmpname);
    if (filename) ne_free(filename);
}

static void execute_get(const char *remote, const char *local)
{
    return do_get(remote, local, 0);
}

static void execute_resumeget(const char *remote, const char *local)
{
    return do_get(remote, local, 1);
}

static void simple_put(const char *local, const char *remote)
{
    struct cad_finfo info;
    char *native_remote = native_path_from_uri(remote);
    int fd;

    /* Every other command names a resource in native form; this one
     * used to hand output() the escaped URI path, so uploading
     * "has space.txt" reported /paths/has%20space.txt where deleting it
     * reported /paths/has space.txt. */
    out_start_transfer(o_upload, _("Uploading %s to `%s':"), local,
                       native_remote);
    ne_free(native_remote);

    /* `mput *' matches directories, and opening one gives EACCES on
     * Windows and EISDIR elsewhere, so it used to report "Could not
     * open file: Permission denied" -- true of neither. */
    if (cad_file_info(local, &info) == 0 && info.is_dir) {
        out_fail(_("%s is a directory, and there is no recursive upload "
                   "yet.\n"), local);
        return;
    }

    fd = open(local, O_RDONLY | OPEN_BINARY_FLAGS | O_LARGEFILE);
    if (fd < 0) {
	out_fail(_("could not open file: %s\n"), strerror(errno));
    } else {
	out_result(ne_put(session.sess, remote, fd));
	(void) close(fd);
    }
}

static void execute_put(const char *local, const char *remote)
{
    char *uri_path = uri_resolve_native(remote ? remote : base_name(local));
    simple_put(local, uri_path);
    free(uri_path);
}

/* A bit like Haskell's map function... applies func to each
 * of the first argc items in argv */
static void 
map_multi(void (*func)(const char *), int argc, const char *argv[])
{
    int n;
    for(n = 0; n < argc; n++) {
	(*func)(argv[n]);
    }
}

static void multi_copy(int argc, const char *argv[])
{
    do_copymove(argc, argv, _("copying"), _("copy"), simple_copy);
}

static void multi_move(int argc, const char *argv[])
{
    do_copymove(argc, argv, _("moving"), _("move"), simple_move);
}

static void multi_mkcol(int argc, const char *argv[])
{
    map_multi(execute_mkcol, argc, argv);
}

static void multi_delete(int argc, const char *argv[])
{
    map_multi(execute_delete, argc, argv);
}

static void multi_rmcol(int argc, const char *argv[])
{
    map_multi(execute_rmcol, argc, argv);
}

static void multi_less(int argc, const char *argv[])
{
    map_multi(execute_less, argc, argv);
}

static void multi_cat(int argc, const char *argv[])
{
    map_multi(execute_cat, argc, argv);
}

/* this is getting too easy */
static void multi_mput(int argc, const char *argv[])
{
    for(; argv[0] != NULL; argv++) {
	char *uri_path = uri_resolve_native(argv[0]);
	simple_put(argv[0], uri_path);
	ne_free(uri_path);
    }    
}

static void multi_mget(int argc, const char *argv[])
{
    for(; argv[0] != NULL; argv++) {
	execute_get(argv[0], NULL);
    }
}

static void execute_chexec(const char *val, const char *native_path)
{
    static const ne_propname execprop = 
    { "http://apache.org/dav/props/", "executable" };
    /* Use a single operation; set the executable property to... */
    ne_proppatch_operation ops[] = { { &execprop, ne_propset, NULL }, { NULL } };
    char *uri_path = uri_resolve_native(native_path);
    ne_server_capabilities caps = {0};
    int ret;

    /* True or false, depending... */
    if (strcmp(val, "+") == 0) {
        ops[0].value = "T";
    }
    else if (strcmp(val, "-") == 0) {
        ops[0].value = "F";
    }
    else {
        out_printf(_("Use:\n"
                 "   chexec + %s   to make the resource executable\n"
                 "or chexec - %s   to make the resource unexecutable\n"),
                 native_path, native_path);
        cmd_failed(_("the first argument must be + or -"));
        return;
    }
    
    out_start(_("Setting isexecutable"), native_path);
    ret = ne_options(session.sess, uri_path, &caps);
    if (ret != NE_OK) {
        out_result(ret);
    }
    else if (!caps.dav_executable) {
        ne_set_error(session.sess,
                     _("The server does not support the 'isexecutable' property."));
        out_result(NE_ERROR);
    }
    else {
        out_result(ne_proppatch(session.sess, uri_path, ops));
    }
    ne_free(uri_path);
}

static void execute_head(const char *native_path)
{
    char *uri_path = uri_resolve_native(native_path);
    ne_request *req;
    int ret;

    /* `head' used to print the status and headers when it got them and
     * nothing at all when it did not, so a HEAD that failed left no
     * trace in the transcript.  It now announces itself and reports the
     * outcome the way `ls' does, before the answer. */
    out_start(_("Fetching headers for"), native_path);

    /* After out_start(), which forgets the request the operation before
     * it made: the hook that records this one runs from
     * ne_request_create(). */
    req = ne_request_create(session.sess, "HEAD", uri_path);

    ret = ne_begin_request(req);
    if (ret == NE_OK) {
        const char *name, *value;
        void *iter = NULL;
        int status = ne_get_status(req)->code;

        /* A HEAD that was answered is a HEAD that worked, whatever the
         * status: the status is the answer the command asked for. */
        out_success();

        out_printf(_("Response status-code %d, headers:\n"), status);
        res_http_status(status);

        while ((iter = ne_response_header_iterate(req, iter,
                                                  &name, &value)) != NULL) {
            out_printf("%s %s: %s\n", bullet_str(), name, value);
            res_header(name, value);
        }

        if (ne_discard_response(req) == NE_OK)
            ne_end_request(req);
    }
    else {
        out_result(ret);
    }

    ne_request_destroy(req);
    ne_free(uri_path);
}

static void execute_lpwd(void)
{
    char pwd[BUFSIZ];
    if (getcwd(pwd, BUFSIZ) == NULL) {
        out_printf(_("Could not read the local directory: %s\n"),
                   strerror(errno));
        cmd_failed(strerror(errno));
    } else {
	out_printf(_("Local directory: %s\n"), pwd);
        res_path(pwd);
    }
}


static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Lists a local directory, in the layout the remote `ls' uses, so that
 * the two read the same way.  It used to fork an `ls', which Windows
 * has neither the call nor the program for. */
static void execute_lls(int argc, const char **argv)
{
    const char *dir = ".";
    char **names = NULL;
    size_t count = 0, alloc = 0, n;
    DIR *dp;
    struct dirent *ent;
    int i;

    for (i = 0; i < argc && argv[i] != NULL; i++) {
        if (argv[i][0] == '-') {
            /* The listing is always in the long form, so -l is the one
             * option that would not change it. */
            if (strcmp(argv[i], "-l") != 0) {
                out_printf(_("lls: unknown option `%s'; only -l is accepted.\n"),
                       argv[i]);
                cmd_failed(_("unknown option"));
                return;
            }
        }
        else {
            dir = argv[i];
        }
    }

    dp = opendir(dir);
    if (dp == NULL) {
        out_printf(_("Could not list local directory `%s': %s\n"),
               dir, strerror(errno));
        cmd_failed(strerror(errno));
        return;
    }

    while ((ent = readdir(dp)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        if (count == alloc) {
            alloc = alloc ? alloc * 2 : 64;
            names = ne_realloc(names, alloc * sizeof *names);
        }

        names[count++] = ne_strdup(ent->d_name);
    }

    closedir(dp);

    if (count == 0) {
        out_printf(_("Local directory `%s' is empty.\n"), dir);
        ne_free(names);
        return;
    }

    qsort(names, count, sizeof *names, compare_names);

    for (n = 0; n < count; n++) {
        struct cad_finfo info;
        char *path = ne_concat(dir, "/", names[n], NULL);

        if (cad_file_info(path, &info) != 0) {
            out_printf(_("Error: %-30s %s\n"), names[n], strerror(errno));
            cmd_failed(strerror(errno));
        }
        else {
            out_printf("%5s   %-29s %10" NE_FMT_NE_OFF_T "  %s\n",
                   info.is_dir ? _("Coll:") : "", names[n],
                   info.size, format_time(info.mtime));
        }

        ne_free(path);
        ne_free(names[n]);
    }

    ne_free(names);
}

static void execute_lcd(const char *p)
{
    const char *real_path;
    if (p) {
	real_path = p;
    } else {
	real_path = cad_home_dir();
	if (!real_path) {
	    out_printf(_("Could not determine home directory from environment.\n"));
            cmd_failed(_("no home directory"));
	    return;
	}
    }
    if (chdir(real_path)) {
	out_printf(_("Could not change local directory:\nchdir: %s\n"),
	       strerror(errno));
        cmd_failed(strerror(errno));
    }
}

static void execute_pwd(void)
{
    char *uri = ne_uri_unparse(&session.uri);
    out_printf(_("Current collection is `%s'.\n"), uri);
    res_path(uri);
    ne_free(uri);
}

static void execute_cd(const char *native_path)
{
    char *dest_uri_path, *uri_path = NULL;

    if (strcmp(native_path, "-") == 0) {
        if (!session.lastwp) {
            out_printf(_("No previous collection.\n"));
            cmd_failed(_("no previous collection"));
            return;
        }
        dest_uri_path = session.lastwp;
    }
    else {
        dest_uri_path = uri_path = uri_resolve_native_coll(native_path);
    }
    /* An operation with no announcement: `cd' prints nothing when it
     * works, but it does make a request, and --json has to be able to
     * say which one. */
    out_start_raw("%s", "");

    if (set_path(dest_uri_path) == 0) {
        out_done();
        /* Success */
        if (dest_uri_path == uri_path) {
            ne_free(session.lastwp);
        }
        session.lastwp = session.uri.path;
        session.uri.path = dest_uri_path;
    }
    else {
        /* set_path() has already said why. */
        out_failed(ne_get_error(session.sess));
        if (uri_path) ne_free(uri_path);
    }
}

static void display_help_message(void)
{
    unsigned int n;

    /* The line break goes before each row rather than after the
     * seventh name, so that the list always ends with exactly one
     * newline however many commands there are.  It used to end with a
     * carriage return when the last row was short, which ran the
     * aliases line into the last command on a terminal and left a stray
     * CR in a redirected transcript. */
    out_printf("Available commands: ");

    for (n = 0; commands[n].id != cmd_unknown; n++) {
        if (n % 7 == 0) out_printf("\n ");
        out_printf("%-11s", commands[n].name);
    }

    out_putchar('\n');

    out_printf(_("Aliases: rm=delete, mkdir=mkcol, mv=move, cp=copy, "
	     "more=less, quit=exit=bye\n"));
}

static void execute_help(const char *arg)
{
    if (!arg) {
	display_help_message();
    } else {
	const struct command *cmd = get_command(arg);

	if (cmd) {
	    out_printf(_(" `%s'   %s\n"), cmd->call, _(cmd->short_help));
	    if (cmd->needs_connection) {
		out_printf(_("This command can only be used when connected to a server.\n"));
	    }
	} else {
	    out_printf(_("Command name not known: %s\n"), arg);
            cmd_failed(_("no such command"));
	}
    }
}

static void execute_echo(int count, const char **args)
{
    const char **pnt;
    for(pnt = args; *pnt != NULL; pnt++) {
	out_printf("%s ", *pnt);
    }
    out_putchar('\n');
}

void execute_about(void)
{
    out_printf("cadaver " PACKAGE_VERSION "\n%s\n", ne_version_string());
#ifdef HAVE_LIBREADLINE
    out_printf("readline %s\n", rl_library_version);
#endif
}

/* The T? macros are used to cast the function pointer into the
 * command structure.  Using GCC extensions this is done in a
 * type-safe way; for non-GCC< it's type-unsafe. */
#if defined(__GNUC__)
#define T0(x) {take0: x}
#define T1(x) {take1: x}
#define T2(x) {take2: x}
#define T3(x) {take3: x}
#define TV(x) {takeV: x}
#else
/* cast to be like first member of union. */
#define Tn(x) { ((void (*)(void)) x) }
#define T0(x) Tn(x)
#define T1(x) Tn(x)
#define T2(x) Tn(x)
#define T3(x) Tn(x)
#define TV(x) Tn(x)
#endif

/* C1: connected, 1-arg function C2: connected, 2-arg function
 * U0: disconnected, 0-arg function. */
#define C1(x,c,h) { cmd_##x, #x, true, 1, 1, parmscope_remote, "r", T1(execute_##x),c,h }
#define C2(x,c,h) { cmd_##x, #x, true, 2, 2, parmscope_remote, "r", T2(execute_##x),c,h }
#define U0(x,h) { cmd_##x, #x, false, 0, 0, parmscope_none, "", T0(execute_##x),#x,h }
#define UO1(x,c,h) { cmd_##x, #x, false, 0, 1, parmscope_none, "n", T1(execute_##x),c,h }
#define C2M(x,c,h) { cmd_##x, #x, true, 2, CMD_VARY, parmscope_remote, "r", TV(multi_##x),c,h }
#define C2M(x,c,h) { cmd_##x, #x, true, 2, CMD_VARY, parmscope_remote, "r", TV(multi_##x),c,h }
#define C1M(x,c,h) { cmd_##x, #x, true, 1, CMD_VARY, parmscope_remote, "r", TV(multi_##x),c,h }

/* commands[] is not static because it would mean adding a bunch of
 * prototypes for execute_* etc, and declaring this at the top of the
 * file. */

/* Separate structures for commands and command names. */
/* DON'T FORGET TO ADD A NEW COMMAND ALIAS WHEN YOU ADD A NEW COMMAND */
const struct command commands[] = {
    { cmd_ls, "ls", true, 0, 1, parmscope_remote, "r", T1(execute_ls), 
      N_("ls [path]"), N_("List contents of current [or other] collection") },
    C1(cd, N_("cd path"), N_("Change to specified collection")),
    { cmd_pwd, "pwd", true, 0, 0, parmscope_none, "", T0(execute_pwd),
      "pwd", N_("Display name of current collection") },
    { cmd_put, "put", true, 1, 2, parmscope_none, "lr", T2(execute_put),
      N_("put local [remote]"), N_("Upload local file") },
    { cmd_get, "get", true, 1, 2, parmscope_none, "rl", T2(execute_get),
      N_("get remote [local]"), N_("Download remote resource") },
    { cmd_resumeget, "resumeget", true, 1, 2, parmscope_none, "rl", T2(execute_resumeget),
      N_("resumeget remote [local]"), N_("Resume download of remote resource") },
    C1M(mget, N_("mget remote..."), N_("Download many remote resources")),
    { cmd_mput, "mput", true, 1, CMD_VARY, parmscope_local, "l", TV(multi_mput), 
      N_("mput local..."), N_("Upload many local files") },
    C1(edit, N_("edit resource"), N_("Edit given resource")),
    C1(head, N_("head remote"), N_("Show resource metadata")),
    C1M(less, N_("less remote..."), N_("Display remote resource through pager")), 
    C1M(mkcol, N_("mkcol remote..."), N_("Create remote collection(s)")), 
    C1M(cat, N_("cat remote..."), N_("Display remote resource(s)")), 
    C1M(delete, N_("delete remote..."), N_("Delete non-collection resource(s)")),
    C1M(rmcol, N_("rmcol remote..."), N_("Delete remote collections and ALL contents")),
    C2M(copy, N_("copy source... dest"), N_("Copy resource(s) from source to dest")), 
    C2M(move, N_("move source... dest"), N_("Move resource(s) from source to dest")),
    C2(rename, N_("rename source dest"), N_("Rename resource from source to dest")),

/* DON'T FORGET TO ADD A NEW COMMAND ALIAS WHEN YOU ADD A NEW COMMAND */

    /*** Locking commands ***/

    C1(lock, N_("lock resource"), N_("Lock given resource")),
    C1(unlock, N_("unlock resource"), N_("Unlock given resource")),
    C1(discover, N_("discover resource"), N_("Display lock information for resource")),
    C1(steal, N_("steal resource"), N_("Steal lock token for resource")),
    { cmd_showlocks, "showlocks", true, 0, 0, parmscope_none, "", T0(execute_showlocks),
      "showlocks", N_("Display list of owned locks") },

    /*** DeltaV commands ***/
    C1(version, N_("version resource"), N_("Place given resource under version control")),
    C1(checkin, N_("checkin resource"), N_("Checkin given resource")),
    C1(checkout, N_("checkout resource"), N_("Checkout given resource")),
    C1(uncheckout, N_("uncheckin resource"), N_("Uncheckout given resource")),
    C1(history, N_("history resource"), N_("Show version history of resource")),

    { cmd_label, "label", true, 3, 3, parmscope_none, "rnn", T3(execute_label),
      N_("label res [add|set|remove] labelname"),
      N_("Set/Del/Edit label on resource") },


    /*** Property handling ***/
    C1(propnames, "propnames res", N_("Names of properties defined on resource")) ,

    { cmd_chexec, "chexec", true, 2, 2, parmscope_none, "nr", T2(execute_chexec),
      N_("chexec [+|-] remote"), N_("Change isexecutable property of resource") },
    
    { cmd_propget, "propget", true, 1, 2, parmscope_none, "rn", T2(execute_propget),
      N_("propget res [propname]"), 
      N_("Retrieve properties of resource") },
    { cmd_propdel, "propdel", true, 2, 2, parmscope_none, "rn", T2(execute_propdel),
      N_("propdel res propname"), 
      N_("Delete property from resource") },
    { cmd_propset, "propset", true, 3, 3, parmscope_none, "rnn", T3(execute_propset),
      N_("propset res propname value"),
      N_("Set property on resource") },

    { cmd_search, "search", true, 1, CMD_VARY, parmscope_remote, "n", TV(execute_search),
      N_("search query"), 
      N_("DASL Search resource in current collection\n\n"
	 " Examples:\n"
	 "   - search where content length is smaller than 100:\n"
	 "      > search getcontentlength < 100\n" 
         "   - search where author is Smith or Jones\n"
         "      > search author = Smith or author = Jones\n"
	 " Available operators and keywords:\n"
	 "     - and, or , (, ), =, <, >, <=, >=, like\n"
         " (See also variables searchdepth, searchorder, searchdorder)\n") },
    
    { cmd_set, "set", false, 0, 2, parmscope_none, "on", T2(execute_set), 
      N_("set [option] [value]"), N_("Set an option, or display options") },
    { cmd_open, "open", false, 1, 1, parmscope_none, "n", T1(open_connection), 
      "open URL", N_("Open connection to given URL") },
    { cmd_close, "close", true, 0, 0, parmscope_none, "", T0(close_connection), 
      "close", N_("Close current connection") },
    { cmd_echo, "echo", false, 1, CMD_VARY, parmscope_remote, "n", TV(execute_echo), 
      "echo", NULL },
    { cmd_quit, "quit", false, 0, 1, parmscope_none, "", T1(NULL), "quit",
      N_("Exit program") },
    /* Unconnected operation, 1 mandatory argument */
    { cmd_unset, "unset", false, 1, 2, parmscope_none, "on", T2(execute_unset), 
      N_("unset [option] [value]"), N_("Unsets or clears value from option.") },
    /* Unconnected operation, 0 arguments */
    { cmd_lcd, "lcd", false, 0, 1, parmscope_none, "l", T1(execute_lcd),
      N_("lcd [directory]"), N_("Change local working directory") },
    { cmd_lls, "lls", false, 0, CMD_VARY, parmscope_local, "l", TV(execute_lls), 
      N_("lls [options]"), N_("Display local directory listing") },
    U0(lpwd, N_("Print local working directory")),
    { cmd_logout, "logout", true, 0, 0, parmscope_none, "", T0(execute_logout), "logout",
      N_("Logout of authentication session") },
    UO1(help, N_("help [command]"), N_("Display help message")), 
    { cmd_describe, "describe", false, 1, 1, parmscope_none, "o", T1(execute_describe),
      "describe option", N_("Describe an option variable") },
    U0(about, N_("Information about this version of cadaver") ),

/* DON'T FORGET TO ADD A NEW COMMAND ALIAS WHEN YOU ADD A NEW COMMAND. */

    { cmd_unknown, 0 } /* end-of-list marker, DO NOT move */
};    

