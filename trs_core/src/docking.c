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

/* Docking ports: latching two vessels into one, and letting go again. */

#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOCK_REARM 2.0 /* m: an undocked port must get this far from its old partner before it can latch again */

static int free_port(const trs_craft *c, int i) {
	if (!trs_craft_def(c, i)->is_dock)
		return 0;
	if (c->parts[i].parent >= 0 && !strcmp(c->parts[i].node, "top"))
		return 0;
	for (int k = i + 1; k < c->count; k++)
		if (c->parts[k].parent == i && !strcmp(c->parts[k].parent_node, "top"))
			return 0;
	return 1;
}

/* The docking face of port i: world position, facing direction and velocity. */
static void port_face(const trs_vessel *vs, int i, trs_v3 *pos, trs_v3 *axis, trs_v3 *vel) {
	const trs_part *p = &vs->craft->parts[i];
	const trs_attach_node *top = trs_part_node(trs_craft_def(vs->craft, i), "top");
	trs_v3 local = v3_add(p->pos, quat_rotate(p->rot, top ? top->pos : v3(0, 0, 0)));
	trs_v3 arm = quat_rotate(vs->q, v3_sub(local, vs->cm));
	*pos = v3_add(vs->r, arm);
	*axis = v3_norm(quat_rotate(vs->q, quat_rotate(p->rot, top ? top->dir : v3(0, 1, 0))));
	*vel = v3_add(vs->v, v3_cross(vs->w, arm));
}

static int live(const trs_vessel *vs) {
	return vs->alive && vs->craft && vs->mode == TRS_MODE_PHYSICS;
}

void trs_world_dock_check(trs_world *w) {
	for (int a = 0; a < w->nvessels; a++) {
		if (!live(&w->vessels[a]))
			continue;
		for (int b = a + 1; b < w->nvessels; b++) {
			trs_vessel *A = &w->vessels[a], *B = &w->vessels[b];
			if (!live(B) || A->body != B->body)
				continue;
			if (v3_len(v3_sub(A->r, B->r)) > A->bound_radius + B->bound_radius + 5)
				continue;
			for (int i = 0; i < A->craft->count; i++) {
				if (!free_port(A->craft, i))
					continue;
				for (int j = 0; j < B->craft->count; j++) {
					if (!free_port(B->craft, j))
						continue;
					trs_part *pi = &A->craft->parts[i], *pj = &B->craft->parts[j];
					const trs_part_def *di = trs_craft_def(A->craft, i), *dj = trs_craft_def(B->craft, j);
					trs_v3 fa, xa, va, fb, xb, vb;
					port_face(A, i, &fa, &xa, &va);
					port_face(B, j, &fb, &xb, &vb);
					double d = v3_len(v3_sub(fa, fb));
					/* a pair that just let go re-arms once well apart */
					int waiting = pi->dock_wait_uid == pj->uid + 1 || pj->dock_wait_uid == pi->uid + 1;
					if (waiting) {
						if (d > DOCK_REARM) {
							if (pi->dock_wait_uid == pj->uid + 1)
								pi->dock_wait_uid = 0;
							if (pj->dock_wait_uid == pi->uid + 1)
								pj->dock_wait_uid = 0;
						}
						continue;
					}
					if (d > fmin(di->dock_capture, dj->dock_capture))
						continue;
					if (v3_dot(xa, xb) > -cos(fmin(di->dock_max_angle, dj->dock_max_angle)))
						continue;
					if (v3_len(v3_sub(va, vb)) > fmin(di->dock_max_speed, dj->dock_max_speed))
						continue;
					trs_world_dock(w, a, i, b, j);
					return; /* indices changed: the rest waits for the next step */
				}
			}
		}
	}
}

