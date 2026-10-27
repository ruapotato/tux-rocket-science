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

/* The ship's computer, wired to the ship. See computer.h. Small on
 * purpose, like RUNBOOK's box.c it follows: the machine is NOMINAL's and
 * unchanged; this installs one, boots it, and answers its one outward
 * syscall from the table the game keeps up to date. */
#include "computer.h"
#include "nom.h"
#include "machine.h"
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 192
#define MAX_VERBS 64

typedef struct {
	char key[48];
	char val[96];
} entry;

typedef struct {
	char name[24];
	char args[12];
	char help[120];
} verb;

struct Computer {
	Machine m;
	entry state[MAX_KEYS];
	int nstate;
	verb verbs[MAX_VERBS];
	int nverbs;
	Buf actions;
	Buf script_out;
	size_t script_seen;
	int mounted[MAX_KEYS]; /* state keys with a /dev/ship file yet */
	int verb_dev[MAX_VERBS];
	unsigned ticks;
	Buf boot_out; /* what the system's own services say (not the scripts') */
};

/* THE BRIDGE IS ONE POINTER (kernel.c's rb_api_hook, lifted unchanged), so
 * one computer answers it: the newest. One ship is flown at a time. */
static Computer *g_comp = NULL;

static void hook(Machine *m, const char *line, Buf *out) {
	(void)m;
	if (!g_comp) {
		buf_puts(out, "-ERR this computer is not aboard a ship\n");
		return;
	}
	computer_api(g_comp, line, out);
}

static const char *state_get(Computer *c, const char *key) {
	for (int i = 0; i < c->nstate; i++)
		if (!strcmp(c->state[i].key, key))
			return c->state[i].val;
	return NULL;
}

static int is_number(const char *s) {
	char *end;
	if (!*s)
		return 0;
	strtod(s, &end);
	return *end == 0;
}

/* ---- the answers ------------------------------------------------------- */

static void ok_body(Buf *out, const char *what) {
	buf_printf(out, "+OK %s\n", what);
}

static void end_body(Buf *out) {
	buf_puts(out, ".\n");
}

static void json_str(Buf *out, const char *s) {
	buf_putc(out, '"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			buf_putc(out, '\\');
		buf_putc(out, *s);
	}
	buf_putc(out, '"');
}

