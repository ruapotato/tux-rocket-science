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

/* /bin/wc — count lines, words and bytes. Reads stdin when given no file, so
 * it is useful at the end of a pipeline, which is where wc lives.
 *
 * `wc -l f` answered "wc: -l: cannot read", because every argument was a
 * filename. -l is the flag people actually type -- more often than they type
 * wc at all -- and being told the flag is a missing file is the worst
 * available answer, since it implicates the file too.
 */
#include "gsys.h"

static char arg[GARG_MAX], buf[4096];
static char *v[GARGS];
static int want_l, want_w, want_c;

static void count(int fd, const char *label)
{
    i64 lines = 0, words = 0, bytes = 0, inword = 0, last = '\n';
    for (;;) {
        i64 n = g_read(fd, buf, sizeof buf);
        if (n <= 0) break;
        bytes += n;
        for (i64 i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') lines++;
            int sp = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');
            if (!sp && !inword) { words++; inword = 1; }
            else if (sp) inword = 0;
            last = ch;
        }
    }
    /* A last line with no newline on it is still a line. */
    if (bytes && last != '\n') lines++;

    /* With no flag you get all three, which is what wc is. With flags you get
     * exactly the ones you asked for, in wc's own order. */
    int all = !want_l && !want_w && !want_c;
    if (all || want_l) { g_puts("  "); g_putn(lines); }
    if (all || want_w) { g_puts("  "); g_putn(words); }
    if (all || want_c) { g_puts("  "); g_putn(bytes); }
    if (label) { g_puts("  "); g_puts(label); }
    g_puts("\n");
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    int n = g_argv(arg, v);
    g_argv_warn("wc");

    int nop = 0;
    for (int i = 0; i < n; i++) {
        if (v[i][0] != '-' || !v[i][1]) { v[nop++] = v[i]; continue; }
        int bad = 0;
        for (const char *f = v[i] + 1; *f; f++) {
            if (*f == 'l') want_l = 1;
            else if (*f == 'w') want_w = 1;
            else if (*f == 'c' || *f == 'm') want_c = 1;   /* no multibyte here */
            else bad = 1;
        }
        if (bad) {
            g_puts("wc: "); g_puts(v[i]); g_putln(": not a flag this wc has");
            g_putln("usage: wc [-l] [-w] [-c] [file ...]");
            g_exit(2);
        }
    }

    if (nop == 0) { count(0, 0); g_exit(0); }

    int rc = 0;
    for (int i = 0; i < nop; i++) {
        int fd = g_open(v[i], O_RDONLY);
        if (fd < 0) {
            g_puts("wc: "); g_puts(v[i]); g_putln(": cannot read");
            rc = 1;
            continue;
        }
        count(fd, nop > 1 ? v[i] : 0);
        g_close(fd);
    }
    g_exit(rc);
}
