/* 
   cadaver, command-line DAV client
   Copyright (C) 1999-2024, Joe Orton <joe@manyfish.co.uk>,
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

#include <sys/types.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/stat.h>

#include <stdio.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif 
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif

#include <errno.h>

#include "i18n.h"

#include <getopt.h>

#include "getpass.h"
#include "system.h"

#ifdef ENABLE_NETRC
#include "netrc.h"
#endif

#include <ne_request.h>
#include <ne_auth.h>
#include <ne_basic.h>
#include <ne_string.h>
#include <ne_uri.h>
#include <ne_socket.h>
#include <ne_locks.h>
#include <ne_alloc.h>
#include <ne_redirect.h>

#include "common.h"
#include "cadaver.h"
#include "cmdline.h"
#include "commands.h"
#include "options.h"
#include "utils.h"

#define DEFAULT_NAMESPACE "http://webdav.org/cadaver/custom-properties/"

#ifdef ENABLE_NETRC
static netrc_entry *netrc_list; /* list of netrc entries */
#endif

/* Global state: */
const char *lock_store_fn = NULL;
static char *progname; /* argv[0], less its directory and suffix */
static char *rcfile;
static char *proxy_hostname;
static char *server_username, *server_password;

/* Current session state. */
struct session session;

int tolerant; /* tolerate DAV-enabledness failure */
int in_completion; /* non-zero if in completion. */

/* Where --trace writes, NULL when not tracing, and whether it is a file
 * of cadaver's own to close.  --verbose widens what goes there from the
 * request and response headers to everything neon reports. */
static FILE *trace_fp;
static int trace_needs_close;
static int verbose;

/* Everything neon can report except NE_DBG_HTTPPLAIN, which is the
 * credentials in the clear.  --verbose is for diagnosing a connection,
 * a handshake or an authentication exchange; it is not a reason to
 * write a password into a log file.  `set debug cleartext' still is. */
#define TRACE_MASK (NE_DBG_HTTP | NE_DBG_HTTPBODY | NE_DBG_XML | \
                    NE_DBG_XMLPARSE | NE_DBG_SOCKET | NE_DBG_SSL | \
                    NE_DBG_HTTPAUTH | NE_DBG_LOCKS | DEBUG_FILES)

/* Current output state */
static enum out_state {
    out_none, /* not doing anything */
    out_incommand, /* doing a simple command */
    out_transfer_upload, /* uploading a file, not yet started */
    out_transfer_download, /* downloading a file, not yet started */
    out_transfer_plain, /* doing a plain ... transfer */
    out_transfer_pretty, /* doing a pretty progress bar transfer */
    out_transfer_done /* a complete transfer */
} out_state;   

/* Prototypes */

static void quit_handler(int signo);

static void notifier(void *ud, ne_session_status status, 
                     const ne_session_status_info *info);
static void pretty_progress_bar(ne_off_t progress, ne_off_t total);
static void hook_create_request(ne_request *req, void *userdata,
                                const char *method, const char *target);
static void hook_pre_send(ne_request *req, void *userdata, ne_buffer *hdr);
static void hook_post_headers(ne_request *req, void *userdata,
                              const ne_status *status);
static int supply_creds_server(void *userdata, const char *realm, int attempt,
			       char *username, char *password);
static int supply_creds_proxy(void *userdata, const char *realm, int attempt,
			      char *username, char *password);

static void usage(void)
{
    /* The rcfile default is worked out at run time rather than written
     * as "~/.cadaverrc": on Windows the home directory comes from the
     * profile and the path is one the user can paste into a shell. */
    char *deflt = cad_home_path(".cadaverrc");

    out_printf(_(
"Usage: %s [OPTIONS] URL\n"
"  URL must be an absolute URI using the http: or https: scheme.\n"
"Options:\n"
"  -t, --tolerant            Allow cd/open into non-WebDAV enabled collection.\n"
"  -r, --rcfile=FILE         Read script from FILE instead of the default.\n"
"  -p, --proxy=PROXY[:PORT]  Use proxy host PROXY and optional proxy port PORT.\n"
"  -c, --clobber=WHAT        What `get' does when the local file exists: ask,\n"
"                            which is the default, yes to overwrite, or no.\n"
"  -j, --json                Write one JSON object describing the session to\n"
"                            standard output, and nothing else.\n"
"  -T, --trace[=FILE]        Dump every request and response to FILE; standard\n"
"                            error if FILE is omitted, standard output for `-'.\n"
"  -v, --verbose             Widen the trace to everything neon reports.\n"
"  -V, --version             Display version information.\n"
"  -h, --help                Display this help message.\n"), progname);

    if (deflt) {
        out_printf(_("The default rcfile is %s\n"), deflt);
        ne_free(deflt);
    }

    out_printf(_("Please report bugs via <https://github.com/Azathothas/"
             "cadaver-windows>\n"));
}

static void init_locking(void)
{
    lock_store_fn = get_option(opt_lockstore);
    session.locks = ne_lockstore_create();
    /* TODO: read in lock list from ~/.davlocks */
}

static void finish_locking(void)
{
    /* TODO: write out lock list to ~/.davlocks */
}

void close_connection(void)
{
    if (session.sess) ne_session_destroy(session.sess);
    session.sess = NULL;
    if (session.connected && session.uri.host)
        out_printf(_("Connection to `%s' closed.\n"), session.uri.host);
    session.connected = false;
    ne_uri_free(&session.uri);
    if (session.lastwp)
        ne_free(session.lastwp);
}

/* Sets the current collection to the given URI path.  Returns zero on
 * success, non-zero if newpath is an untolerated non-WebDAV
 * collection. */
