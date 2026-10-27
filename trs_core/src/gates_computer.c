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

/* test-computer: the ship's computer (os/). It boots; `ship status` prints
 * the readings the game handed it, and `ship get` one of them; a reading is
 * a file (cat /dev/ship/<key>); a verb from the shell, from a file write
 * (echo 0.5 > /dev/ship/throttle) and from a script's do() all reach the
 * game's queue as the same line, and only those (a line said twice in
 * one frame is done once); a wrong argument is
 * refused in words and queues nothing; and a script left running in the
 * background reads the ship and acts on it, slice by slice. */
#include "trs/gate.h"
#include "computer.h"

#include <stdio.h>
#include <string.h>

static int has(const Buf *b, const char *s) {
	return b->p && strstr(b->p, s) != NULL;
}

static void sh(Computer *c, const char *line, Buf *out) {
	buf_clear(out);
	computer_sh(c, line, out);
	buf_putc(out, 0);
}

static void actions(Computer *c, Buf *out) {
	buf_clear(out);
	computer_take_actions(c, out);
	buf_putc(out, 0);
}

trs_gate_status trs_gate_computer(trs_log *out, const char *data_dir) {
	(void)data_dir;
	int failed = 0;
	Computer *c = computer_new(7);
	Buf b, a;
	buf_init(&b);
	buf_init(&a);
	computer_set_verbs(c, "throttle\tn\tsets the throttle, 0 to 1\nstage\t\tfires the next stage\nsas\tw\tSAS mode\n"
			"point\tnn\tpitch and heading, degrees\nsay\tt\tsays it on the HUD\n");
	computer_set_state(c, "altitude\t12345\napoapsis\t80211\nsituation\tflying\nbody\tfloe\n");

	sh(c, "ship status", &b);
	int ok = has(&b, "altitude") && has(&b, "12345") && has(&b, "80211") && has(&b, "flying");
	sh(c, "ship get apoapsis", &b);
	ok = ok && has(&b, "80211");
	failed += !ok;
	trs_log_printf(out, "%s booted; `ship status` and `ship get apoapsis` print the game's readings\n", ok ? "ok  " : "FAIL");

	sh(c, "cat /dev/ship/altitude", &b);
	ok = has(&b, "12345");
	computer_set_state(c, "altitude\t12400\napoapsis\t80300\nsituation\tflying\nbody\tfloe\n");
	sh(c, "cat /dev/ship/altitude", &b);
	ok = ok && has(&b, "12400");
	failed += !ok;
	trs_log_printf(out, "%s a reading is a file, and it follows the ship: cat /dev/ship/altitude says 12400 after the game's update\n", ok ? "ok  " : "FAIL");

	actions(c, &a); /* (nothing yet) */
	sh(c, "ship throttle 0.5", &b);
	sh(c, "echo 1 > /dev/ship/throttle", &b);
	sh(c, "ship point 45 90", &b);
	sh(c, "ship say hello there", &b);
	sh(c, "ship point 45 90", &b); /* said again in the same frame: done once */
	actions(c, &a);
	ok = !strcmp(a.p, "throttle 0.5\nthrottle 1\npoint 45 90\nsay hello there\n");
	failed += !ok;
	trs_log_printf(out, "%s the shell, a file write and a verb with text all reach the game as its own lines: %s", ok ? "ok  " : "FAIL",
			a.p[0] ? a.p : "(nothing)\n");

	sh(c, "ship throttle full", &b);
	int refused = has(&b, "wants a number");
	sh(c, "ship jump", &b);
	refused = refused && has(&b, "no such thing");
	sh(c, "ship stage now", &b);
	refused = refused && has(&b, "no more than that");
	sh(c, "if ship throttle x; then echo yes; else echo no; fi", &b);
	refused = refused && has(&b, "no");
	actions(c, &a);
	ok = refused && a.p[0] == 0;
	failed += !ok;
	trs_log_printf(out, "%s wrong words are refused in words, with a failing exit code, and queue nothing\n", ok ? "ok  " : "FAIL");

	/* a script in the background, acting on what it reads */
	const char *script = "while True:\n"
			     "    s = json(ship())\n"
			     "    if int(s[\"apoapsis\"]) >= 90000:\n"
			     "        do(\"throttle 0\")\n"
			     "        print(\"cut at \" + s[\"apoapsis\"])\n"
			     "        exit(0)\n";
	computer_write(c, "/root/scripts/cut.py", script, strlen(script));
	char err[200];
	int started = computer_start(c, "/root/scripts/cut.py", err, sizeof err);
	computer_tick(c, 20);
	actions(c, &a);
	int idle = a.p[0] == 0;
	computer_set_state(c, "altitude\t30000\napoapsis\t91000\nsituation\tflying\nbody\tfloe\n");
	int ticks = 0;
	do {
		computer_tick(c, 2);
		actions(c, &a);
		ticks++;
	} while (!a.p[0] && ticks < 200);
	Buf said;
	buf_init(&said);
	computer_take_output(c, &said);
	buf_putc(&said, 0);
	ok = started && idle && !strcmp(a.p, "throttle 0\n") && has(&said, "cut at 91000");
	failed += !ok;
	trs_log_printf(out, "%s a script left running waits while the apoapsis is 80 km, and at 91 km cuts the throttle (after %d ticks) and says %s",
			ok ? "ok  " : "FAIL", ticks, said.p && said.p[0] ? said.p : "nothing\n");
	if (!started)
		trs_log_printf(out, "     (would not start: %s)\n", err);
	buf_free(&said);

	/* a second computer (the desktop's own, off a ship) comes and goes:
	 * this one still answers */
	computer_free(computer_new(8));
	sh(c, "ship get apoapsis", &b);
	ok = has(&b, "91000");
	failed += !ok;
	trs_log_printf(out, "%s a second computer booted and freed leaves this one answering: ship get apoapsis says %s", ok ? "ok  " : "FAIL",
			b.p && b.p[0] ? b.p : "nothing\n");

	buf_free(&b);
	buf_free(&a);
	computer_free(c);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
