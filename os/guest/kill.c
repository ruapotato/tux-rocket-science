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

/* /bin/kill — send a signal.
 *
 *   kill -HUP <pid>    tell it to re-read its configuration
 *   kill -TERM <pid>   ask it to stop
 *
 * Nothing is interrupted: there is no preemption on this machine, so the
 * signal sits pending until the process next looks. A well-behaved daemon
 * looks often. That is a smaller promise than a real kernel makes and it is
 * exactly enough for the thing signals are actually used for.
 */
#include "gsys.h"
static char arg[128];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    int sig = SIG_TERM, ai = 0;
    if (n >= 1 && v[0][0] == '-') {
        const char *s = v[0] + 1;
        if (g_streq(s, "HUP") || g_streq(s, "1"))       sig = SIG_HUP;
        else if (g_streq(s, "TERM") || g_streq(s, "15")) sig = SIG_TERM;
        else { g_putln("kill: only -HUP and -TERM are supported"); g_exit(1); }
        ai = 1;
    }
    if (n < ai + 1) { g_putln("usage: kill [-HUP|-TERM] <pid>"); g_exit(1); }
    int pid = 0;
    for (const char *s = v[ai]; *s >= '0' && *s <= '9'; s++) pid = pid * 10 + (*s - '0');
    if (g_kill(pid, sig) != 0) {
        g_puts("kill: no such running process: ");
        g_putln(v[ai]);
        g_exit(1);
    }
    g_exit(0);
}
