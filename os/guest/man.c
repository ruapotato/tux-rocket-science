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

/* /usr/bin/man — the manual, read off the disk like everything else. */
#include "gsys.h"
static char arg[128], path[192], body[8192], name[128];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    if (g_argv(arg, v) < 1) {
        g_putln("what manual page do you want?");
        g_putln("");
        for (int i = 0; i < 128; i++) {
            if (g_readdir("/usr/share/man", i, name) < 0) break;
            g_puts("  "); g_putln(name);
        }
        g_exit(1);
    }
    g_copy(path, "/usr/share/man/", sizeof path);
    g_cat(path, v[0], sizeof path);
    if (g_slurp(path, body, sizeof body) < 0) {
        g_puts("no manual entry for "); g_putln(v[0]);
        g_exit(1);
    }
    g_puts(body);
    g_exit(0);
}
