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

#include "trs/craft.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void trs_craft_init(trs_craft *c, const trs_part_catalog *cat) {
	memset(c, 0, sizeof(*c));
	c->cat = cat;
}

void trs_craft_free(trs_craft *c) {
	free(c->parts);
	c->parts = NULL;
	c->count = c->cap = 0;
}

void trs_craft_copy(trs_craft *dst, const trs_craft *src) {
	*dst = *src;
	dst->cap = src->count ? src->count : 1;
	dst->parts = malloc(sizeof(trs_part) * (size_t)dst->cap);
	memcpy(dst->parts, src->parts, sizeof(trs_part) * (size_t)src->count);
}

static double fuel_capacity(const trs_part_def *d) {
	return d->fuel_liquid > 0 ? d->fuel_liquid : d->fuel_solid;
}

int trs_craft_add(trs_craft *c, int def, int parent, const char *node, const char *parent_node, trs_v3 pos,
		trs_quat rot, int stage, int sym) {
	if (c->count == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 16;
		c->parts = realloc(c->parts, sizeof(trs_part) * (size_t)c->cap);
	}
	trs_part *p = &c->parts[c->count];
	memset(p, 0, sizeof(*p));
	p->def = def;
	p->uid = c->next_uid++;
	p->parent = parent;
	snprintf(p->node, sizeof p->node, "%s", node ? node : "");
	snprintf(p->parent_node, sizeof p->parent_node, "%s", parent_node ? parent_node : "");
	p->pos = pos;
	p->rot = rot;
	p->stage = stage;
	p->sym = sym;
	p->fuel = fuel_capacity(&c->cat->defs[def]);
	p->mono = c->cat->defs[def].fuel_mono;
	p->ec = c->cat->defs[def].ec_capacity;
	p->limit = 1;
	p->temp = TRS_T_AMBIENT;
	return c->count++;
}

/* ---- JSON ---------------------------------------------------------------- */

static int read_vec(const trs_json *a, double *out, int n) {
	if (!a || a->type != TRS_JSON_ARRAY || (int)a->count != n)
		return 0;
	for (int i = 0; i < n; i++) {
		if (a->items[i]->type != TRS_JSON_NUMBER)
			return 0;
		out[i] = a->items[i]->num;
	}
	return 1;
}

int trs_craft_from_json(trs_craft *c, const trs_part_catalog *cat, const trs_json *j, char *err, size_t errlen) {
	trs_craft_init(c, cat);
	if (strcmp(trs_json_str(j, "format", ""), "trs-craft") != 0) {
		snprintf(err, errlen, "not a trs-craft file");
		return 0;
	}
	snprintf(c->name, sizeof c->name, "%s", trs_json_str(j, "name", "Untitled"));
	snprintf(c->description, sizeof c->description, "%s", trs_json_str(j, "description", ""));
	const trs_json *parts = trs_json_get(j, "parts");
	if (!parts || parts->type != TRS_JSON_ARRAY || parts->count == 0) {
		snprintf(err, errlen, "craft has no parts");
		return 0;
	}
	for (size_t i = 0; i < parts->count; i++) {
		const trs_json *pj = trs_json_at(parts, i);
		const char *pid = trs_json_str(pj, "part", "");
		int def = trs_catalog_find(cat, pid);
		if (def < 0) {
			snprintf(err, errlen, "part %zu: unknown part '%s'", i, pid);
			trs_craft_free(c);
			return 0;
		}
		int parent = (int)trs_json_num(pj, "parent", -1);
		if ((i == 0) != (parent < 0) || parent >= (int)i) {
			snprintf(err, errlen, "part %zu: parent must be an earlier part (root first)", i);
			trs_craft_free(c);
			return 0;
		}
		double pos[3] = { 0, 0, 0 }, rot[4] = { 0, 0, 0, 1 };
		read_vec(trs_json_get(pj, "pos"), pos, 3);
		read_vec(trs_json_get(pj, "rot"), rot, 4);
		trs_quat q = { rot[0], rot[1], rot[2], rot[3] };
		int k = trs_craft_add(c, def, parent, trs_json_str(pj, "node", ""), trs_json_str(pj, "parent_node", ""),
				v3(pos[0], pos[1], pos[2]), q, (int)trs_json_num(pj, "stage", -1), (int)trs_json_num(pj, "sym", 0));
		const trs_json *f = trs_json_get(pj, "fuel");
		if (f && f->type == TRS_JSON_NUMBER)
			c->parts[k].fuel = f->num;
		c->parts[k].groups = (int)trs_json_num(pj, "groups", 0) & 0x1ff;
		const trs_json *l = trs_json_get(pj, "limit");
		if (l && l->type == TRS_JSON_NUMBER) {
			if (!(l->num > 0 && l->num <= 1)) {
				snprintf(err, errlen, "part %zu: thrust limit must be in (0, 1]", i);
				trs_craft_free(c);
				return 0;
			}
			c->parts[k].limit = l->num;
		}
	}
	return 1;
}