int set_path(const char *uri_path)
{
    enum resource_type type = getrestype(uri_path);
    int is_coll = type == resr_collection;

    if (is_coll || tolerant) {
	if (!is_coll) {
	    session.isdav = 0;
	    out_printf(_("Ignored error: %s not WebDAV-enabled:\n%s\n"), uri_path,
		   ne_get_error(session.sess));
	} else {
	    session.isdav = 1;
	}
	return 0;
    }

    /* The two ways this fails are not the same thing, and saying "not
     * WebDAV-enabled?" for both sent the reporter of upstream issue #4
     * looking in the wrong place: their server answered the PROPFIND
     * perfectly well but wrote <collection/> without the DAV namespace,
     * so cadaver did not recognise the collection. */
    if (type == resr_error) {
        out_printf(_("Could not access %s (not WebDAV-enabled?):\n%s\n"),
                   uri_path, ne_get_error(session.sess));
    }
    else {
        out_printf(_("Could not access %s: the server answered, but did not "
                     "report it as a collection.\n"
                     "A server whose PROPFIND names the resourcetype without "
                     "the DAV: namespace looks like this.\n"
                     "Use -t (or `set tolerant on') to use it anyway.\n"),
                   uri_path);
    }

    return 1;
}

static int cert_verify(void *ud, int failures, const ne_ssl_certificate *c)
{
    char *tmp, from[NE_SSL_VDATELEN], to[NE_SSL_VDATELEN];
    const char *ident;

    ident = ne_ssl_cert_identity(c);

    if (ident)
        out_printf(_("WARNING: Untrusted server certificate presented for `%s':\n"),
               ident);
    else
        out_puts_line(_("WARNING: Untrusted server certificate presented:\n"));

#if NE_MINIMUM_VERSION(0, 31)
    tmp = ne_ssl_cert_hdigest(c, NE_HASH_SHA256|NE_HASH_COLON);
    if (tmp) {
        out_printf(_("Server certificate SHA-256 digest: %s\n"), tmp);
        ne_free(tmp);
    }
#endif

    if (failures & NE_SSL_IDMISMATCH) {
	out_printf(_("Certificate was issued to hostname `%s' rather than `%s'\n"),
	       ne_ssl_cert_identity(c), session.uri.host);
	out_printf(_("This connection could have been intercepted.\n"));
    }

#define PRINT_AND_FREE(str, dn) \
tmp = ne_ssl_readable_dname(dn); out_printf(str, tmp); free(tmp)

    PRINT_AND_FREE(_("Issued to: %s\n"), ne_ssl_cert_subject(c));
    PRINT_AND_FREE(_("Issued by: %s\n"), ne_ssl_cert_issuer(c));

    ne_ssl_cert_validity(c, from, to);
    out_printf(_("Certificate is valid from %s to %s\n"), from, to);

    if (isatty(STDIN_FILENO)) {
	out_printf(_("Do you wish to accept the certificate? (y/n) "));
	return !yesno();
    } else {
	out_printf(_("Certificate rejected.\n"));
	return -1;
    }
}

static void setup_ssl(ne_session *sess)
{
    char *ccfn = get_option(opt_clicert);
    char *ccuri = get_option(opt_clicert_uri);
    const char *name = NULL;
    ne_ssl_client_cert *cc;

    ne_ssl_trust_default_ca(sess);
    ne_ssl_set_verify(sess, cert_verify, NULL);

    cc = NULL;
    if (ccuri) {
        name = ccuri;
#if NE_MINIMUM_VERSION(0, 35)
        cc = ne_ssl_clicert_fromuri(ccuri, 0);
#else
        out_printf(_("Client certificate URIs are supported "
                 "with this version of neon.\n"));
#endif
    }

    if (name == NULL && ccfn) {
        name = ccfn;
        cc = ne_ssl_clicert_read(ccfn);
    }

    if (!name) return;

    if (!cc) {
        out_printf(_("Could not load client certificate from `%s'.\n"), name);
        cmd_failed(_("could not load the client certificate"));
        return;
    }

    if (ne_ssl_clicert_encrypted(cc)) {
        const char *friendly = ne_ssl_clicert_name(cc);
        int n;
        
        if (!friendly) friendly = name;
        
        out_printf("Client certificate `%s' is encrypted.\n", friendly);

        for (n = 0; n < 3; n++) {
            char *pass = fm_getpassword(_("Decryption password: "));
            if (pass == NULL) break;
            if (ne_ssl_clicert_decrypt(cc, pass)) {
                out_printf("Password incorrect, try again.\n");
            }
            else {
                break;
            }
        }
    }
    
    if (!ne_ssl_clicert_encrypted(cc)) {
        out_printf("Using client certificate.\n");
        ne_ssl_set_clicert(session.sess, cc);
    }

    ne_ssl_clicert_free(cc);
}

