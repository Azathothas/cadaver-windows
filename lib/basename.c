/* basename.c -- return the last element in a path
   Copyright (C) 1990, 1998, 1999 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <basename.h>

/* Windows has two path separators and a drive prefix, so the last
   element of "C:\dir\file" is "file" rather than the whole string.
   base_name() is applied to URI paths as well as to local ones, but a
   backslash in a URI path has been percent-encoded by the time it gets
   here, so treating it as a separator is right in both cases.  */
#ifndef FILESYSTEM_PREFIX_LEN
# ifdef _WIN32
#  define FILESYSTEM_PREFIX_LEN(Filename) \
     (((Filename)[0] != 0 && (Filename)[1] == ':') ? 2 : 0)
# else
#  define FILESYSTEM_PREFIX_LEN(Filename) 0
# endif
#endif

#ifndef ISSLASH
# ifdef _WIN32
#  define ISSLASH(C) ((C) == '/' || (C) == '\\')
# else
#  define ISSLASH(C) ((C) == '/')
# endif
#endif

/* In general, we can't use the builtin `basename' function if available,
   since it has different meanings in different environments.
   In some environments the builtin `basename' modifies its argument.
   If NAME is all slashes, be sure to return `/'.  */

char *
base_name (char const *name)
{
  char const *base = name += FILESYSTEM_PREFIX_LEN (name);
  int all_slashes = 1;
  char const *p;

  for (p = name; *p; p++)
    {
      if (ISSLASH (*p))
	base = p + 1;
      else
	all_slashes = 0;
    }

  /* If NAME is all slashes, arrange to return `/'.  */
  if (*base == '\0' && ISSLASH (*name) && all_slashes)
    --base;

  return (char *) base;
}