static trs_json *num_array(const double *v, int n) {
	trs_json *a = trs_json_new(TRS_JSON_ARRAY);
	for (int i = 0; i < n; i++)
		trs_json_push(a, trs_json_new_num(v[i]));
	return a;
}

trs_json *trs_craft_to_json(const trs_craft *c) {
	trs_json *j = trs_json_new(TRS_JSON_OBJECT);
	trs_json_set(j, "format", trs_json_new_str("trs-craft"));
	trs_json_set(j, "version", trs_json_new_num(1));
	trs_json_set(j, "name", trs_json_new_str(c->name));
	trs_json_set(j, "description", trs_json_new_str(c->description));
	trs_json *parts = trs_json_new(TRS_JSON_ARRAY);
	for (int i = 0; i < c->count; i++) {
		const trs_part *p = &c->parts[i];
		const trs_part_def *d = trs_craft_def(c, i);
		trs_json *pj = trs_json_new(TRS_JSON_OBJECT);
		trs_json_set(pj, "part", trs_json_new_str(d->id));
		trs_json_set(pj, "parent", trs_json_new_num(p->parent));
		trs_json_set(pj, "node", trs_json_new_str(p->node));
		trs_json_set(pj, "parent_node", trs_json_new_str(p->parent_node));
		double pos[3] = { p->pos.x, p->pos.y, p->pos.z };
		double rot[4] = { p->rot.x, p->rot.y, p->rot.z, p->rot.w };
		trs_json_set(pj, "pos", num_array(pos, 3));
		trs_json_set(pj, "rot", num_array(rot, 4));
		trs_json_set(pj, "stage", trs_json_new_num(p->stage));
		trs_json_set(pj, "sym", trs_json_new_num(p->sym));
		if (p->fuel != fuel_capacity(d))
			trs_json_set(pj, "fuel", trs_json_new_num(p->fuel));
		if (p->limit != 1)
			trs_json_set(pj, "limit", trs_json_new_num(p->limit));
		if (p->groups)
			trs_json_set(pj, "groups", trs_json_new_num(p->groups));
		trs_json_push(parts, pj);
	}
	trs_json_set(j, "parts", parts);
	return j;
}

double trs_part_mdot(const trs_craft *c, int i) {
	return trs_engine_mdot(trs_craft_def(c, i)) * c->parts[i].limit;
}

/* ---- mass properties ------------------------------------------------------ */

double trs_craft_part_mass(const trs_craft *c, int i) {
	return trs_craft_def(c, i)->mass + c->parts[i].fuel + c->parts[i].mono;
}

double trs_craft_mono(const trs_craft *c, double *capacity) {
	double m = 0, cap = 0;
	for (int i = 0; i < c->count; i++) {
		m += c->parts[i].mono;
		cap += trs_craft_def(c, i)->fuel_mono;
	}
	if (capacity)
		*capacity = cap;
	return m;
}

double trs_craft_ec(const trs_craft *c, double *capacity) {
	double e = 0, cap = 0;
	for (int i = 0; i < c->count; i++) {
		e += c->parts[i].ec;
		cap += trs_craft_def(c, i)->ec_capacity;
	}
	if (capacity)
		*capacity = cap;
	return e;
}

double trs_craft_ec_add(trs_craft *c, double kj) {
	double cap, e = trs_craft_ec(c, &cap);
	double want = fmin(cap, fmax(0, e + kj));
	for (int i = 0; i < c->count; i++) {
		double ci = trs_craft_def(c, i)->ec_capacity;
		c->parts[i].ec = cap > 0 ? want * ci / cap : 0; /* every battery at the same charge */
	}
	return want - e;
}