void computer_api(Computer *c, const char *line, Buf *out) {
	while (*line == ' ')
		line++;
	char word[32];
	size_t k = 0;
	while (line[k] && line[k] != ' ' && k < sizeof word - 1) {
		word[k] = line[k];
		k++;
	}
	word[k] = 0;
	const char *rest = line + k;
	while (*rest == ' ')
		rest++;

	/* `ship()` in a script: the whole ship as one JSON object, for json() */
	if (!strcmp(word, "ship") || !strcmp(word, "json")) {
		ok_body(out, "ship");
		buf_putc(out, '{');
		for (int i = 0; i < c->nstate; i++) {
			if (i)
				buf_puts(out, ", ");
			json_str(out, c->state[i].key);
			buf_puts(out, ": ");
			json_str(out, c->state[i].val);
		}
		buf_puts(out, "}\n");
		end_body(out);
		return;
	}
	if (!strcmp(word, "status") || !word[0]) {
		ok_body(out, "status");
		for (int i = 0; i < c->nstate; i++)
			buf_printf(out, "%-20s %s\n", c->state[i].key, c->state[i].val);
		end_body(out);
		return;
	}
	if (!strcmp(word, "get")) {
		const char *v = state_get(c, rest);
		if (!v) {
			buf_printf(out, "-ERR no such reading: %s (ship status lists them)\n", rest);
			return;
		}
		ok_body(out, v);
		return;
	}
	/* `ship run <script>`: off it goes in the background */
	if (!strcmp(word, "run")) {
		char err[160];
		if (!*rest) {
			buf_puts(out, "-ERR run what? ship run /root/examples/ascent.py\n");
			return;
		}
		if (!computer_start(c, rest, err, sizeof err)) {
			buf_printf(out, "-ERR %s\n", err);
			return;
		}
		char said[200];
		snprintf(said, sizeof said, "running %s in the background (ps lists it, kill stops it)", rest);
		ok_body(out, said);
		return;
	}
	if (!strcmp(word, "help")) {
		ok_body(out, "help");
		buf_puts(out, "ship                  what it says right now (ship status)\n");
		buf_puts(out, "ship get <reading>    one reading: ship get apoapsis\n");
		buf_puts(out, "ship run <script>     runs a script in the background\n");
		for (int i = 0; i < c->nverbs; i++) {
			char use[64] = "";
			for (const char *a = c->verbs[i].args; *a; a++) {
				const char *w = *a == 'n' ? " <number>" : *a == 'w' ? " <word>" : *a == 't' ? " <text>" : "";
				if (a[1] == '?' && *a != '?')
					w = *a == 'n' ? " [number]" : *a == 'w' ? " [word]" : " [text]";
				if (*a != '?')
					strncat(use, w, sizeof use - strlen(use) - 1);
			}
			char lhs[64];
			snprintf(lhs, sizeof lhs, "ship %s%s", c->verbs[i].name, use);
			buf_printf(out, "%-22s%s\n", lhs, c->verbs[i].help);
		}
		buf_puts(out, "\nEvery reading is also a file: cat /dev/ship/apoapsis.\n");
		buf_puts(out, "Every verb is too: echo 1 > /dev/ship/throttle.\n");
		end_body(out);
		return;
	}
	for (int i = 0; i < c->nverbs; i++) {
		const verb *v = &c->verbs[i];
		if (strcmp(v->name, word))
			continue;
		/* the arguments against what the verb takes */
		const char *a = v->args, *p = rest;
		char tok[96];
		while (*a) {
			if (*a == '?') {
				a++;
				continue;
			}
			int optional = a[1] == '?';
			while (*p == ' ')
				p++;
			if (*a == 't') {
				if (!*p && !optional) {
					buf_printf(out, "-ERR %s wants some text (ship help)\n", v->name);
					return;
				}
				p += strlen(p);
				a++;
				continue;
			}
			size_t n = 0;
			while (p[n] && p[n] != ' ' && n < sizeof tok - 1) {
				tok[n] = p[n];
				n++;
			}
			tok[n] = 0;
			if (!n) {
				if (optional) {
					a++;
					continue;
				}
				buf_printf(out, "-ERR %s wants a %s (ship help)\n", v->name, *a == 'n' ? "number" : "word");
				return;
			}
			if (*a == 'n' && !is_number(tok)) {
				buf_printf(out, "-ERR %s wants a number, not '%s'\n", v->name, tok);
				return;
			}
			p += n;
			a++;
		}
		while (*p == ' ')
			p++;
		if (*p) {
			buf_printf(out, "-ERR %s takes no more than that: '%s' (ship help)\n", v->name, p);
			return;
		}
		char said[160];
		snprintf(said, sizeof said, "%s%s%s", v->name, *rest ? " " : "", rest);
		/* ONCE A FRAME. The ship moves on between frames, and a script loops
		 * many times inside one, reading the same readings: `stage` said in
		 * a loop until the engines light would otherwise fire every stage
		 * on the pad before the first had lit. Said again in the same
		 * frame, a command is done once. */
		int again = 0;
		size_t sl = strlen(said);
		for (const char *q = c->actions.p; q && q < c->actions.p + c->actions.len;) {
			const char *e = memchr(q, '\n', (size_t)(c->actions.p + c->actions.len - q));
			size_t ql = e ? (size_t)(e - q) : (size_t)(c->actions.p + c->actions.len - q);
			again |= ql == sl && !memcmp(q, said, sl);
			q = e ? e + 1 : c->actions.p + c->actions.len;
		}
		if (!again)
			buf_printf(&c->actions, "%s\n", said);
		ok_body(out, said);
		return;
	}
	buf_printf(out, "-ERR no such thing: '%s' (ship help)\n", word);
}

/* ---- /dev/ship ------------------------------------------------------------ */

/* One file per reading (cat it) and per verb (echo into it). Readings are
 * looked up by name, so a file whose reading has gone (no orbit on the
 * pad) says so rather than lying. */
static IoStatus dev_read(VNode *n, Buf *out, void *ctx) {
	(void)ctx;
	if (!g_comp)
		return IO_ERR;
	if (n->id >= 1000) { /* a verb: reading it says how to use it */
		int i = n->id - 1000;
		if (i < 0 || i >= g_comp->nverbs)
			return IO_ERR;
		buf_printf(out, "%s: write to it (%s)\n", g_comp->verbs[i].name, g_comp->verbs[i].help);
		return IO_OK;
	}
	const char *v = state_get(g_comp, n->name);
	buf_printf(out, "%s\n", v ? v : "");
	return IO_OK;
}

