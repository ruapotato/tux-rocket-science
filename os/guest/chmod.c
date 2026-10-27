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

/* /bin/chmod — the fix for the commonest fault there is. */
#include "gsys.h"
static char arg[GARG_MAX];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    if (g_argv(arg, v) < 2) { g_putln("usage: chmod <octal> <path>"); g_exit(1); }
    g_argv_warn("chmod");
    unsigned m = 0;
    for (const char *s = v[0]; *s >= '0' && *s <= '7'; s++) m = m * 8 + (unsigned)(*s - '0');
    if (sysc(SYS_chmod, (i64)v[1], (i64)m, 0) != 0) {
        g_puts("chmod: "); g_puts(v[1]); g_putln(": no such file"); g_exit(1);
    }
    g_exit(0);
}