void trs_craft_mono_draw(trs_craft *c, double amount) {
	double m = trs_craft_mono(c, NULL);
	if (m <= 0)
		return;
	double keep = amount >= m * (1 - 1e-12) ? 0 : 1 - amount / m; /* no crumbs left by rounding */
	for (int i = 0; i < c->count; i++)
		c->parts[i].mono = keep == 0 ? 0 : c->parts[i].mono * keep;
}

double trs_craft_mass(const trs_craft *c) {
	double m = 0;
	for (int i = 0; i < c->count; i++)
		m += trs_craft_part_mass(c, i);
	return m;
}

void trs_craft_mass_props(const trs_craft *c, double *mass, trs_v3 *cm, trs_m3 *inertia) {
	double m = 0;
	trs_v3 s = v3(0, 0, 0);
	for (int i = 0; i < c->count; i++) {
		double mi = trs_craft_part_mass(c, i);
		m += mi;
		s = v3_madd(s, c->parts[i].pos, mi);
	}
	trs_v3 com = m > 0 ? v3_scale(s, 1.0 / m) : v3(0, 0, 0);
	trs_m3 I = m3_zero();
	for (int i = 0; i < c->count; i++) {
		const trs_part_def *d = trs_craft_def(c, i);
		double mi = trs_craft_part_mass(c, i), r = d->radius, h = d->height;
		/* solid cylinder about its own axis (local Y) */
		trs_m3 local = m3_zero();
		local.m[0][0] = local.m[2][2] = mi * (3 * r * r + h * h) / 12.0;
		local.m[1][1] = 0.5 * mi * r * r;
		trs_m3 rl = m3_rotate_tensor(c->parts[i].rot, &local);
		trs_v3 dv = v3_sub(c->parts[i].pos, com);
		double dd[3] = { dv.x, dv.y, dv.z }, d2 = v3_len2(dv);
		for (int a = 0; a < 3; a++)
			for (int b = 0; b < 3; b++)
				I.m[a][b] += rl.m[a][b] + mi * ((a == b ? d2 : 0) - dd[a] * dd[b]);
	}
	if (mass) *mass = m;
	if (cm) *cm = com;
	if (inertia) *inertia = I;
}

int trs_craft_max_stage(const trs_craft *c) {
	int s = -1;
	for (int i = 0; i < c->count; i++)
		if (c->parts[i].stage > s)
			s = c->parts[i].stage;
	return s;
}

/* ---- fuel ---------------------------------------------------------------- */

int trs_craft_fuel_groups(const trs_craft *c, int *group) {
	/* Union-find over tree edges, skipping edges that touch a decoupler. */
	for (int i = 0; i < c->count; i++)
		group[i] = i;
	for (int i = 1; i < c->count; i++) {
		int p = c->parts[i].parent;
		if (trs_craft_def(c, i)->decoupler || trs_craft_def(c, p)->decoupler)
			continue;
		int a = i, b = p;
		while (group[a] != a) a = group[a];
		while (group[b] != b) b = group[b];
		if (a != b)
			group[a < b ? b : a] = a < b ? a : b;
	}
	int n = 0;
	size_t cnt = c->count > 0 ? (size_t)c->count : 1;
	int *root = malloc(sizeof(int) * cnt);
	int *label = malloc(sizeof(int) * cnt);
	for (int i = 0; i < c->count; i++) {
		int r = i;
		while (group[r] != r) r = group[r];
		root[i] = r;
		label[i] = -1;
	}
	for (int i = 0; i < c->count; i++) {
		if (label[root[i]] < 0)
			label[root[i]] = n++;
		group[i] = label[root[i]];
	}
	free(root);
	free(label);
	return n;
}

double trs_craft_group_fuel(const trs_craft *c, const int *group, int g) {
	double f = 0;
	for (int i = 0; i < c->count; i++)
		if (group[i] == g && trs_craft_def(c, i)->fuel_liquid > 0)
			f += c->parts[i].fuel;
	return f;
}

void trs_craft_group_draw(trs_craft *c, const int *group, int g, double amount) {
	double f = trs_craft_group_fuel(c, group, g);
	if (f <= 0)
		return;
	double keep = amount >= f ? 0 : 1 - amount / f;
	for (int i = 0; i < c->count; i++)
		if (group[i] == g && trs_craft_def(c, i)->fuel_liquid > 0)
			c->parts[i].fuel = keep == 0 ? 0 : c->parts[i].fuel * keep;
}

#define FUEL_EPS 1e-9

