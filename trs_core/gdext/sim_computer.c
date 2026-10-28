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

/* TrsComputer: the ship's computer (os/core/computer.c) for GDScript. The
 * desktop (scripts/os) talks to it with the same few calls NOMINAL's did
 * (sh_on, booted, de_apps, de_requests); the flight scene feeds it the ship
 * (set_state, set_verbs) and takes what it asks for (take_actions). */
#include "binder.h"
#include "computer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	Computer *c;
} comp_handle;

void *trs_computer_create(void) {
	return calloc(1, sizeof(comp_handle));
}

void trs_computer_destroy(void *self) {
	comp_handle *h = self;
	computer_free(h->c);
	free(h);
}

static Computer *C(void *self) {
	return ((comp_handle *)self)->c;
}

static void ret_buf(trs_val *r, Buf *b) {
	r->s = trs_strdup_printf("%.*s", (int)b->len, b->p ? b->p : "");
	buf_free(b);
}

static void m_boot(void *self, const trs_val *a, trs_val *r) {
	comp_handle *h = self;
	if (!h->c)
		h->c = computer_new((uint64_t)a[0].i);
	r->b = h->c != NULL;
}

static void m_booted(void *self, const trs_val *a, trs_val *r) {
	(void)a;
	r->b = C(self) != NULL;
}

static void m_sh(void *self, const trs_val *a, trs_val *r) {
	Buf b;
	buf_init(&b);
	if (C(self))
		computer_sh(C(self), a[0].s, &b);
	ret_buf(r, &b);
}

/* sh_on(which, line): NOMINAL's desktop asks a machine by number; there is one */
static void m_sh_on(void *self, const trs_val *a, trs_val *r) {
	Buf b;
	buf_init(&b);
	if (C(self))
		computer_sh(C(self), a[1].s, &b);
	ret_buf(r, &b);
}

static void m_set_state(void *self, const trs_val *a, trs_val *r) {
	(void)r;
	if (C(self))
		computer_set_state(C(self), a[0].s);
}

static void m_set_objects(void *self, const trs_val *a, trs_val *r) {
	(void)r;
	if (C(self))
		computer_set_objects(C(self), a[0].s);
}

static void m_set_verbs(void *self, const trs_val *a, trs_val *r) {
	(void)r;
	if (C(self))
		computer_set_verbs(C(self), a[0].s);
}

static void m_take_actions(void *self, const trs_val *a, trs_val *r) {
	(void)a;
	Buf b;
	buf_init(&b);
	if (C(self))
		computer_take_actions(C(self), &b);
	ret_buf(r, &b);
}

static void m_api(void *self, const trs_val *a, trs_val *r) {
	Buf b;
	buf_init(&b);
	if (C(self))
		computer_api(C(self), a[0].s, &b);
	ret_buf(r, &b);
}

/* start(path) -> "" if it is running, else why not */
static void m_start(void *self, const trs_val *a, trs_val *r) {
	char err[200] = "";
	if (!C(self))
		snprintf(err, sizeof err, "the computer is off");
	else if (computer_start(C(self), a[0].s, err, sizeof err))
		err[0] = 0;
	r->s = trs_strdup_printf("%s", err);
}

static void m_tick(void *self, const trs_val *a, trs_val *r) {
	(void)r;
	if (C(self))
		computer_tick(C(self), (int)a[0].i);
}

static void m_take_output(void *self, const trs_val *a, trs_val *r) {
	(void)a;
	Buf b;
	buf_init(&b);
	if (C(self))
		computer_take_output(C(self), &b);
	ret_buf(r, &b);
}

static void m_scripts(void *self, const trs_val *a, trs_val *r) {
	(void)a;
	Buf b;
	buf_init(&b);
	if (C(self))
		computer_scripts(C(self), &b);
	ret_buf(r, &b);
}

static void m_read_file(void *self, const trs_val *a, trs_val *r) {
	Buf b;
	buf_init(&b);
	if (C(self))
		computer_read(C(self), a[0].s, &b);
	ret_buf(r, &b);
}

static void m_write_file(void *self, const trs_val *a, trs_val *r) {
	r->b = C(self) && computer_write(C(self), a[0].s, a[1].s, (size_t)a[1].slen);
}

static void m_list_dir(void *self, const trs_val *a, trs_val *r) {
	Buf b;
	buf_init(&b);
	if (C(self))
		computer_list(C(self), a[0].s, &b);
	ret_buf(r, &b);
}

static void m_de_apps(void *self, const trs_val *a, trs_val *r) {
	(void)a;
	Buf b;
	buf_init(&b);
	if (C(self))
		computer_de_apps(C(self), &b);
	ret_buf(r, &b);
}

static void m_de_requests(void *self, const trs_val *a, trs_val *r) {
	(void)a;
	Buf b;
	buf_init(&b);
	if (C(self))
		computer_de_requests(C(self), &b);
	ret_buf(r, &b);
}

/* NOMINAL's desktop also asks these: install (its fit-the-computer call),
 * the peer address and whether it is on rescue media. There is one
 * computer, fitted and running. */
static void m_install(void *self, const trs_val *a, trs_val *r) {
	(void)r;
	comp_handle *h = self;
	if (!h->c)
		h->c = computer_new((uint64_t)a[0].i);
}

static void m_peer_addr(void *self, const trs_val *a, trs_val *r) {
	(void)self, (void)a;
	r->s = trs_strdup_printf("%s", "");
}

static void m_on_rescue(void *self, const trs_val *a, trs_val *r) {
	(void)self, (void)a;
	r->b = 0;
}

#define S TRS_T_STRING
#define I TRS_T_INT
#define B TRS_T_BOOL
#define V TRS_T_VOID
const trs_method_def TRS_COMPUTER_METHODS[] = {
	{ "boot", m_boot, B, 1, { I }, { "seed" } },
	{ "booted", m_booted, B, 0, { 0 }, { 0 } },
	{ "sh", m_sh, S, 1, { S }, { "line" } },
	{ "sh_on", m_sh_on, S, 2, { I, S }, { "which", "line" } },
	{ "set_state", m_set_state, V, 1, { S }, { "tsv" } },
	{ "set_verbs", m_set_verbs, V, 1, { S }, { "tsv" } },
	{ "set_objects", m_set_objects, V, 1, { S }, { "tsv" } },
	{ "take_actions", m_take_actions, S, 0, { 0 }, { 0 } },
	{ "api", m_api, S, 1, { S }, { "line" } },
	{ "start", m_start, S, 1, { S }, { "path" } },
	{ "tick", m_tick, V, 1, { I }, { "slices" } },
	{ "take_output", m_take_output, S, 0, { 0 }, { 0 } },
	{ "scripts", m_scripts, S, 0, { 0 }, { 0 } },
	{ "read_file", m_read_file, S, 1, { S }, { "path" } },
	{ "write_file", m_write_file, B, 2, { S, S }, { "path", "data" } },
	{ "list_dir", m_list_dir, S, 1, { S }, { "path" } },
	{ "de_apps", m_de_apps, S, 0, { 0 }, { 0 } },
	{ "de_requests", m_de_requests, S, 0, { 0 }, { 0 } },
	{ "install", m_install, V, 1, { I }, { "seed" } },
	{ "peer_addr", m_peer_addr, S, 0, { 0 }, { 0 } },
	{ "on_rescue", m_on_rescue, B, 0, { 0 }, { 0 } },
};
const int TRS_COMPUTER_METHOD_COUNT = (int)(sizeof TRS_COMPUTER_METHODS / sizeof TRS_COMPUTER_METHODS[0]);
