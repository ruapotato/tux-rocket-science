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

/* /usr/sbin/zbl-install — write the boot sector.
 *
 * The boot sector is not a file. No package owns it, `pkg verify` cannot see
 * it, and no amount of reinstalling will bring it back. When the firmware
 * says "no bootable device" and every file on the disk is perfect, this is
 * the tool.
 */
#include "gsys.h"
static char arg[128];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    const char *dev = n > 0 ? v[0] : "/dev/sda";

    NomStat st;
    if (g_stat("/boot/zbl/zbl.cfg", &st) != 0) {
        g_putln("zbl-install: /boot/zbl/zbl.cfg is missing -- write a");
        g_putln("             configuration before installing the loader");
        g_exit(1);
    }
    g_bootsec(1);
    g_puts("zbl-install: boot sector written to ");
    g_putln(dev);
    g_exit(0);
}