static int point_in_part(const trs_craft *c, int j, trs_v3 p) {
	const trs_part_def *d = trs_craft_def(c, j);
	if (d->is_leg || d->is_parachute || d->plate || d->radius <= 0.05)
		return 0;
	trs_v3 lp = quat_rotate(quat_conj(c->parts[j].rot), v3_sub(p, c->parts[j].pos));
	double hh = d->height * 0.5;
	if (fabs(lp.y) >= hh)
		return 0;
	double f = d->height > 0 ? (lp.y + hh) / d->height : 0.5;
	double r = d->radius_bottom + (d->radius_top - d->radius_bottom) * f;
	return lp.x * lp.x + lp.z * lp.z < r * r;
}

int trs_craft_feed_target(const trs_craft *c, const int *group, int i, double *dist) {
	const trs_part *pl = &c->parts[i];
	double reach = trs_craft_def(c, i)->feed_reach;
	trs_v3 dir = quat_rotate(pl->rot, v3(0, 0, 1));
	for (double s = 0.02; s <= reach + 1e-9; s += 0.02) {
		trs_v3 p = v3_add(pl->pos, v3_scale(dir, s));
		for (int j = 0; j < c->count; j++)
			if (j != i && group[j] != group[i] && !trs_craft_def(c, j)->decoupler && point_in_part(c, j, p)) {
				if (dist)
					*dist = s;
				return j;
			}
	}
	if (dist)
		*dist = reach;
	return -1;
}

void trs_fuel_net_build(trs_fuel_net *n, const trs_craft *c) {
	memset(n, 0, sizeof(*n));
	int cnt = c->count > 0 ? c->count : 1;
	n->count = c->count;
	n->group = malloc(sizeof(int) * (size_t)cnt);
	n->ngroups = trs_craft_fuel_groups(c, n->group);
	int ng = n->ngroups > 0 ? n->ngroups : 1;
	n->up = calloc((size_t)ng * (size_t)ng, 1);
	n->line_part = malloc(sizeof(int) * (size_t)cnt);
	n->line_target = malloc(sizeof(int) * (size_t)cnt);
	for (int i = 0; i < c->count; i++) {
		if (trs_craft_def(c, i)->feed_reach <= 0)
			continue;
		int t = trs_craft_feed_target(c, n->group, i, NULL);
		n->line_part[n->nlines] = i;
		n->line_target[n->nlines++] = t;
		if (t >= 0)
			n->up[n->group[i] * ng + n->group[t]] = 1;
	}
	/* transitive closure (Warshall) */
	for (int k = 0; k < n->ngroups; k++)
		for (int a = 0; a < n->ngroups; a++)
			if (n->up[a * ng + k])
				for (int b = 0; b < n->ngroups; b++)
					if (n->up[k * ng + b])
						n->up[a * ng + b] = 1;
	for (int a = 0; a < n->ngroups; a++)
		n->up[a * ng + a] = 0;
}

void trs_fuel_net_free(trs_fuel_net *n) {
	free(n->group);
	free(n->up);
	free(n->line_part);
	free(n->line_target);
	memset(n, 0, sizeof(*n));
}

void trs_fuel_levels(const trs_fuel_net *n, const trs_craft *c, double *gf) {
	for (int g = 0; g < n->ngroups; g++)
		gf[g] = 0;
	for (int i = 0; i < c->count; i++)
		if (trs_craft_def(c, i)->fuel_liquid > 0)
			gf[n->group[i]] += c->parts[i].fuel;
}

int trs_fuel_reaches(const trs_fuel_net *n, int a, int g) {
	return a == g || n->up[a * n->ngroups + g];
}

int trs_fuel_sources(const trs_fuel_net *n, const double *gf, int g, int *out) {
	int m = 0, ng = n->ngroups;
	for (int h = 0; h < ng; h++) {
		if (gf[h] <= FUEL_EPS || !trs_fuel_reaches(n, h, g))
			continue;
		/* skip h while something still full sits upstream of it (groups
		 * piped both ways round a loop share alike) */
		int blocked = 0;
		for (int a = 0; a < ng && !blocked; a++)
			if (a != h && gf[a] > FUEL_EPS && n->up[a * ng + h] && !n->up[h * ng + a])
				blocked = 1;
		if (!blocked)
			out[m++] = h;
	}
	return m;
}

