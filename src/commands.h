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

#ifndef COMMANDS_H
#define COMMANDS_H

#include "cadaver.h"

/* out_start(), out_result() and the rest live here, because with --json
 * they are also what records the outcome of a command. */
#include "output.h"

extern int child_running; /* true when we have a child running */

#define CMD_VARY 9999

/* Returns the command structure for the command of given name. */
const struct command *get_command(const char *name);

/* What argument number `argno' of `cmd' completes to; argument 1 is the
 * first after the command name.  parmscope_none for anything that does
 * not complete. */
enum command_scope completion_scope(const struct command *cmd, int argno);

/* Which argument of `line' the offset `start' falls in: 0 while still on
 * the command name. */
int argument_index(const char *line, int start);

/* Naming conventions used here:
 *
 * "native character encoding" is the character encoding used for
 * input/output in the terminal.
 *
 * A "native path" is relative path reference in the native character
 * encoding. Example: "../€.txt".
 *
 * A "URI path" is an absolute URI path segment (escaped UTF-8 string)
 * specifically a "path-absolute" in the RFC 3986 grammar.
 * Example: "/dav/%e2%82%ac.txt"
 */

/* Convert a URI path to a native path.  Never NULL: a path that cannot
 * be unescaped comes back as it went in. */
char *native_path_from_uri(const char *uri_path);

/* Convert a relative native path into a URI path, resolved against
 * the session URI path, e.g. "../fish food.txt" ->
 * "/dav/fish%20food.txt" */
char *uri_resolve_native(const char *native);

/* Convert a relative native path into a URI path with a trailing
 * slash. */
char *uri_resolve_native_coll(const char *native);

/* Converts a native path to a URI path, adding the trailing slash
 * only where the resource turns out to be a collection.  Costs one
 * PROPFIND, which the per-command cache in src/utils.c usually
 * answers.  `type' is filled in with what the resource turned out
 * to be when it is not NULL; resr_error means it was not found,
 * which is not the same as its being a plain resource. */
char *uri_resolve_native_true(const char *native,
                              enum resource_type *type);

/* Displays cadaver version details. */
void execute_about(void);

/* Returns owner href. */
char *getowner(void);

/* Output charset if using iconv(). */
extern const char *out_charset;

#endif /* COMMANDS_H */