void trs_world_dock(trs_world *w, int a, int pa, int b, int pb) {
	trs_vessel *A = &w->vessels[a], *B = &w->vessels[b];
	trs_craft *ca = A->craft, *cb = B->craft;

	/* momentum of the pair, about their common CoM */
	double M = A->mass + B->mass;
	trs_v3 R = v3_scale(v3_add(v3_scale(A->r, A->mass), v3_scale(B->r, B->mass)), 1.0 / M);
	trs_v3 V = v3_scale(v3_add(v3_scale(A->v, A->mass), v3_scale(B->v, B->mass)), 1.0 / M);
	trs_m3 IA = m3_rotate_tensor(A->q, &A->inertia), IB = m3_rotate_tensor(B->q, &B->inertia);
	trs_v3 L = v3_add(m3_mulv(&IA, A->w), m3_mulv(&IB, B->w));
	L = v3_add(L, v3_scale(v3_cross(v3_sub(A->r, R), v3_sub(A->v, V)), A->mass));
	L = v3_add(L, v3_scale(v3_cross(v3_sub(B->r, R), v3_sub(B->v, V)), B->mass));

	/* B snaps face to face with A's port (no roll correction) */
	trs_v3 fa, xa, va, fb, xb, vb;
	port_face(A, pa, &fa, &xa, &va);
	port_face(B, pb, &fb, &xb, &vb);
	trs_quat S = quat_from_to(xb, v3_neg(xa));
	trs_quat qai = quat_conj(A->q);

	/* B's tree, re-rooted at its port: breadth first from pb, so every
	 * part comes after the one it now hangs from */
	int n = cb->count;
	int *order = malloc(sizeof(int) * (size_t)n), *from = malloc(sizeof(int) * (size_t)n);
	unsigned char *seen = calloc((size_t)n, 1);
	char (*node)[16] = malloc(sizeof(char[16]) * (size_t)n), (*pnode)[16] = malloc(sizeof(char[16]) * (size_t)n);
	int head = 0, tail = 0;
	order[tail++] = pb;
	seen[pb] = 1;
	from[pb] = -1;
	snprintf(node[pb], 16, "top");
	snprintf(pnode[pb], 16, "top");
	while (head < tail) {
		int cur = order[head++];
		for (int k = 0; k < n; k++) {
			if (seen[k])
				continue;
			if (cb->parts[k].parent == cur) { /* a child keeps its joint */
				snprintf(node[k], 16, "%s", cb->parts[k].node);
				snprintf(pnode[k], 16, "%s", cb->parts[k].parent_node);
			} else if (cb->parts[cur].parent == k) { /* the old parent hangs from it now */
				snprintf(node[k], 16, "%s", cb->parts[cur].parent_node);
				snprintf(pnode[k], 16, "%s", cb->parts[cur].node);
			} else {
				continue;
			}
			seen[k] = 1;
			from[k] = cur;
			order[tail++] = k;
		}
	}
	int base = ca->count, maxsym = 0;
	for (int i = 0; i < ca->count; i++)
		maxsym = ca->parts[i].sym > maxsym ? ca->parts[i].sym : maxsym;
	int *newidx = malloc(sizeof(int) * (size_t)n);
	for (int k = 0; k < tail; k++)
		newidx[order[k]] = base + k;
	trs_v3 cm_old = A->cm;
	for (int k = 0; k < tail; k++) {
		int o = order[k];
		trs_part p = cb->parts[o];
		trs_v3 wp = v3_add(B->r, quat_rotate(B->q, v3_sub(p.pos, B->cm)));
		trs_v3 sp = v3_add(fa, quat_rotate(S, v3_sub(wp, fb)));
		p.pos = v3_add(quat_rotate(qai, v3_sub(sp, A->r)), cm_old);
		p.rot = quat_normalize(quat_mul(qai, quat_mul(S, quat_mul(B->q, p.rot))));
		p.parent = from[o] < 0 ? pa : newidx[from[o]];
		snprintf(p.node, sizeof p.node, "%s", node[o]);
		snprintf(p.parent_node, sizeof p.parent_node, "%s", pnode[o]);
		if (p.sym)
			p.sym += maxsym;
		int idx = trs_craft_add(ca, p.def, p.parent, p.node, p.parent_node, p.pos, p.rot, p.stage, p.sym);
		ca->parts[idx] = p;
	}
	snprintf(ca->parts[newidx[pb]].dock_name, sizeof ca->parts[0].dock_name, "%.47s", cb->name);
	if (cb->next_uid > ca->next_uid)
		ca->next_uid = cb->next_uid;
	if (B->next_stage > A->next_stage)
		A->next_stage = B->next_stage;
	free(order);
	free(from);
	free(seen);
	free(node);
	free(pnode);
	free(newidx);

	trs_vessel_refresh(w, a);
	A = &w->vessels[a];
	A->r = v3_add(A->r, quat_rotate(A->q, v3_sub(A->cm, cm_old)));
	A->v = V;
	trs_m3 I = m3_rotate_tensor(A->q, &A->inertia);
	trs_m3 Ii = m3_inverse(&I);
	A->w = m3_mulv(&Ii, L);
	A->fresh = 1;
	trs_world_remove_vessel(w, b);
	trs_world_push_event(w, (trs_world_event){ TRS_WEV_DOCKED, w->t, a, b, ca->parts[pa].uid, "", 0 });
}