void trs_fuel_rates(const trs_fuel_net *n, const trs_craft *c, const double *mdot, double *rate) {
	int ng = n->ngroups > 0 ? n->ngroups : 1;
	double *gf = malloc(sizeof(double) * (size_t)ng);
	int *src = malloc(sizeof(int) * (size_t)ng);
	trs_fuel_levels(n, c, gf);
	for (int g = 0; g < n->ngroups; g++)
		rate[g] = 0;
	for (int i = 0; i < c->count; i++) {
		if (mdot[i] <= 0 || trs_craft_def(c, i)->propellant != TRS_PROP_LIQUID)
			continue;
		int m = trs_fuel_sources(n, gf, n->group[i], src);
		for (int k = 0; k < m; k++)
			rate[src[k]] += mdot[i] / m;
	}
	free(gf);
	free(src);
}

int trs_craft_engine_fed(const trs_craft *c, const trs_fuel_net *n, int i) {
	const trs_part_def *d = trs_craft_def(c, i);
	if (d->propellant == TRS_PROP_SOLID)
		return c->parts[i].fuel > FUEL_EPS;
	if (d->propellant != TRS_PROP_LIQUID)
		return 0;
	int g = n->group[i];
	for (int k = 0; k < c->count; k++)
		if (trs_craft_def(c, k)->fuel_liquid > 0 && c->parts[k].fuel > 0 && trs_fuel_reaches(n, n->group[k], g)) {
			/* the sum, not one tank, decides: tanks drain together */
			double f = 0;
			int ng = n->ngroups > 0 ? n->ngroups : 1;
			double *gf = malloc(sizeof(double) * (size_t)ng);
			trs_fuel_levels(n, c, gf);
			for (int h = 0; h < n->ngroups && f <= FUEL_EPS; h++)
				if (trs_fuel_reaches(n, h, g) && gf[h] > FUEL_EPS)
					f = gf[h];
			free(gf);
			return f > FUEL_EPS;
		}
	return 0;
}

/* ---- splitting ------------------------------------------------------------ */

static void mark_subtree(const trs_craft *c, int root, unsigned char *mask) {
	mask[root] = 1;
	for (int i = root + 1; i < c->count; i++)
		if (c->parts[i].parent >= 0 && mask[c->parts[i].parent])
			mask[i] = 1;
}

void trs_craft_detach_set(const trs_craft *c, int d, unsigned char *mask) {
	memset(mask, 0, (size_t)c->count);
	const trs_part_def *dd = trs_craft_def(c, d);
	if (!dd->decoupler && !dd->is_dock)
		return;
	if (dd->decoupler == TRS_DECOUPLER_RADIAL) {
		if (c->parts[d].parent >= 0)
			mark_subtree(c, d, mask); /* decoupler leaves with the booster */
		return;
	}
	/* Stack (and docking ports): separates across its "top" node,
	 * whichever side that is. */
	if (c->parts[d].parent >= 0 && !strcmp(c->parts[d].node, "top")) {
		mark_subtree(c, d, mask);
		return;
	}
	for (int i = d + 1; i < c->count; i++)
		if (c->parts[i].parent == d && !strcmp(c->parts[i].parent_node, "top")) {
			mark_subtree(c, i, mask);
			return;
		}
}

int trs_craft_split(trs_craft *c, int d, trs_craft *out) {
	unsigned char *mask = calloc((size_t)c->count, 1);
	trs_craft_detach_set(c, d, mask);
	trs_craft_init(out, c->cat);
	snprintf(out->name, sizeof out->name, "%.56s debris", c->name);
	int moved = 0;
	for (int i = 0; i < c->count; i++)
		moved += mask[i];
	if (!moved || mask[0]) {
		free(mask);
		return 0;
	}
	int *remap = malloc(sizeof(int) * (size_t)c->count);
	/* detached piece, in original (topological) order */
	out->cap = moved;
	out->parts = malloc(sizeof(trs_part) * (size_t)moved);
	out->next_uid = c->next_uid;
	for (int i = 0; i < c->count; i++) {
		if (!mask[i])
			continue;
		trs_part p = c->parts[i];
		remap[i] = out->count;
		p.parent = (p.parent >= 0 && mask[p.parent]) ? remap[p.parent] : -1;
		if (p.parent < 0) {
			p.node[0] = 0;
			p.parent_node[0] = 0;
		}
		out->parts[out->count++] = p;
	}
	/* remaining piece */
	int n = 0;
	for (int i = 0; i < c->count; i++) {
		if (mask[i])
			continue;
		trs_part p = c->parts[i];
		remap[i] = n;
		p.parent = p.parent >= 0 ? remap[p.parent] : -1;
		c->parts[n++] = p;
	}
	c->count = n;
	free(remap);
	free(mask);
	return 1;
}

