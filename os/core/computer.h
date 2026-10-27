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

/* The ship's computer: NOMINAL's machine (an RV64 CPU, a kernel, a disk, a
 * userland), by way of RUNBOOK's bridge, aboard a Tux Rocket Science ship.
 *
 * The game owns the ship; the computer only sees it. Each frame the game
 * hands over what the ship is doing (a table of key = value, the same
 * numbers the HUD prints) and the verbs it takes (throttle, sas, stage...).
 * On the machine that table is /dev/ship/<key> and `ship status`, and a
 * verb is `ship <verb> <args>`, `do("verb args")` in a script, or a write to
 * /dev/ship/<verb>. A verb the game declared is checked here (its argument
 * count and kind) and queued; the game takes the queue each frame and does
 * them through the same controls the keys use. Nothing on the machine can
 * reach the ship any other way. */
#ifndef TRS_COMPUTER_H
#define TRS_COMPUTER_H

#include "rb.h"

typedef struct Computer Computer;

Computer *computer_new(uint64_t seed);
void computer_free(Computer *c);

/* A shell line, run to the end, its output into out. */
void computer_sh(Computer *c, const char *line, Buf *out);
/* The ship as it is now: "key\tvalue\n" lines (replaces the table). */
void computer_set_state(Computer *c, const char *tsv);
/* The verbs: "name\targs\thelp\n" lines; args is a string of kinds, one per
 * argument: n a number, w a word, t the rest of the line; a trailing ? makes
 * the last optional. */
void computer_set_verbs(Computer *c, const char *tsv);
/* What the machine has asked of the ship since last time: one line each. */
void computer_take_actions(Computer *c, Buf *out);
/* Background work: start a script (py, or sh for *.sh) at a path on the
 * disk; run every running one for `slices` slices of the cpu; take what
 * they have printed. */
int computer_start(Computer *c, const char *path, char *err, size_t errcap);
void computer_tick(Computer *c, int slices);
void computer_take_output(Computer *c, Buf *out);
/* The disk. */
int computer_read(Computer *c, const char *path, Buf *out);
int computer_write(Computer *c, const char *path, const char *data, size_t len);
void computer_list(Computer *c, const char *path, Buf *out);
/* The desktop: its .desktop registry ("key\tName\tIcon" lines) and the
 * window requests programs have made (`open g2048`), drained. */
void computer_de_apps(Computer *c, Buf *out);
void computer_de_requests(Computer *c, Buf *out);
/* How the machine answers `ship <line>` (the same function the bridge calls). */
void computer_api(Computer *c, const char *line, Buf *out);

#endif
