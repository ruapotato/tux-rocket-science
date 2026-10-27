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

/* /bin/rev — reverse each line.
 *
 * The smallest useful filter there is, and it earns its place twice. It is a
 * joke you can type at a prompt, and it is the shortest possible proof that
 * pipelines on this machine really carry bytes: `cat f | rev | rev` gives back
 * `cat f`, and if it does not, the pipe is what is broken and not your eyes.
 *
 *   rev [file ...]      with no file it reads stdin, which is the point
 *
 * The trailing newline is left where it was. Reversing it into the middle of
 * the next line would be funny exactly once.
 */
#include "gsys.h"

static char arg[GARG_MAX];
static char body[65536];
static char out[65536];

static void rev_buf(char *b, i64 len)
{
    i64 o = 0;
    i64 start = 0;
    for (i64 i = 0; i <= len; i++) {
        if (i == len || b[i] == '\n') {
            /* Strip a \r so a file that came off a DOS machine reverses to
             * something readable rather than to something with a carriage
             * return at the FRONT of every line. */
            i64 end = i;
            if (end > start && b[end - 1] == '\r') end--;
            for (i64 j = end - 1; j >= start; j--) out[o++] = b[j];
            if (i < len) out[o++] = '\n';
            start = i + 1;
        }
        if (o > (i64)sizeof out - 4) break;
    }
    g_write(1, out, (u64)o);
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    g_argv_warn("rev");

    if (n == 0) {
        i64 len = g_slurp_stdin(body, sizeof body);
        if (len > 0) rev_buf(body, len);
        g_exit(0);
    }

    int rc = 0;
    for (int i = 0; i < n; i++) {
        i64 len = g_slurp(v[i], body, sizeof body);
        if (len < 0) {
            g_puts("rev: ");
            g_puts(v[i]);
            g_putln(": cannot read");
            rc = 1;
            continue;
        }
        rev_buf(body, len);
    }
    g_exit(rc);
}