void open_connection(const char *url)
{
    const char *proxy_host = get_option(opt_proxy);
    int ret;
    ne_session *sess = NULL;
    unsigned int proxy_port = 8080;

    close_connection();

    /* Parse the URL */
    if (ne_uri_parse(url, &session.uri) || session.uri.path == NULL
        || session.uri.scheme == NULL || session.uri.host  == NULL) {
        out_printf(_("Could not parse URL `%s'\n"), url);
        cmd_failed(_("could not parse the URL"));
        goto fail;
    }

    if (session.uri.userinfo) {
        out_printf(_("User info must not be used in URL `%s'\n"), url);
        cmd_failed(_("credentials must not be in the URL"));
        goto fail;
    }

    if (!session.uri.port)
        session.uri.port = ne_uri_defaultport(session.uri.scheme);

    /* Collections must be slash-terminated to avoid a redirect
     * round-trip. */
    if (!ne_path_has_trailing_slash(session.uri.path)) {
        char *pnt = ne_concat(session.uri.path, "/", NULL);
        ne_free(session.uri.path);
        session.uri.path = pnt;
    }

    sess = ne_session_create(session.uri.scheme, session.uri.host, session.uri.port);
    session.sess = sess;

    if (ne_strcasecmp(session.uri.scheme, "https") == 0) {
        if (!ne_has_support(NE_FEATURE_SSL)) {
            out_printf(_("No SSL/TLS support, cannot use URL `%s'\n"), url);
            cmd_failed(_("no TLS support in this build"));
        goto fail;
        }
        setup_ssl(sess);
    }
    else if (ne_strcasecmp(session.uri.scheme, "http")) {
        out_printf(_("URL scheme '%s' not supported.\n"), session.uri.scheme);
        cmd_failed(_("unsupported URL scheme"));
        goto fail;
    }

    ne_hook_create_request(sess, hook_create_request, NULL);
    ne_hook_post_headers(sess, hook_post_headers, NULL);
    if (trace_fp) ne_hook_pre_send(sess, hook_pre_send, NULL);

    ne_lockstore_register(session.locks, sess);
    ne_redirect_register(sess);
    ne_set_notifier(sess, notifier, NULL);
    ne_set_session_flag(sess, NE_SESSFLAG_PERSIST, get_bool_option(opt_keepalive));
    ne_set_session_flag(sess, NE_SESSFLAG_EXPECT100, get_bool_option(opt_expect100));

    /* Get the proxy details */
    if (proxy_host && get_option(opt_proxy_port) != NULL) {
        proxy_port = atoi(get_option(opt_proxy_port));
    }

#ifdef ENABLE_NETRC
    {
	netrc_entry *found;
	found = search_netrc(netrc_list, session.uri.host);
	if (found != NULL) {
            /* Whichever fields the entry has.  An entry with a login
             * and no password used to be ignored entirely, so cadaver
             * prompted for both and the .netrc looked broken --
             * upstream issue #25.  supply_creds_server() now fills in
             * what is there and asks only for the rest. */
            if (found->account) server_username = found->account;
            if (found->password) server_password = found->password;
	}
    }
#endif /* ENABLE_NETRC */
    session.connected = 0;

    ne_set_useragent(session.sess, "cadaver/" PACKAGE_VERSION);
    ne_set_server_auth(session.sess, supply_creds_server, NULL);
    ne_set_proxy_auth(session.sess, supply_creds_proxy, NULL);

    if (get_bool_option(opt_systemproxy)) {
        ne_session_system_proxy(session.sess, 0);
    }
    else if (proxy_host) {
        if (proxy_hostname) ne_free(proxy_hostname);
        proxy_hostname = ne_strdup(proxy_host);
        ne_session_proxy(session.sess, proxy_host, proxy_port);
    }

    session.caps = 0;
    ret = ne_options2(session.sess, session.uri.path, &session.caps);

    switch (ret) {
    case NE_OK:
        if ((session.caps & NE_CAP_DAV_CLASS1) == 0) {
            out_printf(_("%s: Location does not advertise WebDAV class 1 support.\n"),
                   tolerant ? _("Warning") : _("Error"));
            if (!tolerant) {
                cmd_failed(_("the location does not advertise WebDAV class 1"));
                break;
            }
        }
	if (set_path(session.uri.path) == 0) {
            session.connected = true;
            return;
        }
        cmd_failed(ne_get_error(session.sess));
        break;
    case NE_CONNECT:
	if (proxy_host) {
	    out_printf(_("Could not connect to `%s' on port %d:\n%s\n"),
		   proxy_hostname, proxy_port, ne_get_error(session.sess));
	} else {
	    out_printf(_("Could not connect to `%s' on port %d:\n%s\n"),
		   session.uri.host, session.uri.port, ne_get_error(session.sess));
	}
        cmd_failed(ne_get_error(session.sess));
	break;
    case NE_LOOKUP:
	out_puts_line(ne_get_error(session.sess));
        cmd_failed(ne_get_error(session.sess));
	break;
    default:
	out_printf(_("Could not open collection:\n%s\n"),
	       ne_get_error(session.sess));
        cmd_failed(ne_get_error(session.sess));
	break;
    }

fail:
    close_connection();
}
       
/* Sets proxy server from hostport argument */    
/* Opens the --trace destination.  Returns non-zero if it could not be
 * opened, having said so on standard error. */
static int open_trace(const char *fname)
{
    if (fname == NULL || *fname == '\0') {
        trace_fp = stderr;
    }
    else if (strcmp(fname, "-") == 0) {
        if (out_trace_claims_stdout()) return 1;
        trace_fp = stdout;
    }
    else {
        trace_fp = fopen(fname, "w");
        if (trace_fp == NULL) {
            fprintf(stderr, _("cadaver: could not open trace file `%s': %s\n"),
                    fname, strerror(errno));
            return 1;
        }
        trace_needs_close = 1;
    }

    return 0;
}

/* The message-body bit of the debug mask, put back by
 * trace_body_restore().  Zero when it was not set to begin with. */
static int trace_body_bit;

void trace_body_suppress(void)
{
    trace_body_bit = ne_debug_mask & NE_DBG_HTTPBODY;
    ne_debug_mask &= ~NE_DBG_HTTPBODY;
}

void trace_body_restore(void)
{
    ne_debug_mask |= trace_body_bit;
    trace_body_bit = 0;
}

static void close_trace(void)
{
    if (trace_fp) {
        fflush(trace_fp);
        if (trace_needs_close) fclose(trace_fp);
        trace_fp = NULL;
        trace_needs_close = 0;
    }
}

/* Writes `text' with every line prefixed by `prefix', so that a request
 * and a response stay visually distinct.  Bodies are not prefixed: a
 * PROPFIND or LOCK body is XML and nearly every line of one starts with
 * a `<' already. */
