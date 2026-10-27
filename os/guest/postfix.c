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

/* /usr/sbin/postfix — the mail daemon.
 *
 * Reads its configuration at startup and then stays running. If the config is
 * missing it says so and exits, which is a service failing to start rather
 * than a service that was never there -- a different fault with a different
 * fix.
 */
#define NOM_NEEDS_LIBZ   /* gzips what it writes */
#include "gsys.h"
static char conf[2048];
static const char *CONF[] = { "/etc/postfix/main.cf", 0 };

static void publish(void)
{
    /* the first non-comment line of the config, as loaded */
    static char state[256];
    state[0] = 0;
    char *q = conf;
    while (*q) {
        char *nl = q; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        char *t = g_trim(q);
        if (*t && *t != '#') { g_copy(state, t, sizeof state); *nl = save; break; }
        *nl = save; q = *nl ? nl + 1 : nl;
    }
    /* Two lines: which file was loaded, and what it said. The kernel compares
     * the second against the file named by the first, which is how "running
     * with a stale configuration" becomes a state the machine can notice
     * rather than a thing only a person could spot. */
    int fd = g_open("/run/postfix.state", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        /* A daemon that cannot write its state file is not running properly,
         * whatever it thinks. This used to return quietly, which meant that
         * deleting /run -- something a careless cleanup really does -- left
         * every service reporting itself healthy while the machine had no
         * idea what any of them had loaded. Silence here is how a fault stops
         * being a fault. */
        g_puts("postfix: ");
        g_puts("/run/postfix.state");
        g_putln(": cannot write state -- refusing to start");
        g_exit(1);
    }
    sysc(SYS_write, fd, (i64)CONF[0], (i64)g_strlen(CONF[0]));
    sysc(SYS_write, fd, (i64)"\n", 1);
    sysc(SYS_write, fd, (i64)state, (i64)g_strlen(state));
    sysc(SYS_write, fd, (i64)"\n", 1);
    g_close(fd);
}

static const char *KEY = "myhostname";
void _start(void)
{
    for (int i = 0; CONF[i]; i++) {
        if (g_slurp(CONF[i], conf, sizeof conf) < 0) {
            g_puts("postfix: ");
            g_puts(CONF[i]);
            g_putln(": cannot read -- refusing to start");
            g_exit(1);
        }
    }
    /* The config is there. Is it USABLE? A file that exists and does not say
     * the one thing this daemon needs is a completely different fault from a
     * file that is missing, and it fails later and less obviously. */
    {
        int ok = 0;
        char *q = conf;
        while (*q) {
            char *nl = q; while (*nl && *nl != '\n') nl++;
            char save = *nl; *nl = 0;
            char *t = g_trim(q);
            if (*t && *t != '#') {
                u64 k = 0;
                while (KEY[k] && t[k] == KEY[k]) k++;
                if (!KEY[k]) ok = 1;      /* empty KEY: any real line will do */
            }
            *nl = save; q = *nl ? nl + 1 : nl;
            if (ok) break;
        }
        if (!ok) {
            g_puts("postfix: ");
            g_puts(CONF[0]);
            g_putln(": no hostname configured -- refusing to start");
            g_exit(1);
        }
    }

    /* Publish what was actually loaded. The file on disk says what the
     * machine is SUPPOSED to do; this says what the running process is
     * actually doing, and the two drift the moment somebody edits a config
     * and does not reload. That gap is invisible without this. */
    publish();

    /* Up. A daemon spends its life here, looking occasionally to see whether
     * anyone has asked it to re-read its configuration. */
    for (;;) {
        if (g_sigpend() == SIG_HUP) {
            if (g_slurp(CONF[0], conf, sizeof conf) >= 0) publish();
        }
    }
}
