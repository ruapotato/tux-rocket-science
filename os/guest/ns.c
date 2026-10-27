/*
 * Tux Rocket Science
 * Copyright (C) 2026 Tux Rocket Science contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* /bin/ns — print a process's namespace, the way Plan 9's ns(1) does.
 *
 * This is the tool for the fault where nothing is corrupt: every file matches
 * its package and the machine still reads the wrong one, because something is
 * bound over the top of it.
 */
#include "gsys.h"
static char arg[64], path[64], body[2048];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    g_copy(path, "/proc/", sizeof path);
    g_cat(path, n > 0 ? v[0] : "self", sizeof path);
    g_cat(path, "/ns", sizeof path);
    if (g_slurp(path, body, sizeof body) < 0) { g_putln("ns: no such process"); g_exit(1); }
    if (!body[0]) { g_putln("(empty namespace: every path means itself)"); g_exit(0); }
    g_puts(body);
    g_exit(0);
}
