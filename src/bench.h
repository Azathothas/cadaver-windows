/*
   cadaver, command-line DAV client -- the `bench' command
   Copyright (C) 2026 the cadaver-windows authors

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

#ifndef CAD_BENCH_H
#define CAD_BENCH_H

/* Measures what this connection to this server can do: the round-trip
 * time of a PROPFIND, and the rate a PUT and a GET of a generated
 * payload move at.
 *
 *   bench [SIZE [COUNT]]
 *
 * SIZE takes a K, M or G suffix, all powers of 1024, and COUNT is how
 * many times each part is repeated.  The payload goes into the current
 * collection under one name, which is deleted afterwards. */
void execute_bench(const char *size, const char *count);

#endif /* CAD_BENCH_H */
