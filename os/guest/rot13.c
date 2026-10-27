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

/* /usr/bin/rot13 — rotate letters by thirteen.
 *
 * The oldest joke in the business and the only cipher that is its own undo,
 * which is exactly why people who are not hiding anything from anybody use it
 * anyway: to stop a thing being readable by ACCIDENT. A punchline, a spoiler,
 * a sentence you did not want the person walking past your desk to read over
 * your shoulder.
 *
 *   rot13 [file ...]     with no file it reads stdin
 *
 * It is not encryption and this page will not pretend otherwise. Anybody who
 * can run `rot13` can undo `rot13`, because running it twice is the undo.
 * Digits, punctuation and whitespace are left exactly as they are, so a file
 * that has been through it still has its shape -- which is how you can tell
 * at a glance that something is rot13 and not damage.
 */
#include "gsys.h"

static char arg[GARG_MAX];
static char body[65536];

static void rot(char *b, i64 len)
{
    for (i64 i = 0; i < len; i++) {
        char c = b[i];
        if (c >= 'a' && c <= 'z')      b[i] = (char)('a' + (c - 'a' + 13) % 26);
        else if (c >= 'A' && c <= 'Z') b[i] = (char)('A' + (c - 'A' + 13) % 26);
    }
    g_write(1, b, (u64)len);
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    g_argv_warn("rot13");

    if (n == 0) {
        i64 len = g_slurp_stdin(body, sizeof body);
        if (len > 0) rot(body, len);
        g_exit(0);
    }

    int rc = 0;
    for (int i = 0; i < n; i++) {
        i64 len = g_slurp(v[i], body, sizeof body);
        if (len < 0) {
            g_puts("rot13: ");
            g_puts(v[i]);
            g_putln(": cannot read");
            rc = 1;
            continue;
        }
        rot(body, len);
    }
    g_exit(rc);
}
