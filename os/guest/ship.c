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

/* ship.c -- the ship, from the ship's computer.
 *
 * Tux Rocket Science: the one program this game adds to the machine's
 * userland (lifted from RUNBOOK's rb.c, which did the same for its world).
 * Every verb the flight controls have is one shell command away, so a script
 * on this computer can fly the ship:
 *
 *     ship                 what it says right now
 *     ship throttle 0.5    the same thing Shift does
 *     ship sas prograde    the same thing the SAS buttons do
 *     ship help            every verb there is
 *
 * The answer comes back as the game's own words. The exit code is the
 * answer too, so `if ship stage; then` works.
 */
#include "gsys.h"

#define SHIP_OUT 65536
static char out[SHIP_OUT];

void _start(void)
{
    char arg[GARG_MAX];
    if (g_getarg(arg, sizeof arg) <= 0) {
        arg[0] = 's', arg[1] = 't', arg[2] = 'a', arg[3] = 't', arg[4] = 'u', arg[5] = 's', arg[6] = 0;
    }
    i64 n = sysc(SYS_rbapi, (i64)arg, (i64)out, (i64)sizeof out);
    if (n < 0) {
        g_puts("ship: this computer is not aboard a ship\n");
        g_exit(1);
    }
    if (n >= (i64)sizeof out) n = (i64)sizeof out - 1;
    out[n] = 0;
    /* the envelope off: "+OK what it did" then the body, or "-ERR why" */
    int bad = out[0] == '-';
    i64 i = 0;
    while (i < n && out[i] != ' ' && out[i] != '\n') i++;
    if (i < n && out[i] == ' ') i++;
    i64 eol = i;
    while (eol < n && out[eol] != '\n') eol++;
    i64 body = eol < n ? eol + 1 : n, end = n;
    /* the lone dot that closes a body */
    if (end >= 2 && out[end - 1] == '\n' && out[end - 2] == '.' && (end == 2 || out[end - 3] == '\n'))
        end -= 2;
    if (body < end)
        g_write(1, out + body, (u64)(end - body));
    else if (eol > i) {
        g_write(1, out + i, (u64)(eol - i));
        g_write(1, "\n", 1);
    }
    g_exit(bad ? 1 : 0);
}