static void trace_block(const char *prefix, const char *text)
{
    const char *p = text;

    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);

        /* Requests carry CRLF line endings; drop the CR so the trace
         * does not end up with stray carriage returns in it. */
        if (len > 0 && p[len - 1] == '\r') len--;

        fprintf(trace_fp, "%s %.*s\n", prefix, (int)len, p);

        if (!eol) break;
        p = eol + 1;
    }
}

/* Records the request being built, so that --json can classify a
 * failure by method and status rather than by matching the prose in
 * "context". */
static void hook_create_request(ne_request *req, void *userdata,
                                const char *method, const char *target)
{
    req_started(method, target);
}

/* Dumps the request line and headers, tagged with the command that
 * issued them, so that grepping the trace for a command finds the
 * exchange it caused. */
static void hook_pre_send(ne_request *req, void *userdata, ne_buffer *hdr)
{
    fprintf(trace_fp, "\n--- %s ---\n", cmd_trace_label());
    trace_block(">", hdr->data);
    fflush(trace_fp);
}

static void hook_post_headers(ne_request *req, void *userdata,
                              const ne_status *status)
{
    req_status(status->code);

    if (trace_fp) {
        void *cursor = NULL;
        const char *name, *value;

        fprintf(trace_fp, "< HTTP/%d.%d %d %s\n", status->major_version,
                status->minor_version, status->code,
                status->reason_phrase ? status->reason_phrase : "");

        while ((cursor = ne_response_header_iterate(req, cursor,
                                                    &name, &value)) != NULL)
            fprintf(trace_fp, "< %s: %s\n", name, value);

        fputs("<\n", trace_fp);
        fflush(trace_fp);
    }
}

static void set_proxy(const char *str)
{
    char *hostname = ne_strdup(str), *pnt;

    pnt = strchr(hostname, ':');
    if (pnt != NULL) {
	*pnt++ = '\0';
        if (*pnt)
            set_option(opt_proxy_port, ne_strdup(pnt));
    }
    set_option(opt_proxy, (void *)hostname);
}

/* --help and --version answer a question about the program rather than
 * report a session, so they are plain text on standard output whatever
 * else was asked for, and they succeed. */
static void inform_and_exit(void (*what)(void))
{
    out_json = 0;
    (*what)();
    exit(0);
}

static void parse_args(int argc, char **argv)
{
    static const struct option opts[] = {
        { "version", no_argument, NULL, 'V' },
        { "help", no_argument, NULL, 'h' },
        { "proxy", required_argument, NULL, 'p' },
        { "tolerant", no_argument, NULL, 't' },
        { "rcfile", required_argument, NULL, 'r' },
        { "clobber", required_argument, NULL, 'c' },
        { "json", no_argument, NULL, 'j' },
        { "trace", optional_argument, NULL, 'T' },
        { "verbose", no_argument, NULL, 'v' },
        { 0, 0, 0, 0 }
    };
    int optc;

    while ((optc = getopt_long(argc, argv, "htp:r:c:jT::vV", opts,
                               NULL)) != -1) {
        switch (optc) {
        case 'h': inform_and_exit(usage); break;
        case 'V': inform_and_exit(execute_about); break;
        case 'p': set_proxy(optarg); break;
        case 't': tolerant = 1; break;
        case 'r': rcfile = ne_strdup(optarg); break;
        case 'c':
            if (set_clobber(optarg)) exit(CAD_EXIT_USAGE);
            break;
        case 'j':
            if (out_set_json()) exit(CAD_EXIT_USAGE);
            break;
        case 'T':
            if (open_trace(optarg)) exit(CAD_EXIT_USAGE);
            break;
        case 'v': verbose = 1; break;
        case '?':
        default:
            fprintf(stderr, _("Try `%s --help' for more information.\n"),
                    progname);
            exit(CAD_EXIT_USAGE);
        }
    }

    /* Before the first request, so that a connection made from the
     * command line is traced like everything else.  --verbose without
     * --trace writes to standard error, which is where neon's own
     * debugging has always gone. */
    /* --trace turns on neon's body debugging and nothing else: the
     * request and response headers come from the hooks below, which
     * tag each exchange with the command that caused it, and the body
     * is what neon alone can supply.  --verbose widens it to everything
     * neon reports. */
    if (verbose || trace_fp)
        ne_debug_init(trace_fp ? trace_fp : stderr,
                      verbose ? TRACE_MASK
                              : (ne_debug_mask | (trace_fp ? NE_DBG_HTTPBODY
                                                           : 0)));

    if (optind == (argc-1)) {
        /* The connection is a command in its own right: it is what
         * `open URL' would have done, it can fail, and a session whose
         * connection failed has to say so in its exit status. */
        const char *url = argv[optind];

        run_begin(url);
        cmd_begin(NULL);
        cmd_named("open", 1, &url);
        open_connection(url);
        cmd_end();
#ifdef HAVE_ADD_HISTORY
        {
            char *run_cmd = ne_concat("open ", argv[optind], NULL);
            add_history(run_cmd);
            free(run_cmd);
        }
#endif
    } else if (optind < argc) {
        usage();
        exit(CAD_EXIT_USAGE);
    } else {
        run_begin(NULL);
    }
}

static char *read_command(void)
{
    char prompt[BUFSIZ];

    if (session.uri.path) {
        char *p = native_path_from_uri(session.uri.path);
        ne_snprintf(prompt, BUFSIZ, "dav:%s%c ", p,
                    session.isdav ? '>' : '?');
        ne_free(p);
    }
    else {
        ne_strnzcpy(prompt, "dav:!> ", sizeof prompt);
    }

    return readline(prompt); 
}

