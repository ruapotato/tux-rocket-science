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
#include <stdlib.h>
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

	/* real numbers, and the maths: on the machine's own floating point */
	{
		const char *m = "print(10 / 4, 2 ** 0.5, round(sqrt(2) * 1000) / 1000, round(deg(atan2(1, 1))), 9.81 * 3, int(\"75432.9\"), float(\"1.5e3\") + 1)\n";
		computer_write(c, "/root/maths.py", m, strlen(m));
	}
	sh(c, "py /root/maths.py", &b);
	ok = has(&b, "2.5 1.414213562 1.414 45 29.43 75432 1501.0");
	failed += !ok;
	trs_log_printf(out, "%s numbers are real and the maths is there: %s", ok ? "ok  " : "FAIL", b.p);
	computer_set_state(c, "altitude\t12400.25\napoapsis\t91000\nsituation\tflying\nframe\t3\n");
	sh(c, "py -c 'print(get(\"altitude\") + 0.75, get(\"apoapsis\") + 1, get(\"situation\"), get(\"nothing\"))'", &b);
	ok = has(&b, "12401.0 91001 flying nil");
	failed += !ok;
	trs_log_printf(out, "%s get() reads a reading as the number it is (or the word): %s", ok ? "ok  " : "FAIL", b.p);
	sh(c, "py -c 'wait(5)'", &b);
	ok = has(&b, "run the script in the background");
	failed += !ok;
	trs_log_printf(out, "%s a call that waits for the ship, typed at a terminal, says to run it in the background (and does not hang)\n", ok ? "ok  " : "FAIL");

	/* THE DOT: the things aboard, as the game lays them out; f-strings,
	 * methods on strings and lists, a part's reading set and a part told
	 * to do something reaching the game as their own lines */
	computer_set_objects(c, "orbit\t@orbit:orbit\t\norbit.top\t81234.5\t\norbit.bottom\t76000\t\n"
				"engines\t@[part.4:Swivel 1|part.9:Swivel 2]\t\n"
				"part.4.name\tSwivel 1\t\npart.4.fuel\t62.5\t\npart.4.throttle\t0\tw\npart.4.ignite\t@call\t\n"
				"part.9.name\tSwivel 2\t\npart.9.fuel\t100\t\npart.9.throttle\t0\tw\n"
				"part.7.name\tTD-12 Decoupler\t\npart.7.decouple\t@call\t\npart.7.fired\tfalse\t\n");
	{
		const char *s = "e = ship.part(\"Swivel 1\")\n"
				"e.throttle = 100\n"
				"e.throttle += 5\n"
				"d = ship.part(\"TD-12 Decoupler\")\n"
				"d.decouple()\n"
				"o = {\"top\": 1}\n"
				"o.top += 2\n"
				"names = []\n"
				"for x in ship.engines:\n"
				"    names.append(x.name)\n"
				"print(f\"{ship.orbit.top / km:.1f} km, {e.fuel:.0f}% {e.throttle} {{ok}}\", \", \".join(names), o.top, len(ship.parts(\"Swivel\")))\n"
				"print(format(1234567.891, \",.2f\"), format(0.5, \".0%\"), \"[\" + format(42, \">5\") + \"]\", \"a,b\".split(\",\"), e == ship.engines[0], d.fired)\n";
		computer_write(c, "/root/dot.py", s, strlen(s));
	}
	actions(c, &a);
	sh(c, "py /root/dot.py", &b);
	ok = has(&b, "81.2 km, 62% 105 {ok} Swivel 1, Swivel 2 3 2") && has(&b, "1,234,567.89 50% [   42] [\"a\", \"b\"] true false");
	actions(c, &a);
	ok = ok && !strcmp(a.p, "set part.4.throttle 100\nset part.4.throttle 105\ncall part.7.decouple\n");
	failed += !ok;
	trs_log_printf(out, "%s the dot: parts by name, readings read and set, a thing done, f-strings and methods: %s     game got: %s",
			ok ? "ok  " : "FAIL", b.p, a.p[0] ? a.p : "(nothing)\n");
	sh(c, "py -c 'ship.part(\"Swivel\")'", &b);
	ok = has(&b, "2 parts are called 'Swivel': Swivel 1, Swivel 2");
	trs_log_printf(out, "     %s", b.p);
	sh(c, "py -c 'ship.part(\"Swivel 1\").thrust_now = 3'", &b);
	ok = ok && has(&b, "it has no 'thrust_now'");
	trs_log_printf(out, "     %s", b.p);
	sh(c, "py -c 'ship.part(\"Swivel 2\").fuel = 3'", &b);
	ok = ok && has(&b, "fuel can only be read");
	trs_log_printf(out, "     %s", b.p);
	sh(c, "py -c 'x = ship.orbit.middle'", &b);
	ok = ok && has(&b, "line 1: it has no 'middle'");
	trs_log_printf(out, "     %s", b.p);
	actions(c, &a);
	ok = ok && a.p[0] == 0;
	failed += !ok;
	trs_log_printf(out, "%s and says plainly what it can't: two parts by one name, a reading that isn't there, one that's only read\n",
			ok ? "ok  " : "FAIL");

	/* THE DEBUGGER: a script in the background says what its variables
	 * are and the line it is on; stops at a breakpoint, steps a line, and
	 * goes on when told */
	{
		const char *s = "count = 0\n"
				"best = {\"top\": 0}\n"
				"def bump(by):\n"
				"    total = count + by\n"
				"    return total\n"
				"while True:\n"
				"    count = bump(1)\n"
				"    best[\"top\"] = count * 2\n"
				"    wait(0)\n";
		computer_write(c, "/root/dbg.py", s, strlen(s));
	}
	computer_set_state(c, "time\t100\nframe\t1\napoapsis\t91000\n");
	started = computer_start(c, "/root/dbg.py", err, sizeof err);
	computer_tick(c, 60);
	Buf ls;
	buf_init(&ls);
	computer_list(c, "/run/py", &ls);
	buf_putc(&ls, 0);
	char vars[64] = "", ctl[64] = "";
	for (const char *q = ls.p; q && *q;) {
		const char *e = strchr(q, '\n');
		size_t n = e ? (size_t)(e - q) : strlen(q);
		if (n > 5 && !strncmp(q + n - 5, ".vars", 5)) {
			snprintf(vars, sizeof vars, "/run/py/%.*s", (int)n, q);
			snprintf(ctl, sizeof ctl, "/run/py/%.*s.ctl", (int)(n - 5), q);
		}
		q = e ? e + 1 : q + n;
	}
	buf_free(&ls);
	buf_clear(&b);
	computer_read(c, vars, &b);
	buf_putc(&b, 0);
	ok = started && vars[0] && has(&b, "script /root/dbg.py") && has(&b, "count\t") && has(&b, "best\t{\"top\": ");
	failed += !ok;
	trs_log_printf(out, "%s the watch: a running script's variables, live, in %s: %s", ok ? "ok  " : "FAIL", vars[0] ? vars : "(none)", b.p ? b.p : "\n");
	computer_write(c, ctl, "break 4\nmode run\nstep 0\n", 23);
	computer_tick(c, 30);
	buf_clear(&b);
	computer_read(c, vars, &b);
	buf_putc(&b, 0);
	if (getenv("TRS_DBG"))
		trs_log_printf(out, "     [after break 4] %s", b.p);
	int at4 = has(&b, "line 4\n") && has(&b, "paused at line 4 (a breakpoint)") && has(&b, "bump.by\t1");
	char before[32] = "";
	const char *cv = strstr(b.p ? b.p : "", "\ncount\t");
	if (cv)
		snprintf(before, sizeof before, "%.*s", (int)strcspn(cv + 7, "\n"), cv + 7);
	computer_tick(c, 30);
	buf_clear(&b);
	computer_read(c, vars, &b);
	buf_putc(&b, 0);
	int held = has(&b, "line 4\n");
	computer_write(c, ctl, "break 4\nmode pause\nstep 1\n", 25);
	computer_tick(c, 30);
	buf_clear(&b);
	computer_read(c, vars, &b);
	buf_putc(&b, 0);
	int stepped = has(&b, "line 5\n") && has(&b, "(a step)") && has(&b, "bump.total\t");
	computer_write(c, ctl, "mode run\nstep 1\n", 17);
	computer_tick(c, 30);
	buf_clear(&b);
	computer_read(c, vars, &b);
	buf_putc(&b, 0);
	const char *cv2 = strstr(b.p ? b.p : "", "\ncount\t");
	int went = cv2 && before[0] && atoi(cv2 + 7) > atoi(before);
	ok = at4 && held && stepped && went;
	failed += !ok;
	trs_log_printf(out, "%s breakpoints: stopped before line 4 (in bump, by = 1) and stayed; a step ran it and stopped at line 5; told to run, it went on (count %s, then %d)\n",
			ok ? "ok  " : "FAIL", before, cv2 ? atoi(cv2 + 7) : -1);
	if (!ok)
		trs_log_printf(out, "     at4 %d held %d stepped %d went %d: %s", at4, held, stepped, went, b.p);
	sh(c, "ps", &b);
	for (char *q = b.p; q && *q;) {
		char *e = strchr(q, '\n');
		if (e)
			*e = 0;
		if (strstr(q, "/bin/py")) {
			char k[40];
			snprintf(k, sizeof k, "kill %d", atoi(q));
			Buf o;
			buf_init(&o);
			computer_sh(c, k, &o);
			buf_free(&o);
		}
		q = e ? e + 1 : NULL;
	}

	computer_tick(c, 2);
	sh(c, "ps", &b);
	{
		int still = 0;
		for (char *q = b.p; q && *q;) {
			char *e = strchr(q, '\n');
			if (e)
				*e = 0;
			still |= strstr(q, "/bin/py") && strstr(q, " running ");
			q = e ? e + 1 : NULL;
		}
		ok = !still;
	}
	failed += !ok;
	trs_log_printf(out, "%s kill stops a script there and then (the Script Editor's Stop)\n", ok ? "ok  " : "FAIL");

	/* THE CONSOLE: py -i keeps its variables (and functions) from piece to
	 * piece, and prints what an expression on its own comes to */
	started = computer_start(c, "-i", err, sizeof err);
	if (!started)
		trs_log_printf(out, "     (the console would not start: %s)\n", err);
	computer_tick(c, 10);
	const char *in = "x = 21\n\x1e\nx * 2\n\x1e\ndef twice(n):\n    return n * 2\n\x1e\ntwice(x) + 0.5\n\x1e\n\"km is \" + str(km)\n\x1e\nnope(\n\x1e\nundefined_thing\n\x1e\nprint(\"still here\")\n\x1e\n";
	computer_write(c, "/run/py/console.in", in, strlen(in));
	for (int i = 0; i < 40; i++)
		computer_tick(c, 2);
	Buf con;
	buf_init(&con);
	computer_take_output(c, &con);
	buf_putc(&con, 0);
	if (getenv("TRS_DBG")) {
		Buf l2;
		buf_init(&l2);
		computer_list(c, "/run/py", &l2);
		buf_putc(&l2, 0);
		trs_log_printf(out, "     [/run/py] %s", l2.p);
		buf_free(&l2);
		sh(c, "ps", &b);
		trs_log_printf(out, "     [ps] %s", b.p);
		sh(c, "cat /run/py/console.in | wc", &b);
		trs_log_printf(out, "     [in] %s", b.p);
	}
	ok = started && has(&con, "console ready") && has(&con, "\n42\n") && has(&con, "\n42.5\n") && has(&con, "\"km is 1000\"")
			&& has(&con, "error: undefined name 'nope'") && has(&con, "error: undefined name 'undefined_thing'") && has(&con, "still here");
	failed += !ok;
	trs_log_printf(out, "%s the console (py -i): x = 21, then x * 2 says 42; a def stays for the next piece (42.5); a mistake says so and it goes on: %s",
			ok ? "ok  " : "FAIL", con.p);
	buf_free(&con);

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
