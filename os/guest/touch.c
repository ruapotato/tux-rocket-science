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

/* /bin/touch — create an empty file if it is not there. */
#include "gsys.h"
static char arg[GARG_MAX];
void _start(void){
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    if (n < 1) { g_putln("usage: touch <path>..."); g_exit(1); }
    g_argv_warn("touch");
    /* EVERY path. It created v[0] and dropped the rest without a word, so
     * `touch a b c` reported success having made one file, and `ls` a moment
     * later disagreed with the command that had just succeeded. */
    int bad = 0;
    for (int i = 0; i < n; i++) {
        int fd = g_open(v[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            g_puts("touch: "); g_puts(v[i]); g_putln(": cannot create");
            bad = 1;
            continue;
        }
        g_close(fd);
    }
    g_exit(bad);
}