static int execute_command(const char *line)
{
    const struct command *cmd;
    char **tokens;
    int n, argcount, ret = 0;

    /* The record is opened before the line is parsed, because expanding
     * a wildcard prints as it goes and that output belongs to the
     * command which caused it. */
    cmd_begin(line);

    tokens = parse_command(line, &argcount);
    if (argcount == 0) {
        free(tokens);
        cmd_discard();
        return 0;
    }
    argcount--;
    cmd_named(tokens[0], argcount, (const char **)&tokens[1]);
    cmd = get_command(tokens[0]);
    if (cmd == NULL) {
        out_printf(_("Unrecognised command. Type 'help' for a list of commands.\n"));
        cmd_failed(_("unrecognised command"));
    } else if (argcount < cmd->min_args) {
        out_printf(_("The `%s' command requires %d argument%s"),
                tokens[0], cmd->min_args, cmd->min_args==1?"":"s");
        if (cmd->short_help) {
            out_printf(_(":\n  %s : %s\n"), cmd->call, cmd->short_help);
        } else {
            out_printf(".\n");
        }
        cmd_failed(_("too few arguments"));
    } else if (argcount > cmd->max_args) {
        if (cmd->max_args) {
            out_printf(_("The `%s' command takes at most %d argument%s"),
                    tokens[0], cmd->max_args, cmd->max_args==1?"":"s");
        } else {
            out_printf(_("The `%s' command takes no arguments"), tokens[0]);
        }
        if (cmd->short_help) {
            out_printf(_(":\n" "  %s : %s\n"), cmd->call, cmd->short_help);
        } else {
            out_printf(".\n");
        }
        cmd_failed(_("too many arguments"));
    } else if (!session.connected && cmd->needs_connection) {
        out_printf(_("The `%s' command can only be used when connected to the server.\n"
                  "Try running `open' first (see `help open' for more details).\n"),
                  tokens[0]);
        cmd_failed(_("not connected"));
    } else if (cmd->id == cmd_quit) {
        ret = -1;
    } else {
        /* Cast away */
        /* with a nod in the general direction of apache */
        switch (cmd->max_args) {
        case 0: cmd->handler.take0(); break;
        case 1: /* tokens[1]==NULL if argcount==0 */
            cmd->handler.take1(tokens[1]); break;
        case 2:
            if (argcount <=1) {
                cmd->handler.take2(tokens[1], NULL);
            } else {
                cmd->handler.take2(tokens[1], tokens[2]);
            }
            break;
        case 3:
            cmd->handler.take3(tokens[1], tokens[2], tokens[3]);
            break;
        case CMD_VARY:
            cmd->handler.takeV(argcount, (const char **) &tokens[1]);
        default:
            break;
        }
    }
    /* parse_command() counts the command name as a token, so there are
     * argcount+1 of them; the last argument used to be left behind. */
    for (n = 0; n <= argcount; n++) {
        ne_free(tokens[n]);
    }
    ne_free(tokens);
    cmd_end();
    return ret;
}

static void quit_handler(int sig)
{
    /* Reinstall handler */
    if (child_running) {
	/* The child gets the signal anyway... it can deal with it.
	 * Proper way is probably to ignore signals while child is
	 * running? */
	signal(sig, quit_handler);
	return;
    } else {
	out_printf(_("Terminated by signal %d.\n"), sig);
	if (session.connected) {
	    close_connection();
	}
        /* The convention a shell uses for a process a signal ended,
         * and above the range a count of failed commands can reach, so
         * the two can never be confused.  run_finish() is not called:
         * it is not safe from a signal handler, and a session cut short
         * has no result to report. */
	exit(128 + (sig & 0x7F));
    }
}

static void init_signals(void)
{
    signal(SIGTERM, quit_handler);
    signal(SIGABRT, quit_handler);
#ifdef SIGQUIT
    /* Not one of the six signals ISO C requires, and Windows has no
     * equivalent. */
    signal(SIGQUIT, quit_handler);
#endif
    signal(SIGINT, quit_handler);
}

static void init_netrc(void)
{
#ifdef ENABLE_NETRC
    char *netrc = cad_home_path(".netrc");

    if (netrc) {
        netrc_list = parse_netrc(netrc);
        ne_free(netrc);
    }
#endif
}

/* Reads one line from `f', however long, with the line ending left on.
 * Returns NULL at end of file; the result is the caller's to free.  Not
 * fgets() into a fixed buffer: a line longer than that used to become
 * several commands, and a rcfile line is a command. */
static char *read_line(FILE *f)
{
    ne_buffer *buf = ne_buffer_create();
    char chunk[BUFSIZ];

    while (fgets(chunk, sizeof chunk, f) != NULL) {
        ne_buffer_zappend(buf, chunk);
        if (strchr(chunk, '\n')) break;
    }

    if (buf->used == 1) {
        ne_buffer_destroy(buf);
        return NULL;
    }

    return ne_buffer_finish(buf);
}

static int init_rcfile(void)
{
    int ret = 0;
    struct stat st;
    FILE *f;

    if (rcfile == NULL) {
	rcfile = cad_home_path(".cadaverrc");
	if (rcfile == NULL) return 0;

	if (stat(rcfile, &st) != 0) {
	    NE_DEBUG(DEBUG_FILES, "No rcfile\n");
	    ne_free(rcfile);
	    return 0;
	}
    }

    f = fopen(rcfile, "r");
    if (f == NULL) {
        out_printf(_("Could not read rcfile %s: %s\n"), rcfile,
	   strerror(errno));
        cmd_failed(strerror(errno));
    } else {
	for (;;) {
            char *line = read_line(f);

            if (line == NULL) break;

            /* ne_shave() returns a pointer within the line, so the
             * allocation is what gets freed. */
            ret = execute_command(ne_shave(line, "\r\n"));
            ne_free(line);

            if (ret != 0) break;
	}
	fclose(f);
    }
    ne_free(rcfile);
    return ret;
}


#ifdef HAVE_LIBREADLINE

#define COMPLETION_CACHE_EXPIRE 10 /* seconds */