/* Extracts the subtree rooted at r into out (re-indexed), leaving c intact. */
static void extract_subtree(const trs_craft *c, int r, trs_craft *out) {
	unsigned char *mask = calloc((size_t)c->count, 1);
	mark_subtree(c, r, mask);
	int *remap = malloc(sizeof(int) * (size_t)c->count);
	trs_craft_init(out, c->cat);
	snprintf(out->name, sizeof out->name, "%.56s debris", c->name);
	out->next_uid = c->next_uid;
	for (int i = 0; i < c->count; i++)
		if (mask[i])
			out->cap++;
	out->parts = malloc(sizeof(trs_part) * (size_t)(out->cap ? out->cap : 1));
	for (int i = 0; i < c->count; i++) {
		if (!mask[i])
			continue;
		trs_part p = c->parts[i];
		remap[i] = out->count;
		p.parent = (i == r) ? -1 : remap[p.parent];
		if (i == r)
			p.node[0] = p.parent_node[0] = 0;
		out->parts[out->count++] = p;
	}
	free(remap);
	free(mask);
}

int trs_craft_remove_part(trs_craft *c, int d, trs_craft *pieces, int max) {
	int n = 0;
	for (int i = d + 1; i < c->count; i++)
		if (c->parts[i].parent == d && n < max)
			extract_subtree(c, i, &pieces[n++]);
	/* drop d and its whole subtree from c */
	unsigned char *mask = calloc((size_t)c->count, 1);
	mark_subtree(c, d, mask);
	int *remap = malloc(sizeof(int) * (size_t)c->count);
	int k = 0;
	for (int i = 0; i < c->count; i++) {
		if (mask[i])
			continue;
		trs_part p = c->parts[i];
		remap[i] = k;
		p.parent = p.parent >= 0 ? remap[p.parent] : -1;
		c->parts[k++] = p;
	}
	c->count = k;
	free(remap);
	free(mask);
	return n;
}

/* ---- staging readouts ------------------------------------------------------ */

void trs_craft_activate_stage_root_only(trs_craft *c, int s) {
	for (int i = 0; i < c->count; i++) {
		if (c->parts[i].stage != s)
			continue;
		const trs_part_def *d = trs_craft_def(c, i);
		if (d->propellant != TRS_PROP_NONE)
			c->parts[i].engine_on = 1;
		if (d->is_leg)
			c->parts[i].legs_deployed = 1;
	}
	/* Fire decouplers; indices shift after each split, so rescan by uid. */
	for (;;) {
		int fired = 0;
		for (int i = 0; i < c->count; i++) {
			if (c->parts[i].stage == s && trs_craft_def(c, i)->decoupler && !c->parts[i].fired) {
				c->parts[i].fired = 1;
				trs_craft piece;
				if (trs_craft_split(c, i, &piece))
					trs_craft_free(&piece);
				fired = 1;
				break;
			}
		}
		if (!fired)
			break;
	}
}