static IoStatus dev_write(VNode *n, const char *data, size_t len, void *ctx) {
	(void)ctx;
	if (!g_comp || n->id < 1000)
		return IO_ERR;
	int i = n->id - 1000;
	if (i < 0 || i >= g_comp->nverbs)
		return IO_ERR;
	char line[160];
	size_t k = (size_t)snprintf(line, sizeof line, "%s ", g_comp->verbs[i].name);
	for (size_t j = 0; j < len && k < sizeof line - 1; j++) {
		if (data[j] == '\n' || data[j] == '\r')
			break;
		line[k++] = data[j];
	}
	line[k] = 0;
	Buf out;
	buf_init(&out);
	computer_api(g_comp, line, &out);
	int bad = out.len && out.p[0] == '-';
	buf_free(&out);
	return bad ? IO_ERR : IO_OK;
}

static void mount(Computer *c) {
	Vfs *fs = &c->m.disk;
	if (!vfs_lookup(fs, "/dev/ship"))
		vfs_mkdir(fs, "/dev/ship");
	char p[NOM_PATH_MAX];
	for (int i = 0; i < c->nstate; i++) {
		snprintf(p, sizeof p, "/dev/ship/%s", c->state[i].key);
		if (!vfs_lookup(fs, p)) {
			VNode *n = vfs_mkdev(fs, p, dev_read, NULL, 0);
			if (n)
				n->mode = 0444;
		}
	}
	for (int i = 0; i < c->nverbs; i++) {
		snprintf(p, sizeof p, "/dev/ship/%s", c->verbs[i].name);
		VNode *n = vfs_lookup(fs, p);
		if (!n || n->id < 1000) {
			if (n)
				vfs_remove(fs, p);
			n = vfs_mkdev(fs, p, dev_read, dev_write, 1000 + i);
			if (n)
				n->mode = 0644;
		}
	}
}

/* ---- the machine ---------------------------------------------------------- */

Computer *computer_new(uint64_t seed) {
	Computer *c = rb_alloc(sizeof *c);
	memset(c, 0, sizeof *c);
	buf_init(&c->actions);
	buf_init(&c->script_out);
	buf_init(&c->boot_out);
	machine_install(&c->m, seed);
	machine_boot(&c->m);
	g_comp = c;
	rb_api_hook = hook;
	mount(c);
	return c;
}

void computer_free(Computer *c) {
	if (!c)
		return;
	/* (the hook stays: another computer may still be running, and it
	 * answers "no computer" by itself while none is) */
	if (g_comp == c)
		g_comp = NULL;
	machine_free(&c->m);
	buf_free(&c->actions);
	buf_free(&c->script_out);
	buf_free(&c->boot_out);
	rb_free(c);
}

void computer_sh(Computer *c, const char *line, Buf *out) {
	g_comp = c;
	kernel_run(&c->m, line, out);
}

static void split_tsv(char *line, char **f, int nf) {
	for (int i = 0; i < nf; i++) {
		f[i] = line;
		char *t = strchr(line, '\t');
		if (!t) {
			for (int j = i + 1; j < nf; j++)
				f[j] = line + strlen(line);
			return;
		}
		*t = 0;
		line = t + 1;
	}
}

void computer_set_state(Computer *c, const char *tsv) {
	c->nstate = 0;
	const char *s = tsv;
	while (*s && c->nstate < MAX_KEYS) {
		const char *e = strchr(s, '\n');
		size_t n = e ? (size_t)(e - s) : strlen(s);
		char line[160];
		if (n >= sizeof line)
			n = sizeof line - 1;
		memcpy(line, s, n);
		line[n] = 0;
		char *f[2];
		split_tsv(line, f, 2);
		if (f[0][0]) {
			entry *en = &c->state[c->nstate++];
			snprintf(en->key, sizeof en->key, "%s", f[0]);
			snprintf(en->val, sizeof en->val, "%s", f[1]);
		}
		s = e ? e + 1 : s + n;
	}
	mount(c);
}

void computer_set_verbs(Computer *c, const char *tsv) {
	c->nverbs = 0;
	const char *s = tsv;
	while (*s && c->nverbs < MAX_VERBS) {
		const char *e = strchr(s, '\n');
		size_t n = e ? (size_t)(e - s) : strlen(s);
		char line[256];
		if (n >= sizeof line)
			n = sizeof line - 1;
		memcpy(line, s, n);
		line[n] = 0;
		char *f[3];
		split_tsv(line, f, 3);
		if (f[0][0]) {
			verb *v = &c->verbs[c->nverbs++];
			snprintf(v->name, sizeof v->name, "%s", f[0]);
			snprintf(v->args, sizeof v->args, "%s", f[1]);
			snprintf(v->help, sizeof v->help, "%s", f[2]);
		}
		s = e ? e + 1 : s + n;
	}
	mount(c);
}

