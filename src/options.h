/* 
   cadaver, command-line DAV client
   Copyright (C) 1999-2001, Joe Orton <joe@orton.demon.co.uk>
                                                                     
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

#ifndef OPTIONS_H
#define OPTIONS_H

#include <ne_locks.h>

enum option_id {
    opt_tolerant,
    opt_keepalive,
    opt_expect100,
    opt_editor,
    opt_clicert,
    opt_clicert_uri,
    opt_namespace,
    opt_quiet,
    opt_proxy,
    opt_proxy_port,
    opt_systemproxy,
    opt_debug,
    opt_utf8,
    opt_overwrite,
    opt_lockowner,
    opt_lockstore,
    opt_lockdepth,
    opt_lockscope,
    opt_clobber,
    opt_pager,
   
    opt_searchdepth,
    opt_searchorder,
    opt_searchdorder,
    opt_searchall
};

/* What `get' does when the local file already exists.  Asking is right
 * in front of a terminal and useless in a script, where the answer
 * would be read from the command stream or from end of input; the
 * option is how a script says which it wants instead. */
enum clobber_mode {
    clobber_ask = 0,   /* prompt for another name; the default */
    clobber_yes,       /* overwrite it */
    clobber_no         /* leave it alone and fail */
};

extern enum clobber_mode clobber;

/* Sets the option from the name of a mode.  Returns non-zero if `what'
 * is not one, having said so on standard error -- it is called from the
 * command line as well as from `set'. */
int set_clobber(const char *what);

extern int lockdepth; /* current lock depth setting. */
extern int searchdepth; /* current search depth setting. */
extern enum ne_lock_scope lockscope; /* current lock scope setting. */

/* Completes an option name for readline.  See the definition for the
 * generator convention. */
char *option_generator(const char *text, int state);

void execute_set( const char *opt, const char * );
void execute_unset( const char *opt, const char * );
/* Describe option of given name */
void execute_describe(const char *name);

void *get_option( enum option_id id );
void set_option( enum option_id id, void *newval );

int get_bool_option(enum option_id id);
void set_bool_option(enum option_id id, int truth);

#endif /* OPTIONS_H */