int trs_world_undock(trs_world *w, int id, int port) {
	trs_vessel *vs = &w->vessels[id];
	trs_craft *c = vs->craft;
	if (!c || port < 0 || port >= c->count || !trs_craft_def(c, port)->is_dock || free_port(c, port))
		return -1;
	if (vs->mode != TRS_MODE_PHYSICS)
		trs_vessel_set_mode(w, id, TRS_MODE_PHYSICS);
	vs = &w->vessels[id];
	/* the two ports of the joint */
	int other = -1;
	if (c->parts[port].parent >= 0 && !strcmp(c->parts[port].node, "top"))
		other = c->parts[port].parent;
	else
		for (int k = port + 1; k < c->count; k++)
			if (c->parts[k].parent == port && !strcmp(c->parts[k].parent_node, "top"))
				other = k;
	int uid_a = c->parts[port].uid, uid_b = c->parts[other].uid;
	const trs_part *dp = &c->parts[port];
	const trs_part_def *dd = trs_craft_def(c, port);
	unsigned char *mask = calloc((size_t)c->count, 1);
	trs_craft_detach_set(c, port, mask);
	int leaving = mask[port];
	int leaving_port = leaving ? port : other; /* the port that goes with the piece */
	char name[64];
	snprintf(name, sizeof name, "%s", c->parts[leaving_port].dock_name);
	free(mask);
	const trs_attach_node *top = trs_part_node(dd, "top");
	trs_v3 up = quat_rotate(dp->rot, v3(0, 1, 0));
	trs_v3 p_local = v3_add(dp->pos, quat_rotate(dp->rot, top ? top->pos : v3(0, 0, 0)));
	trs_v3 n_local = leaving ? v3_neg(up) : up;
	double J = dd->dock_impulse;

	trs_craft piece;
	if (!trs_craft_split(c, port, &piece))
		return -1;
	if (name[0])
		snprintf(piece.name, sizeof piece.name, "%s", name);
	trs_v3 cm_old = vs->cm, r_old = vs->r, v_old = vs->v, w_old = vs->w;
	trs_quat q = vs->q;
	trs_craft *pc = malloc(sizeof(trs_craft));
	*pc = piece;
	int nid = trs_world_add_vessel(w, vs->body, r_old, v_old);
	if (nid < 0) {
		trs_craft_free(pc);
		free(pc);
		return -1;
	}
	w->vessels[nid].lifted = w->vessels[id].lifted, w->vessels[nid].liftoff_t = w->vessels[id].liftoff_t;
	vs = &w->vessels[id];
	vs->fresh = 1;
	trs_vessel *nv = &w->vessels[nid];
	nv->craft = pc;
	nv->q = q;
	nv->w = w_old;
	nv->sas = TRS_SAS_OFF;
	nv->sas_hold = q;
	nv->next_stage = trs_craft_max_stage(pc);
	trs_vessel_refresh(w, nid);
	trs_vessel_refresh(w, id);
	trs_v3 dA = quat_rotate(q, v3_sub(vs->cm, cm_old)), dB = quat_rotate(q, v3_sub(nv->cm, cm_old));
	vs->r = v3_add(r_old, dA);
	vs->v = v3_add(v_old, v3_cross(w_old, dA));
	nv->r = v3_add(r_old, dB);
	nv->v = v3_add(v_old, v3_cross(w_old, dB));
	/* a gentle push apart at the faces */
	trs_v3 n = quat_rotate(q, v3_norm(n_local));
	trs_v3 P = v3_add(r_old, quat_rotate(q, v3_sub(p_local, cm_old)));
	trs_v3 Jv = v3_scale(n, J);
	nv->v = v3_madd(nv->v, Jv, 1.0 / nv->mass);
	vs->v = v3_madd(vs->v, Jv, -1.0 / vs->mass);
	trs_m3 IA = m3_rotate_tensor(q, &vs->inertia), IB = m3_rotate_tensor(q, &nv->inertia);
	trs_m3 IAi = m3_inverse(&IA), IBi = m3_inverse(&IB);
	nv->w = v3_add(nv->w, m3_mulv(&IBi, v3_cross(v3_sub(P, nv->r), Jv)));
	vs->w = v3_add(vs->w, m3_mulv(&IAi, v3_cross(v3_sub(P, vs->r), v3_neg(Jv))));
	/* the two ports stay apart until they have drifted clear */
	for (int k = 0; k < vs->craft->count; k++) {
		if (vs->craft->parts[k].uid == uid_a)
			vs->craft->parts[k].dock_wait_uid = uid_b + 1;
		if (vs->craft->parts[k].uid == uid_b)
			vs->craft->parts[k].dock_wait_uid = uid_a + 1;
	}
	for (int k = 0; k < nv->craft->count; k++) {
		if (nv->craft->parts[k].uid == uid_a)
			nv->craft->parts[k].dock_wait_uid = uid_b + 1;
		if (nv->craft->parts[k].uid == uid_b)
			nv->craft->parts[k].dock_wait_uid = uid_a + 1;
		if (nv->craft->parts[k].uid == uid_a || nv->craft->parts[k].uid == uid_b)
			nv->craft->parts[k].dock_name[0] = 0;
	}
	trs_world_push_event(w, (trs_world_event){ TRS_WEV_UNDOCKED, w->t, id, nid, uid_a, "", 0 });
	return nid;
}