#ifndef HAVE_RL_COMPLETION_MATCHES
/* readline <4.2 compatibility. */
#define rl_completion_matches completion_matches
#define rl_filename_completion_function filename_completion_function
#endif

/* The remote completion generator is invoked multiple times to return
 * all possible matches; the remote collection listing is cached to
 * allow this, and for any future attempts at tab-completion within
 * the same collection. */
struct completion_cache {
    struct resource *list;
    time_t expiry;
    char *path; /* URI path. */
};

static void refresh_completion_cache(const char *uri_path,
                                     struct completion_cache *cache)
{
    if (cache->expiry < time(NULL) || strcmp(uri_path, cache->path)) {
        if (cache->expiry) {
            ne_free(cache->path);
            free_resource_list(cache->list);
        }

        if (fetch_resource_list(session.sess, uri_path, 1, 0,
                                &cache->list) == NE_OK) {
            cache->expiry = time(NULL) + COMPLETION_CACHE_EXPIRE;
            cache->path = ne_strdup(uri_path);
        }
        else {
            memset(cache, 0, sizeof *cache);
        }
    }
}

/* Remote filename completion generator. */
static char *remote_completion(const char *text, int state)
{
    static struct completion_cache cache;
    static struct resource *current;
    static char *text_uri_path;
    static size_t text_uri_len;
    size_t sup_len = strlen(session.uri.path);
    char *ret;

    if (state == 0) {
        /* For the initial state, refresh the completion cache after
         * determining the root path. */
        const char *sep;
        char *uri_root_path;

        /* Convert the input text to URI form for comparison against
         * the URI form of the paths within the collection. */
        if (text_uri_path) ne_free(text_uri_path);
        text_uri_path = uri_resolve_native(text);
        text_uri_len = strlen(text_uri_path);

        /* If there is a path segment in the input text, use that to
         * resolve a collection name relative to the session URI,
         * otherwise resolve against the current URI. */
        if ((sep = strrchr(text, '/')) != NULL) {
            char *native_root = ne_strndup(text, sep-text);
            uri_root_path = uri_resolve_native_coll(native_root);
            ne_free(native_root);
        }
        else {
            uri_root_path = ne_strdup(session.uri.path);
        }

        refresh_completion_cache(uri_root_path, &cache);
        current = cache.list;
        ne_free(uri_root_path);
    }

    /* For the first and subsequent invocation, iterate from the
     * current position in the resource list until a match for the
     * input text is found. */
    for (ret = NULL; current && !ret; current = current->next) {
        if (strncmp(text_uri_path, current->uri, text_uri_len) == 0) {
            /* If matching, return the path without the current path
             * prefix if the URI path is below the current path, else
             * return the absolute form. */
            if (strlen(current->uri) > sup_len
                && strncmp(current->uri, session.uri.path, sup_len) == 0)
                ret = native_path_from_uri(current->uri + sup_len);
            else
                ret = native_path_from_uri(current->uri);
        }
    }

    return ret;
}

static char **completion(const char *text, int start, int end)
{
    char **matches = NULL;
    char *sep = strchr(rl_line_buffer, ' ');

    in_completion = 1;

    if (start == 0) {
        matches = rl_completion_matches(text, command_generator);
    }
    else if (sep != NULL) {
        char *cname = ne_strndup(rl_line_buffer, sep - rl_line_buffer);
        const struct command *cmd = get_command(cname);
        enum command_scope scope =
            completion_scope(cmd, argument_index(rl_line_buffer, start));

        ne_free(cname);

        switch (scope) {
        case parmscope_none:
            break;
        case parmscope_local:
            matches = rl_completion_matches(text,
                                            rl_filename_completion_function);
            break;
        case parmscope_option:
            matches = rl_completion_matches(text, option_generator);
            break;
        case parmscope_remote:
            if (session.connected) {
                matches = rl_completion_matches(text, remote_completion);
            }
            break;
        }
    }

    in_completion = 0;

    return matches;
}

#endif /* HAVE_LIBREADLINE */

void out_state_reset(void)
{
    out_state = out_none;
}

void output(enum output_type t, const char *fmt, ...)
{
    va_list params;
    if (t == o_finish) {
	switch (out_state) {
	case out_transfer_plain:
	    out_printf("] ");
	    break;
	default:
	    out_putchar(' ');
	    break;
	}
    }
    va_start(params, fmt);
    out_vprintf(fmt, params);
    va_end(params);
    out_flush();
    switch (t) { 
    case o_start:
	out_state = out_incommand;
	break;
    case o_upload:
	out_state = out_transfer_upload;
	break;
    case o_download:
        out_state = out_transfer_download;
        break;
    case o_finish:
	out_state = out_none;
	break;
    }
}

static void init_readline(void)
{
#ifdef HAVE_LIBREADLINE
    rl_readline_name = "cadaver";
    rl_attempted_completion_function = completion;
    /* readline echoes the prompt, and the line itself when the input is
     * not a terminal, to rl_outstream.  With --json standard output
     * carries the result document and nothing else, so a prompt goes to
     * standard error along with everything else meant for a person. */
    if (out_json) rl_outstream = stderr;
#endif /* HAVE_LIBREADLINE */
}

#ifndef HAVE_LIBREADLINE
char *readline(const char *prompt)
{
    char *line, *ret;

    if (prompt) {
	out_printf("%s", prompt);
    }

    line = read_line(stdin);
    if (line == NULL) return NULL;

    ret = ne_strdup(ne_shave(line, "\r\n"));
    ne_free(line);

    return ret;
}
#endif

