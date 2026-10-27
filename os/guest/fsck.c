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

/* /sbin/fsck — check and repair a filesystem.
 *
 * Run against an UNMOUNTED device. Metadata it can rebuild; the contents of
 * whatever was being written when the power went it cannot, so a dirty
 * filesystem is usually two repairs: fsck, then whatever pkg verify finds.
 */
#include "gsys.h"
static char arg[128], report[2048];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    const char *dev = n > 0 ? v[0] : "/dev/sda1";

    i64 rc = sysc(SYS_fsck, (i64)dev, (i64)report, sizeof report);
    g_puts(report);
    if (rc < 0) g_exit(1);
    g_exit(0);
}