void computer_take_actions(Computer *c, Buf *out) {
	buf_put(out, c->actions.p, c->actions.len);
	buf_clear(&c->actions);
}

int computer_start(Computer *c, const char *path, char *err, size_t errcap) {
	g_comp = c;
	size_t plen = strlen(path);
	const char *interp = (plen > 3 && !strcmp(path + plen - 3, ".sh")) ? "/bin/sh" : "/bin/py";
	size_t before = c->script_out.len;
	int64_t rc = kernel_start_daemon(&c->m, interp, path, "script", 0, &c->script_out, NULL);
	if (rc < 0) {
		const char *why = c->script_out.len > before ? c->script_out.p + before : NULL;
		snprintf(err, errcap, "%s", why && *why ? why : "the computer would not start it");
		for (char *q = err; *q; q++)
			if (*q == '\n') {
				*q = 0;
				break;
			}
		return 0;
	}
	return 1;
}

void computer_tick(Computer *c, int slices) {
	g_comp = c;
	if (slices > 0)
		kernel_tick_some(&c->m, slices, &c->script_out, "script", 1);
	/* the system's services, a slice every 30 frames: enough for them to
	 * answer (nomde opening a window), not enough to eat the frame */
	if (++c->ticks % 30 == 0)
		kernel_tick_some(&c->m, 1, &c->boot_out, "script", 0);
}

void computer_take_output(Computer *c, Buf *out) {
	if (c->script_seen < c->script_out.len)
		buf_put(out, c->script_out.p + c->script_seen, c->script_out.len - c->script_seen);
	c->script_seen = c->script_out.len;
	if (c->script_out.len > (1u << 20)) { /* a chatty script: keep the buffer bounded */
		buf_clear(&c->script_out);
		c->script_seen = 0;
	}
}

int computer_read(Computer *c, const char *path, Buf *out) {
	g_comp = c;
	return vfs_read(&c->m.disk, path, out) == IO_OK;
}

int computer_write(Computer *c, const char *path, const char *data, size_t len) {
	g_comp = c;
	return vfs_write(&c->m.disk, path, data, len) == IO_OK;
}

void computer_list(Computer *c, const char *path, Buf *out) {
	vfs_list(&c->m.disk, path, out);
}

/* ---- the desktop's two questions (NOMINAL's de.gd asks them) ---------------- */

void computer_de_requests(Computer *c, Buf *out) {
	VNode *n = vfs_lookup(&c->m.disk, "/run/nomde/requests");
	if (!n || n->kind != VN_FILE || !n->data.len)
		return;
	buf_put(out, n->data.p, n->data.len);
	buf_clear(&n->data);
}

void computer_de_apps(Computer *c, Buf *out) {
	VNode *d = vfs_lookup(&c->m.disk, "/usr/share/applications");
	for (VNode *k = d ? d->child : NULL; k; k = k->next) {
		if (k->kind != VN_FILE)
			continue;
		size_t nl = strlen(k->name);
		if (nl < 9 || strcmp(k->name + nl - 8, ".desktop") != 0)
			continue;
		char key[64], nm[64] = "", ic[32] = "";
		size_t kl = nl - 8;
		if (kl >= sizeof key)
			kl = sizeof key - 1;
		memcpy(key, k->name, kl);
		key[kl] = 0;
		const char *p = k->data.p ? k->data.p : "";
		for (const char *q = p; q && *q;) {
			const char *e = strchr(q, '\n');
			size_t len = e ? (size_t)(e - q) : strlen(q);
			if (len > 5 && strncmp(q, "Name=", 5) == 0) {
				size_t c2 = len - 5 < sizeof nm - 1 ? len - 5 : sizeof nm - 1;
				memcpy(nm, q + 5, c2);
				nm[c2] = 0;
			} else if (len > 5 && strncmp(q, "Icon=", 5) == 0) {
				size_t c2 = len - 5 < sizeof ic - 1 ? len - 5 : sizeof ic - 1;
				memcpy(ic, q + 5, c2);
				ic[c2] = 0;
			}
			q = e ? e + 1 : NULL;
		}
		buf_printf(out, "%s\t%s\t%s\n", key, nm[0] ? nm : key, ic);
	}
}
