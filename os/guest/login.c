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

/* /sbin/login — the last thing a boot does. */
#include "gsys.h"
static char b[512];
void _start(void)
{
    if (g_slurp("/etc/issue", b, sizeof b) > 0) { g_puts("\n"); g_puts(g_trim(b)); g_puts("\n"); }
    if (g_slurp("/etc/hostname", b, sizeof b) > 0) { g_puts(g_trim(b)); g_putln(" login:"); }
    else g_putln("login:");
    g_exit(0);
}
