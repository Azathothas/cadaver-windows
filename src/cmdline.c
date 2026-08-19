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

#include "config.h"

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <stdio.h>

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>

#include <ne_string.h>
#include <ne_alloc.h>

#include "i18n.h"
#include "glob.h"
#include "basename.h"

#include "common.h"
#include "commands.h"
#include "cadaver.h"
#include "cmdline.h"
#include "utils.h"

static int has_glob_pattern(const char *str) {
    const char *pnt;
    for (pnt = str; *pnt != '\0'; pnt++)
	if (*pnt=='*' || *pnt=='[' || *pnt=='?')
	    return 1;
    return 0;
}

/* Whether a backslash quotes the character after it.
 *
 * On Windows it does not: the backslash is the path separator, and
 * every rule that made it an escape as well turned some ordinary path
 * into something else.  Quoting a character with a backslash before it
 * would leave `lcd C:\Users\me' unusable; quoting only the characters
 * that could not be meant literally still breaks `lls C:\dir\*', where
 * the star is a wildcard, and `lcd C:\a\#b', where the hash is part of
 * a directory name.  Double or single quotes are the way to write a
 * name containing a space or a hash there, and a Windows file name
 * cannot contain a quote character at all, so nothing is left without
 * a spelling.
 *
 * Everywhere else a backslash quotes anything, as a shell does. */
#ifdef _WIN32
#define BACKSLASH_QUOTES (0)
#else
#define BACKSLASH_QUOTES (1)
#endif

/* Gets the next token for parse_command...
 * Starts at position in line.
 * DFA states:
 *   0: chewing leading whitespace
 *   1: chewing characters, normally
 *   2: chewing characters in a quote
 *   3: just got a backslash in normal chew
 *   4: just got a backslash in a quoted chew
 *   8: ignoring comment
 *   9: acceptance state
 * 
 * State diagram is left as an exercise to the reader, since 
 * I'm not going to draw it in ASCII. ;)
 *
 * Returns the token, malloc()-allocated, or NULL on end-of-line.
 * Position updated to point after token.
 */
static char *gettoken(const char *line, const char **pointer)
{
    const char *pnt;
    int state = 0;
    char quote = 0; /* = 0 to keep gcc -Wall happy */
    /* Grown to fit rather than a fixed BUFSIZ array: a token that long
     * used to make this return NULL, which the caller reads as end of
     * line, so the rest of the command was dropped without a word.
     * BUFSIZ is 512 on Windows, which a deep path with escaped UTF-8 in
     * it reaches without trying. */
    ne_buffer *buf = ne_buffer_create();
    size_t pos;
    char *token;

    pnt = *pointer;

#define ISQUOTE(x) (x=='\'' || x=='\"')
#define ISWHITE(x) (x==' ' || x=='\t')
#define KEEP(c) ne_buffer_append(buf, &(c), 1)

     while (*pnt != '\0' && state != 9) {
	 switch (state) {
	 case 0: /* leading whitespace chew */
	     if (ISQUOTE(*pnt)) {
		 state = 2;
		 quote = *pnt;
	     } else if (*pnt == '#') {
		 state = 8;
	     } else if (!ISWHITE(*pnt)) {
		 KEEP(*pnt);
		 state = 1;
	     }
	     break;
	 case 1: /* normal chew */
	     if (ISWHITE(*pnt)) {
		 state = 9;
	     } else if (*pnt == '#') {
		 state = 8;
	     } else if (BACKSLASH_QUOTES && *pnt == '\\') {
		 state = 3;
	     } else {
		 KEEP(*pnt);
	     }
	     break;
	 case 2: /* quoted chew */
	     if (*pnt == quote) {
		 state = 9;
	     } else if (BACKSLASH_QUOTES && *pnt == '\\') {
                state = 4;
	     } else {
		 KEEP(*pnt);
	     }
	     break;
	 case 3: /* chew an escaped literal */
	     KEEP(*pnt);
	     state = 1;
	     break;
	 case 4: /* backslash in quoted chew... like 3 except
		  * we switch back to state 2 afterwards */
	     KEEP(*pnt);
	     state = 2;
	     break;
	 case 5: /* comment chew */
	     break;
	 }
	 pnt++;
     }

#undef ISQUOTE
#undef ISWHITE
#undef KEEP

     /* A backslash at the end of the line quotes nothing, so it stands
      * for itself. */
     if (state == 3 || state == 4) ne_buffer_czappend(buf, "\\");

     pos = buf->used - 1;
     token = ne_buffer_finish(buf);
     *pointer = pnt;
     if (pos > 0) {
#ifdef I_AM_A_LUMBERJACK
	 /* a little hack; does env. var expansion...
	  * 1) is this really useful?
	  * 2) this should be done in parse_command not gettoken
	  */
	 if (token[0] == '$') {
	     char *val = getenv(&token[1]);
	     if (val) {
                 ne_free(token);
                 return ne_strdup(val);
             }
	 }
#endif
	 return token;
     } else {
         ne_free(token);
	 return NULL;
     }
}