static void init_options(void)
{
    char *lockowner;
    const char *user = cad_user_name(), *hostname = cad_host_name();
    int utf8_default;

    if (user && hostname) {
	/* set this here so they can override it */
	lockowner = ne_concat("mailto:", user, "@", hostname, NULL);
	set_option(opt_lockowner, lockowner);
    } else {
	set_option(opt_lockowner, NULL);
    }

    set_option(opt_editor, NULL);
    set_option(opt_namespace, ne_strdup(DEFAULT_NAMESPACE));
    set_bool_option(opt_overwrite, 1);
    set_bool_option(opt_quiet, 1);
    set_bool_option(opt_searchall, 1);
    lockdepth = NE_DEPTH_INFINITE;
    lockscope = ne_lockscope_exclusive;
    searchdepth = NE_DEPTH_INFINITE;

    /* Detect whether it is possible to output UTF-8 directly.  On
     * Windows cad_system_init() has already put the console into
     * UTF-8, so this is normally true there. */
    out_charset = cad_codeset();
    utf8_default = strcmp(out_charset, "UTF-8") == 0;

#ifndef HAVE_ICONV
    if (!utf8_default) {
        fprintf(stderr, _("cadaver: Error: cadaver can only run in a locale "
                          "using the UTF-8 character encoding since iconv support "
                          "was not detected at build time.\n"));
        exit(EXIT_FAILURE);
    }
#endif

    set_option(opt_utf8, &utf8_default);
}

int main(int argc, char *argv[])
{
    int ret = 0;
    const char *home;
    char *tmp;

    /* Before anything reads argv or writes output: on Windows this
     * switches the console to UTF-8 and re-decodes the command line so
     * that a non-ASCII argument survives. */
    cad_system_init(&argc, &argv);

    progname = cad_program_name(argv[0]);

#ifdef HAVE_SETLOCALE
    setlocale(LC_ALL, "");
#endif

#ifdef ENABLE_NLS
    bindtextdomain(PACKAGE_NAME, LOCALEDIR);
    textdomain(PACKAGE_NAME);
#endif /* ENABLE_NLS */

    ne_debug_init(stderr, 0);

    home = cad_home_dir();
    if (!home) {
	/* Show me the way to go home... */
	out_printf(_("Could not determine the home directory; "
		 "set $HOME and try again.\n"));
	return -1;
    }

    ne_sock_init();

    memset(&session, 0, sizeof session);

    /* Options before rcfile, so rcfile settings can
     * override defaults */
    tmp = cad_home_path(".cadaver-locks");
    set_option(opt_lockstore, tmp);
    init_options();
    init_netrc();

    init_signals();
    init_locking();
    
    parse_args(argc, argv);

    ret = init_rcfile();

    init_readline();

    while (ret == 0) {
	char *cmd;
	cmd = read_command();
	if (cmd == NULL) {
	    /* End of input.  The newline closes the line the prompt
	     * left open, so that a transcript does not end mid-line. */
	    out_putchar('\n');
	    ret = 1;
	} else {
#ifdef HAVE_ADD_HISTORY
	    if (strcmp(cmd, "") != 0) add_history(cmd);
#endif
	    ret = execute_command(cmd);
	    free(cmd);
	}
    }

    if (session.connected) {
	close_connection();
    }

    finish_locking();

    ne_sock_exit();

    ret = run_finish();
    close_trace();

    return ret;
}

static void notifier(void *ud, ne_session_status status, const ne_session_status_info *info)
{
    int quiet = get_bool_option(opt_quiet);

    if (in_completion) return; /* do nothing during tab-completion */

    /* The dots and the progress bar are for someone watching a slow
     * transfer happen.  With --json there is nobody watching and the
     * result document has no use for a line of dots. */
    if (out_json) return;

    switch (out_state) {
    case out_none:
        if (quiet) break;

	switch (status) {
	case ne_status_lookup:
	    out_printf(_("Looking up hostname... "));
	    break;
	case ne_status_connecting:
	    out_printf(_("Connecting to server... "));
	    break;
	case ne_status_connected:
	    out_printf(_("connected.\n"));
	    break;
#if NE_MINIMUM_VERSION(0, 35)
        case ne_status_handshake:
            out_printf(_("TLS handshake completed: protocol version %s, cipher %s\n"),
                   ne_ssl_proto_name(info->hs.protocol),
                   info->hs.ciphersuite ? info->hs.ciphersuite : _("unknown"));
            break;
#endif
        default:
            break;
	}
	break;
    case out_incommand:
    case out_transfer_upload:
    case out_transfer_download:
    case out_transfer_done:
	switch (status) {
	case ne_status_connecting:
            if (!quiet) out_printf(_(" (reconnecting..."));
            /* FIXME: should reset out_state here if transfer_done */
	    break;
	case ne_status_connected:
	    if (!quiet) out_printf(_("done)"));
	    break;
        case ne_status_recving:
        case ne_status_sending:
            /* Start of transfer: */
            if ((out_state == out_transfer_download 
                 && status == ne_status_recving)
                || (out_state == out_transfer_upload 
                    && status == ne_status_sending)) {
                if (isatty(STDOUT_FILENO) && info->sr.total > 0) {
                    out_state = out_transfer_pretty;
                    out_putchar('\n');
                    pretty_progress_bar(info->sr.progress, info->sr.total);
                } else {
                    out_state = out_transfer_plain;
                    out_printf(" [.");
                }
            }
            break;                
        default:
            break;
	}
	break;
    case out_transfer_plain:
	switch (status) {
	case ne_status_connecting:
	    out_printf(_("] reconnecting: "));
	    break;
	case ne_status_connected:
	    out_printf(_("okay ["));
	    break;
        case ne_status_sending:
        case ne_status_recving:
            out_putchar('.');
            out_flush();
            if (info->sr.progress == info->sr.total) {
                out_state = out_transfer_done;
            }
            break;
        default:
            break;
	}
	break;
    case out_transfer_pretty:
	switch (status) {
	case ne_status_connecting:
	    if (!quiet) {
                out_putchar('\r');
                out_printf(_("Transfer timed out, reconnecting... "));
            }
	    break;
	case ne_status_connected:
	    if (!quiet) out_printf(_("okay."));
	    break;
        case ne_status_recving:
        case ne_status_sending:
	    pretty_progress_bar(info->sr.progress, info->sr.total);
            if (info->sr.progress == info->sr.total) {
                out_state = out_transfer_done;
            }
        default:
            break;
	}
	break;	
    }
    out_flush();
}

