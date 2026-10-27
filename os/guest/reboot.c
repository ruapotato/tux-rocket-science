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

/* /sbin/reboot, /sbin/halt, /sbin/poweroff — the commands everyone types.
 *
 * `init 6` worked and `reboot` did not, which is backwards: nobody types
 * `init 6` first. All three are the same act with different endings, and they
 * go through the same restart the service processor uses, so a technician
 * watching the console sees exactly what a person at the keyboard would.
 */
#include "gsys.h"

void _start(void)
{
    static char arg[64];
    g_getarg(arg, sizeof arg);

    /* argv[0] is not available here, so the behaviour is chosen by the
     * argument: bare = reboot, -h/--halt = stop. */
    int halt = 0;
    char *v[GARGS];
    int n = g_argv(arg, v);
    for (int i = 0; i < n; i++)
        if (g_streq(v[i], "-h") || g_streq(v[i], "--halt") ||
            g_streq(v[i], "-p") || g_streq(v[i], "--poweroff")) halt = 1;

    g_putln(halt ? "The system is going down for halt NOW!"
                 : "The system is going down for reboot NOW!");
    if (sysc(SYS_reboot, halt, 0, 0) != 0) {
        g_putln("reboot: cannot restart this machine");
        g_exit(1);
    }
    g_exit(0);
}