int trs_craft_stage_stats(const trs_craft *src, int next_stage, double g_ref, double p_sl, trs_stage_stats *stats,
		int max) {
	trs_craft c;
	trs_craft_copy(&c, src);
	trs_fuel_net net;
	memset(&net, 0, sizeof net);
	unsigned char *drop = malloc((size_t)(c.count ? c.count : 1));
	int n = 0;
	for (int s = next_stage; s >= 0 && n < max; s--) {
		trs_craft_activate_stage_root_only(&c, s);
		trs_fuel_net_free(&net);
		trs_fuel_net_build(&net, &c);
		int ngs = net.ngroups > 0 ? net.ngroups : 1;
		double *md = calloc((size_t)(c.count ? c.count : 1), sizeof(double));
		double *rate = calloc((size_t)ngs, sizeof(double));
		double *gf = calloc((size_t)ngs, sizeof(double));
		/* parts the next stage will drop */
		memset(drop, 0, (size_t)c.count);
		unsigned char *tmp = malloc((size_t)c.count);
		for (int i = 0; i < c.count; i++)
			if (c.parts[i].stage == s - 1 && trs_craft_def(&c, i)->decoupler) {
				trs_craft_detach_set(&c, i, tmp);
				for (int k = 0; k < c.count; k++)
					drop[k] |= tmp[k];
			}
		free(tmp);

		trs_stage_stats *st = &stats[n++];
		memset(st, 0, sizeof(*st));
		st->stage = s;
		st->mass_start = trs_craft_mass(&c);
		int first = 1;
		for (int seg = 0; seg < 256; seg++) {
			int any = 0, any_drop_lit = 0, drop_has_engines = 0;
			double mdot = 0;
			trs_v3 fv_vac = v3(0, 0, 0), fv_sl = v3(0, 0, 0); /* net: tilted engines partly cancel */
			for (int i = 0; i < c.count; i++) {
				const trs_part_def *d = trs_craft_def(&c, i);
				/* rocket stages only: a jet's thrust depends on the air and
				 * speed, so it is reported apart (trs_craft_jet_stats) */
				if (!c.parts[i].engine_on || d->propellant == TRS_PROP_NONE || d->air_breathing)
					continue;
				if (drop[i])
					drop_has_engines = 1;
				if (!trs_craft_engine_fed(&c, &net, i))
					continue;
				any = 1;
				if (drop[i])
					any_drop_lit = 1;
				double md = trs_part_mdot(&c, i);
				mdot += md;
				trs_v3 ax = quat_rotate(c.parts[i].rot, v3(0, 1, 0));
				fv_vac = v3_add(fv_vac, v3_scale(ax, md * d->isp_vac * TRS_G0));
				fv_sl = v3_add(fv_sl, v3_scale(ax, md * trs_engine_isp(d, p_sl) * TRS_G0));
			}
			double f_vac = v3_len(fv_vac), f_sl = v3_len(fv_sl);
			if (!any || (drop_has_engines && !any_drop_lit))
				break;
			if (first) {
				st->thrust_vac = f_vac;
				st->thrust_sl = f_sl;
				st->twr_vac = f_vac / (st->mass_start * g_ref);
				st->twr_sl = f_sl / (st->mass_start * g_ref);
				st->isp_vac = f_vac / (mdot * TRS_G0);
				st->isp_sl = f_sl / (mdot * TRS_G0);
				first = 0;
			}
			/* time until the first drawn group (or solid) runs dry */
			double tau = INFINITY;
			for (int i = 0; i < c.count; i++) {
				const trs_part_def *d = trs_craft_def(&c, i);
				md[i] = c.parts[i].engine_on && d->propellant == TRS_PROP_LIQUID && !d->air_breathing && trs_craft_engine_fed(&c, &net, i)
						? trs_part_mdot(&c, i) : 0;
			}
			trs_fuel_rates(&net, &c, md, rate);
			trs_fuel_levels(&net, &c, gf);
			for (int g = 0; g < net.ngroups; g++)
				if (rate[g] > 0)
					tau = fmin(tau, gf[g] / rate[g]);
			for (int i = 0; i < c.count; i++) {
				const trs_part_def *d = trs_craft_def(&c, i);
				if (c.parts[i].engine_on && d->propellant == TRS_PROP_SOLID && c.parts[i].fuel > FUEL_EPS)
					tau = fmin(tau, c.parts[i].fuel / trs_part_mdot(&c, i));
			}
			double m0 = trs_craft_mass(&c), m1 = m0 - mdot * tau;
			st->dv_vac += f_vac / mdot * log(m0 / m1);
			st->dv_sl += f_sl / mdot * log(m0 / m1);
			st->burn_time += tau;
			/* burn the propellant */
			for (int g = 0; g < net.ngroups; g++)
				if (rate[g] > 0)
					trs_craft_group_draw(&c, net.group, g, rate[g] * tau * (1 + 1e-12));
			for (int i = 0; i < c.count; i++) {
				const trs_part_def *d = trs_craft_def(&c, i);
				if (c.parts[i].engine_on && d->propellant == TRS_PROP_SOLID && c.parts[i].fuel > FUEL_EPS) {
					c.parts[i].fuel -= trs_part_mdot(&c, i) * tau;
					if (c.parts[i].fuel < FUEL_EPS * 10)
						c.parts[i].fuel = 0;
				}
			}
		}
		st->mass_end = trs_craft_mass(&c);
		free(md);
		free(rate);
		free(gf);
	}
	free(drop);
	trs_fuel_net_free(&net);
	trs_craft_free(&c);
	return n;
}