/* From ncftp.
   This function is (C) 1995 Mike Gleason, (mgleason@NcFTP.com)
 */
static void 
sub_timeval(struct timeval *tdiff, struct timeval *t1, struct timeval *t0)
{
    tdiff->tv_sec = t1->tv_sec - t0->tv_sec;
    tdiff->tv_usec = t1->tv_usec - t0->tv_usec;
    if (tdiff->tv_usec < 0) {
	tdiff->tv_sec--;
	tdiff->tv_usec += 1000000;
    }
}

/* Smooth progress bar.
 * Doesn't update the bar more than once every 100ms, since this 
 * might give flicker, and would be bad if we are displaying on
 * a slow link anyway.
 */
static void pretty_progress_bar(ne_off_t progress, ne_off_t total)
{
    int len, n;
    double pc;
    static struct timeval last_call = {0};
    struct timeval this_call;
    
    if (total < 0)
	return;

    if (progress < total && gettimeofday(&this_call, NULL) == 0) {
	struct timeval diff;
	sub_timeval(&diff, &this_call, &last_call);
	if (diff.tv_sec == 0 && diff.tv_usec < 100000) {
	    return;
	}
	last_call = this_call;
    }
    if (progress == 0 || total == 0) {
	pc = 0;
    } else {
	pc = (double)progress / total;
    }
    len = pc * 30;
    out_putchar('\r');
    out_printf(_("Progress: ["));
    for (n = 0; n<30; n++) {
	out_putchar((n<len-1)?'=':
		 (n==(len-1)?'>':' '));
    }
    out_printf(_("] %5.1f%% of %" NE_FMT_NE_OFF_T " bytes"), pc*100, total);
    out_flush();
}

/* Asks for the credentials the .netrc did not supply.  `known_user' and
 * `known_pass' are what it did, either or both NULL. */
static int supply_creds(const char *prompt, const char *realm, const char *hostname,
			char *username, char *password,
                        const char *known_user, const char *known_pass)
{
    char *tmp;

    switch (out_state) {
    case out_transfer_pretty:
    case out_transfer_done:
	out_putchar('\n');
        break;
    case out_none:
	break;
    case out_incommand:
    case out_transfer_upload:
    case out_transfer_download:
	out_putchar(' ');
	break;
    case out_transfer_plain:
	out_printf("] ");
	break;
    }
    out_printf(prompt, realm, hostname);

    if (known_user) {
        if (strlen(known_user) >= NE_ABUFSIZ) {
            out_printf(_("Username too long (>%d)\n"), NE_ABUFSIZ);
            return -1;
        }
        strcpy(username, known_user);
        out_printf(_("Username: %s\n"), known_user);
    }
    else {
        tmp = readline(_("Username: "));
        if (tmp == NULL) {
            out_putchar('\r'); out_printf(_("Authentication aborted!\n"));
            return -1;
        } else if (strlen(tmp) >= NE_ABUFSIZ) {
            out_putchar('\r'); out_printf(_("Username too long (>%d)\n"), NE_ABUFSIZ);
            free(tmp);
            return -1;
        }

        strcpy(username, tmp);
        free(tmp);
    }

    if (known_pass) {
        if (strlen(known_pass) >= NE_ABUFSIZ) {
            out_printf(_("Password too long (>%d)\n"), NE_ABUFSIZ);
            return -1;
        }
        strcpy(password, known_pass);
    }
    else {
        tmp = fm_getpassword(_("Password: "));
        if (tmp == NULL) {
            out_printf(_("Authentication aborted!\n"));
            return -1;
        } else if (strlen(tmp) >= NE_ABUFSIZ) {
            out_putchar('\r'); out_printf(_("Password too long (>%d)\n"), NE_ABUFSIZ);
            return -1;
        }

        strcpy(password, tmp);
    }
	
    switch (out_state) {
    case out_transfer_download:
    case out_transfer_upload:
    case out_transfer_done:
    case out_incommand:
	out_printf(_("Retrying:"));
	out_flush();
	break;
    case out_transfer_plain:
	out_printf(_("Retrying ["));
	out_flush();
	break;
    default:
	break;
    }
    return 0;
}

static int supply_creds_server(void *userdata, const char *realm, int attempt,
			       char *username, char *password)
{
    int from_netrc = server_username != NULL || server_password != NULL;

    /* A complete .netrc entry answers the first attempt without a
     * prompt.  A partial one fills in what it has and the prompt below
     * asks for the rest. */
    if (attempt == 0 && server_username && server_password) {
	ne_strnzcpy(username, server_username, NE_ABUFSIZ);
	ne_strnzcpy(password, server_password, NE_ABUFSIZ);
	return 0;
    }

    /* Two prompts, plus the .netrc attempt where there was one. */
    if (attempt > (from_netrc ? 2 : 1))
	return -1;

    return supply_creds(
	_("Authentication required for %s on server `%s':\n"), realm,
	session.uri.host, username, password,
        attempt == 0 ? server_username : NULL,
        attempt == 0 ? server_password : NULL);
}

static int supply_creds_proxy(void *userdata, const char *realm, int attempt,
			      char *username, char *password) 
{
    if (attempt > 1)
	return -1;

    return supply_creds(
	_("Authentication required for %s on proxy server `%s':\n"), realm,
	proxy_hostname, username, password, NULL, NULL);
}