volatile int interrupt_state; /* for glob */

struct dg_ctx {
    size_t rootlen;
    struct resource *files;
    struct resource *current;
    struct dirent ent;
};

static void *davglob_opendir(const char *dir) 
{
    struct dg_ctx *ctx = NULL;
    struct resource *files;
    char *uri_path = uri_resolve_native_coll(dir);
    NE_DEBUG(DEBUG_FILES, "opendir: %s\n", uri_path);
    switch (fetch_resource_list(session.sess, uri_path, 1, 0, &files)) {
    case NE_OK:
	ctx = ne_calloc(sizeof *ctx);
	ctx->rootlen = strlen(uri_path);
	ctx->files = files;
	ctx->current = ctx->files;
	break;
    case NE_AUTH:
	errno = EACCES;
	break;
    default:
	/* Let them know it doesn't exist. */
	errno = ENOENT;
	break;
    }	
    ne_free(uri_path);
    return (void *)ctx;
}

/* Mocks up a dummy dirent structure */
static struct dirent *davglob_readdir(void *dir)
{
    struct dg_ctx *ctx = dir;
    const char *uri_segment;
    char *native_segment;

    while (ctx->current && strlen(ctx->current->uri) <= ctx->rootlen) {
        NE_DEBUG(DEBUG_FILES, "readdir: path %s shorter than root, ignoring\n",
                 ctx->current->uri);
        ctx->current = ctx->current->next;
    }

    if (!ctx->current) { 
	NE_DEBUG(DEBUG_FILES, "readdir: end of list.\n");
	return NULL;
    }

    uri_segment = ctx->current->uri + ctx->rootlen;
    native_segment = native_path_from_uri(uri_segment);
    memset(&ctx->ent, 0, sizeof ctx->ent);
    ne_strnzcpy(ctx->ent.d_name, native_segment, sizeof ctx->ent.d_name);
    ne_free(native_segment);

    /* Nothing else in the dirent is read: lib/glob.c asks gl_stat
     * whether an entry is a collection rather than trusting d_type,
     * which the Windows dirent does not have. */

    NE_DEBUG(DEBUG_FILES, "readdir: native entry %s\n", ctx->ent.d_name);
    ctx->current = ctx->current->next;
    return &ctx->ent;
}

static void davglob_closedir(void *dir) 
{
    struct dg_ctx *ctx = dir;
    NE_DEBUG(DEBUG_FILES, "closedir\n");
    free_resource_list(ctx->files);
    free(ctx);
}

static int davglob_stat(const char *filename, struct stat *st) {
    /* presumption: all glob needs to know is whether it's a directory
     * or not. I think this is true for the glob in glibc2 */
    char *uri_path = uri_resolve_native_coll(filename);
    NE_DEBUG(DEBUG_FILES, "stat %s\n", filename);
    if (getrestype(uri_path) == resr_collection) {
	st->st_mode = S_IFDIR;
    } else {
	st->st_mode = S_IFREG;
    }
    ne_free(uri_path);
    return 0;
}

static void davglob_interrupt(int sig) {
    interrupt_state = 1;
}

static int davglob_errfunc(const char *filename, int errcode) 
{
    output(o_finish, "Error on %s: %s\n", filename, strerror(errcode));
    cmd_failed(strerror(errcode));
    return 0;
}

char **parse_command(const char *line, int *count) 
{ 
    char *token, **tokens = NULL;
    const char *pnt = line;
    int numtokens = 0;
    const struct command *cmd = NULL;

#define ADDTOK(x) 						\
do {								\
    tokens = realloc(tokens, ++numtokens*sizeof(char *));	\
    tokens[numtokens-1] = x;					\
} while (0)

    while ((token = gettoken(line, &pnt)) != NULL) {
	if (!numtokens) {
	    /* The first token: get the command */
	    cmd = get_command(token);
	    ADDTOK(token);
	} else if (has_glob_pattern(token) && cmd &&
		   ((cmd->scope == parmscope_remote && session.connected) || 
		     (cmd->scope == parmscope_local))) {
	    /* Let us Glob */
	    glob_t gl = {0};
	    int ret, flags = 0;
	    void (*oldhand)(int sig);

	    output(o_start, _("[Matching..."));
	    interrupt_state = 0;

	    /* A remote expansion makes one PROPFIND per collection it
	     * descends into, so it can take a while against a slow
	     * server.  lib/glob.c polls gl_abort between directory
	     * reads and between entries, and setting the flag is all
	     * the handler below has to do. */
	    gl.gl_abort = &interrupt_state;
	    oldhand = signal(SIGINT, davglob_interrupt);

	    /* This is nice. We expand the glob in the same way
	     * whether it is a local or a remote glob, except we lob
	     * in the remote-glob handlers here if the command
	     * requires remote globs to be expanded */
	    if (cmd->scope == parmscope_remote) {
		gl.gl_closedir = davglob_closedir;
		gl.gl_opendir = davglob_opendir;
		gl.gl_readdir = davglob_readdir;
		gl.gl_stat = davglob_stat;
		flags |= GLOB_ALTDIRFUNC;
	    }
	    /* Do the Glob Thang */
	    ret = glob(token, flags, davglob_errfunc, &gl);
	    switch (ret) {
	    case 0: {
		unsigned int n;
		if (gl.gl_pathc > 1) {
		    output(o_finish, _("%ld matches.]\n"), (long)gl.gl_pathc);
		} else {
		    output(o_finish, _("1 match.]\n"));
		}
		for (n = 0; n < gl.gl_pathc; n++) {
                    ADDTOK(ne_strdup(gl.gl_pathv[n]));
		}
	    } break;
	    case GLOB_NOSPACE:
		output(o_finish, "failed: out of memory.]\n");
                cmd_failed(_("out of memory expanding a wildcard"));
		break;
	    case GLOB_ABORTED:
		output(o_finish, "aborted]\n");
                cmd_failed(_("interrupted expanding a wildcard"));
		break;
	    case GLOB_NOMATCH:
                /* Not a failure in itself: the pattern is passed
                 * through unexpanded, as a shell does, and the command
                 * then reports what happened to it. */
		output(o_finish, "no matches.]\n");
		break;
	    default:
		output(o_finish, "failed.]\n");
                cmd_failed(_("could not expand a wildcard"));
	    }
	    if (ret) {
		/* For all the failure cases, put in the glob instead.
		 * Perhaps we should fail here instead... but, this is
		 * what bash does, so we stick with consistent
		 * behaviour. TODO: this could be an option.
		 */
		ADDTOK(token);
	    } else {
		/* Otherwise, we don't use the actual token */
		free(token);
	    }
	    globfree(&gl);
	    signal(SIGINT, oldhand);
	} else {
	    ADDTOK(token);
	}
    }

    *count = numtokens;
    /* add a NULL at the end of the list */
    ADDTOK(NULL);

    return tokens;
}
