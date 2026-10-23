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

/* Physics-mode dynamics for rigid craft: thrust, propellant flow, drag,
 * attitude control, parachutes, and staging/decoupling. */
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUEL_EPS 1e-9

/* ---- caches ---------------------------------------------------------------- */

static trs_v3 part_axis(const trs_part *p) {
	return quat_rotate(p->rot, v3(0, 1, 0));
}

/* Part attached at part i's node `name`, either as parent or child. */
static int neighbour_at(const trs_craft *c, int i, const char *name) {
	if (c->parts[i].parent >= 0 && !strcmp(c->parts[i].node, name))
		return c->parts[i].parent;
	for (int k = i + 1; k < c->count; k++)
		if (c->parts[k].parent == i && !strcmp(c->parts[k].parent_node, name))
			return k;
	return -1;
}

/* Hull contact points: rims of each part's ends (fins: corners; deployed
 * legs: the foot, with the leg's own spring). Vessel-local frame. */
static void build_contacts(trs_vessel *vs) {
	const trs_craft *c = vs->craft;
	free(vs->contact_pt);
	free(vs->contact_part);
	free(vs->contact_leg);
	int cap = c->count * 16 + 1;
	vs->contact_pt = malloc(sizeof(trs_v3) * (size_t)cap);
	vs->contact_part = malloc(sizeof(int) * (size_t)cap);
	vs->contact_leg = malloc((size_t)cap);
	int n = 0;
	for (int i = 0; i < c->count; i++) {
		const trs_part *p = &c->parts[i];
		const trs_part_def *d = trs_craft_def(c, i);
		trs_v3 local[16];
		int m = 0, leg = 0;
		if (d->is_leg) {
			local[m++] = trs_leg_foot(d, p->legs_deployed ? 1.0 : 0.0);
			leg = p->legs_deployed;
		} else if (!strcmp(d->category, "aero") && d->is_parachute == 0 && d->radius < 0.31 && d->height <= 1.0) {
			/* fins: root ends and the tip */
			local[m++] = v3(0, d->height * 0.5, 0);
			local[m++] = v3(0, -d->height * 0.5, 0);
			local[m++] = v3(0, -d->height * 0.5, 0.8);
		} else {
			double r = d->radius, h = d->height * 0.5;
			for (int k = 0; k < 8; k++) {
				double a = TRS_TAU * k / 8;
				local[m++] = v3(r * sin(a), -h, r * cos(a));
				local[m++] = v3(r * sin(a), h, r * cos(a));
			}
		}
		for (int k = 0; k < m; k++) {
			vs->contact_pt[n] = v3_add(p->pos, quat_rotate(p->rot, local[k]));
			vs->contact_part[n] = i;
			vs->contact_leg[n] = (unsigned char)leg;
			n++;
		}
	}
	vs->ncontact = n;
	vs->bound_radius = 0;
	for (int k = 0; k < n; k++)
		vs->bound_radius = fmax(vs->bound_radius, v3_len(v3_sub(vs->contact_pt[k], vs->cm)));
}

void trs_vessel_refresh(trs_world *w, int id) {
	(void)w;
	trs_vessel *vs = &w->vessels[id];
	trs_craft *c = vs->craft;
	if (!c)
		return;
	free(vs->fuel_group);
	free(vs->face_top);
	free(vs->face_bottom);
	free(vs->wake_top);
	free(vs->wake_bottom);
	int n = c->count ? c->count : 1;
	vs->fuel_group = malloc(sizeof(int) * (size_t)n);
	vs->face_top = malloc(sizeof(double) * (size_t)n);
	vs->face_bottom = malloc(sizeof(double) * (size_t)n);
	vs->wake_top = malloc(sizeof(double) * (size_t)n);
	vs->wake_bottom = malloc(sizeof(double) * (size_t)n);
	trs_craft_fuel_groups(c, vs->fuel_group);
	for (int i = 0; i < c->count; i++) {
		const trs_part_def *d = trs_craft_def(c, i);
		double top = d->area_axial, bottom = d->area_axial;
		int nt = neighbour_at(c, i, "top"), nb = neighbour_at(c, i, "bottom");
		if (nt >= 0)
			top = fmax(0, top - trs_craft_def(c, nt)->area_axial);
		if (nb >= 0)
			bottom = fmax(0, bottom - trs_craft_def(c, nb)->area_axial);
		/* parts with no node on a face (nosecone tip, chute) keep it exposed */
		vs->face_top[i] = top;
		vs->face_bottom[i] = bottom;
		/* a part right behind a wider one sits in its wake: side heating
		 * scales with the share of the frontal area it would present */
		vs->wake_top[i] = nt >= 0 ? fmin(1, d->area_axial / fmax(trs_craft_def(c, nt)->area_axial, 1e-6)) : 1;
		vs->wake_bottom[i] = nb >= 0 ? fmin(1, d->area_axial / fmax(trs_craft_def(c, nb)->area_axial, 1e-6)) : 1;
	}
	trs_craft_mass_props(c, &vs->mass, &vs->cm, &vs->inertia);
	build_contacts(vs);
	vs->dirty = 0;
}

/* Re-centres after propellant moves the CoM, keeping the structure fixed. */
static void recentre(trs_vessel *vs) {
	trs_v3 old = vs->cm;
	trs_craft_mass_props(vs->craft, &vs->mass, &vs->cm, &vs->inertia);
	trs_v3 d = quat_rotate(vs->q, v3_sub(vs->cm, old));
	vs->r = v3_add(vs->r, d);
	vs->v = v3_add(vs->v, v3_cross(vs->w, d));
}

/* ---- forces ------------------------------------------------------------------ */

typedef struct {
	int n;
	int idx[64];
	double mdot[64];  /* kg/s */
	trs_v3 dir[64];   /* thrust direction, vessel-local */
	double total_mdot;
	trs_v3 force_local; /* at the current pressure */
	trs_v3 torque_local;
	double gimbal_authority; /* N m available for pitch/yaw */
} engine_set;

static void gather_engines(const trs_vessel *vs, double pressure, engine_set *es) {
	const trs_craft *c = vs->craft;
	memset(es, 0, sizeof(*es));
	for (int i = 0; i < c->count && es->n < 64; i++) {
		const trs_part *p = &c->parts[i];
		const trs_part_def *d = trs_craft_def(c, i);
		if (!p->engine_on || d->propellant == TRS_PROP_NONE || !trs_craft_engine_fed(c, vs->fuel_group, i))
			continue;
		double thr = d->propellant == TRS_PROP_SOLID ? 1.0 : vs->throttle;
		if (thr <= 0)
			continue;
		int k = es->n++;
		es->idx[k] = i;
		es->mdot[k] = trs_engine_mdot(d) * thr;
		es->dir[k] = part_axis(p);
		double f = es->mdot[k] * trs_engine_isp(d, pressure) * TRS_G0;
		trs_v3 fv = v3_scale(es->dir[k], f);
		es->force_local = v3_add(es->force_local, fv);
		trs_v3 lever = v3_sub(p->pos, vs->cm);
		es->torque_local = v3_add(es->torque_local, v3_cross(lever, fv));
		es->gimbal_authority += f * sin(d->gimbal) * v3_len(lever);
		es->total_mdot += es->mdot[k];
	}
}

/* Seconds until the first fed propellant pool among the lit engines runs dry. */
static double time_to_flameout(const trs_vessel *vs, const engine_set *es) {
	const trs_craft *c = vs->craft;
	double tau = INFINITY;
	for (int k = 0; k < es->n; k++) {
		int i = es->idx[k];
		const trs_part_def *d = trs_craft_def(c, i);
		if (d->propellant == TRS_PROP_SOLID) {
			tau = fmin(tau, c->parts[i].fuel / es->mdot[k]);
			continue;
		}
		int g = vs->fuel_group[i];
		double gm = 0;
		for (int j = 0; j < es->n; j++)
			if (trs_craft_def(c, es->idx[j])->propellant == TRS_PROP_LIQUID && vs->fuel_group[es->idx[j]] == g)
				gm += es->mdot[j];
		tau = fmin(tau, trs_craft_group_fuel(c, vs->fuel_group, g) / gm);
	}
	return tau;
}

static void burn(trs_vessel *vs, const engine_set *es, double h, int to_empty) {
	trs_craft *c = vs->craft;
	int ng = 0;
	for (int i = 0; i < c->count; i++)
		if (vs->fuel_group[i] + 1 > ng)
			ng = vs->fuel_group[i] + 1;
	double *draw = calloc((size_t)(ng ? ng : 1), sizeof(double));
	for (int k = 0; k < es->n; k++) {
		int i = es->idx[k];
		if (trs_craft_def(c, i)->propellant == TRS_PROP_SOLID) {
			c->parts[i].fuel -= es->mdot[k] * h;
			if (c->parts[i].fuel < FUEL_EPS * 10 || (to_empty && c->parts[i].fuel < 1e-6))
				c->parts[i].fuel = 0;
		} else {
			draw[vs->fuel_group[i]] += es->mdot[k] * h;
		}
	}
	for (int g = 0; g < ng; g++)
		if (draw[g] > 0) {
			double f = trs_craft_group_fuel(c, vs->fuel_group, g);
			trs_craft_group_draw(c, vs->fuel_group, g, (to_empty && draw[g] > f * (1 - 1e-9)) ? f : draw[g]);
		}
	free(draw);
}

/* Total aerodynamic force (SOI frame) and torque about the CoM (vessel-local).
 *
 * Each part sees the air at its own velocity (the CoM's plus the spin's
 * w x arm), so a tumbling craft is damped the way a real one is. A body's
 * force splits into an axial part on the face meeting the air and a
 * cross-flow part (Newtonian, ~sin^2 of the angle of attack) acting at the
 * part's centre of pressure. Fins are flat plates: they push back only along
 * their normal, which is what makes them lift and keeps rockets pointed.
 * Open parachutes are pure drag. For flow straight along a part's axis this
 * reduces exactly to Cd * A * q on the leading face. */
static void aero(const trs_world *w, const trs_vessel *vs, trs_v3 r, trs_v3 v, trs_v3 *force, trs_v3 *torque_local) {
	*force = v3(0, 0, 0);
	if (torque_local)
		*torque_local = v3(0, 0, 0);
	const trs_body *b = &w->sys.bodies[vs->body];
	if (b->atm.height <= 0)
		return;
	double alt = v3_len(r) - b->radius;
	double p, rho;
	trs_atmosphere_at(b, alt, &p, &rho);
	if (rho <= 0)
		return;
	trs_v3 omega_b = trs_body_omega(b);
	trs_v3 vair = v3_sub(v, v3_cross(omega_b, r));
	if (v3_len(vair) < 1e-9 && v3_len(v3_sub(vs->w, omega_b)) < 1e-9)
		return;
	trs_quat qi = quat_conj(vs->q);
	trs_v3 vl = quat_rotate(qi, vair);
	/* spin relative to the (co-rotating) air, vessel-local */
	trs_v3 wl = quat_rotate(qi, v3_sub(vs->w, omega_b));
	const trs_craft *c = vs->craft;
	trs_v3 f_local_total = v3(0, 0, 0);
	for (int i = 0; i < c->count; i++) {
		const trs_part *pt = &c->parts[i];
		const trs_part_def *d = trs_craft_def(c, i);
		trs_v3 arm = v3_sub(pt->pos, vs->cm);
		trs_v3 vp = v3_add(vl, v3_cross(wl, arm));
		double sp = v3_len(vp);
		if (sp < 1e-9)
			continue;
		trs_v3 dl = v3_scale(vp, 1.0 / sp);
		double qd = 0.5 * rho * sp * sp;
		trs_v3 ax = part_axis(pt);
		double cs = v3_dot(dl, ax);
		double face = cs > 0 ? d->cd_top * vs->face_top[i] : d->cd_bottom * vs->face_bottom[i];
		/* axial: on the leading face, along the axis */
		trs_v3 f_axial = v3_scale(ax, -qd * face * cs * fabs(cs));
		trs_v3 f_cross;
		if (d->plate) {
			trs_v3 n = quat_rotate(pt->rot, v3(1, 0, 0));
			double cn = v3_dot(dl, n);
			f_cross = v3_scale(n, -qd * d->cd_side * d->area_side * cn * fabs(cn));
		} else {
			trs_v3 perp = v3_sub(dl, v3_scale(ax, cs));
			f_cross = v3_scale(perp, -qd * d->cd_side * d->area_side * v3_len(perp));
		}
		trs_v3 f_chute = v3(0, 0, 0);
		if (d->is_parachute) {
			double cda = pt->chute == TRS_CHUTE_SEMI ? d->chute_semi_cda : pt->chute == TRS_CHUTE_FULL ? d->chute_full_cda : 0;
			f_chute = v3_scale(dl, -qd * cda);
		}
		trs_v3 f = v3_add(v3_add(f_axial, f_cross), f_chute);
		f_local_total = v3_add(f_local_total, f);
		if (torque_local) {
			trs_v3 cop = v3_add(arm, v3_scale(ax, d->cop * d->height));
			*torque_local = v3_add(*torque_local, v3_add(v3_cross(arm, v3_add(f_axial, f_chute)), v3_cross(cop, f_cross)));
			/* the part's own extent resists turning end over end: linearised
			 * cross-flow on a spinning slab of side area A and length L, with
			 * an empirical factor standing in for the pitch-damping derivative */
			double wa = v3_dot(wl, ax);
			trs_v3 w_perp = v3_sub(wl, v3_scale(ax, wa));
			double cq = 0.5 * rho * sp * d->cd_side * d->area_side;
			double L = fmax(d->height, 2 * d->radius);
			*torque_local = v3_sub(*torque_local, v3_scale(w_perp, cq * L * L));
			*torque_local = v3_sub(*torque_local, v3_scale(ax, cq * d->radius * d->radius * 0.1 * wa));
		}
	}
	*force = quat_rotate(vs->q, f_local_total);
}

/* ---- ground contact ----------------------------------------------------------- */

#define CONTACT_OMEGA 22.0  /* rad/s: hull stiffness, well inside RK4 stability at 50 Hz */
#define CONTACT_ZETA 0.8
#define FRICTION_MU 0.5
#define SLIP_REG 0.08 /* m/s: friction regularisation */

#define MAX_CRASH 64

typedef struct {
	int touching;
	int ncrash;          /* parts hitting faster than their tolerance */
	int crash_part[MAX_CRASH];
	double crash_speed[MAX_CRASH];
	double max_speed;
} contact_report;

/* Ground force (SOI frame) and torque about the CoM (vessel-local) for a
 * vessel at (r, v) with the attitude/spin from the start of the tick. */
static void ground(const trs_world *w, const trs_vessel *vs, double t, trs_v3 r, trs_v3 v, trs_v3 *force,
		trs_v3 *torque_local, contact_report *rep) {
	*force = v3(0, 0, 0);
	if (torque_local)
		*torque_local = v3(0, 0, 0);
	if (rep) {
		rep->touching = 0;
		rep->ncrash = 0;
		rep->max_speed = 0;
	}
	const trs_body *b = &w->sys.bodies[vs->body];
	double rl = v3_len(r);
	double h_here = trs_body_terrain_at(b, t, r);
	if (rl - b->radius - h_here > vs->bound_radius + 5.0)
		return;
	trs_v3 omega_b = trs_body_omega(b);
	trs_v3 nrm = trs_body_from_fixed(b, t,
			trs_terrain_normal(&b->terrain, b->radius, v3_norm(trs_body_to_fixed(b, t, r)), 2.0));
	if (!b->terrain.enabled)
		nrm = v3_scale(r, 1.0 / rl);
	double m = vs->mass;
	double k_hull = m * CONTACT_OMEGA * CONTACT_OMEGA / 4.0;
	double c_hull = 2 * CONTACT_ZETA * sqrt(k_hull * m / 4.0);
	const trs_craft *c = vs->craft;
	for (int k = 0; k < vs->ncontact; k++) {
		trs_v3 arm = quat_rotate(vs->q, v3_sub(vs->contact_pt[k], vs->cm));
		trs_v3 p = v3_add(r, arm);
		double pl = v3_len(p);
		double h = trs_body_terrain_at(b, t, p);
		double depth = (b->radius + h - pl) * v3_dot(nrm, v3_scale(p, 1.0 / pl));
		if (depth <= 0)
			continue;
		trs_v3 vp = v3_sub(v3_add(v, v3_cross(vs->w, arm)), v3_cross(omega_b, p));
		double vn = v3_dot(vp, nrm);
		double kk = k_hull, cc = c_hull;
		const trs_part_def *d = trs_craft_def(c, vs->contact_part[k]);
		if (vs->contact_leg[k]) {
			kk = d->leg_spring;
			cc = d->leg_damper;
		}
		double fn = kk * depth - cc * vn;
		if (fn < 0)
			fn = 0;
		trs_v3 vt = v3_sub(vp, v3_scale(nrm, vn));
		double vtl = v3_len(vt);
		trs_v3 ft = v3_scale(vt, -FRICTION_MU * fn / fmax(vtl, SLIP_REG));
		trs_v3 f = v3_add(v3_scale(nrm, fn), ft);
		*force = v3_add(*force, f);
		if (torque_local)
			*torque_local = v3_add(*torque_local, quat_rotate(quat_conj(vs->q), v3_cross(arm, f)));
		if (rep) {
			rep->touching++;
			double impact = -vn;
			double tol = vs->contact_leg[k] ? d->leg_max_speed : d->crash_tolerance;
			rep->max_speed = fmax(rep->max_speed, impact);
			if (impact > tol) {
				int part = vs->contact_part[k], seen = 0;
				for (int q = 0; q < rep->ncrash; q++)
					if (rep->crash_part[q] == part) {
						seen = 1;
						rep->crash_speed[q] = fmax(rep->crash_speed[q], impact);
					}
				if (!seen && rep->ncrash < MAX_CRASH) {
					rep->crash_part[rep->ncrash] = part;
					rep->crash_speed[rep->ncrash++] = impact;
				}
			}
		}
	}
}

/* ---- attitude ------------------------------------------------------------------ */

static double clampd(double x, double lo, double hi) {
	return x < lo ? lo : x > hi ? hi : x;
}

trs_v3 trs_sas_direction(const trs_world *w, int id, trs_sas_mode m) {
	const trs_vessel *vs = &w->vessels[id];
	trs_v3 r, v;
	trs_vessel_state(w, id, &r, &v);
	const trs_body *b = &w->sys.bodies[vs->body];
	trs_v3 vs_rel = v3_sub(v, v3_cross(trs_body_omega(b), r));
	trs_v3 h = v3_cross(r, v);
	switch (m) {
	case TRS_SAS_PROGRADE: return v3_norm(v);
	case TRS_SAS_RETROGRADE: return v3_neg(v3_norm(v));
	case TRS_SAS_NORMAL: return v3_norm(h);
	case TRS_SAS_ANTINORMAL: return v3_neg(v3_norm(h));
	case TRS_SAS_RADIAL_OUT: return v3_norm(v3_cross(v3_norm(h), v3_norm(v)));
	case TRS_SAS_RADIAL_IN: return v3_neg(v3_norm(v3_cross(v3_norm(h), v3_norm(v))));
	case TRS_SAS_SURF_PROGRADE: return v3_norm(vs_rel);
	case TRS_SAS_SURF_RETROGRADE: return v3_neg(v3_norm(vs_rel));
	case TRS_SAS_TARGET: return vs->sas_target;
	default: return quat_rotate(vs->q, v3(0, 1, 0));
	}
}

static trs_v3 control_torque(const trs_world *world, trs_vessel *vs, double gimbal_authority) {
	if (vs->sas >= TRS_SAS_PROGRADE) {
		trs_v3 d = trs_sas_direction(world, (int)(vs - world->vessels), vs->sas);
		if (v3_len2(d) > 0.5)
			vs->sas_target = d;
	}
	const trs_craft *c = vs->craft;
	double wheel = 0;
	for (int i = 0; i < c->count; i++)
		wheel += trs_craft_def(c, i)->torque;
	/* available torque per local axis: X pitch, Y roll, Z yaw */
	trs_v3 avail = v3(wheel + gimbal_authority, wheel, wheel + gimbal_authority);
	trs_v3 wb = quat_rotate(quat_conj(vs->q), vs->w);
	trs_v3 Iw = m3_mulv(&vs->inertia, wb);
	double Ix = vs->inertia.m[0][0], Iy = vs->inertia.m[1][1], Iz = vs->inertia.m[2][2];
	trs_v3 cmd = v3(0, 0, 0);
	int manual = fabs(vs->input.x) + fabs(vs->input.y) + fabs(vs->input.z) > 1e-6;

	if (vs->sas >= TRS_SAS_TARGET || (vs->sas == TRS_SAS_STABILITY && !manual)) {
		trs_v3 err; /* rotation vector from current to target, local frame */
		if (vs->sas >= TRS_SAS_TARGET && v3_len2(vs->sas_target) > 0) {
			trs_v3 nose = v3(0, 1, 0);
			trs_v3 tgt = v3_norm(quat_rotate(quat_conj(vs->q), vs->sas_target));
			trs_v3 ax = v3_cross(nose, tgt);
			double s = v3_len(ax), cth = v3_dot(nose, tgt);
			double ang = atan2(s, cth);
			err = s > 1e-12 ? v3_scale(ax, ang / s) : (cth < 0 ? v3(TRS_PI, 0, 0) : v3(0, 0, 0));
			err.y = 0; /* roll is free when pointing */
		} else {
			trs_quat dq = quat_mul(quat_conj(vs->q), vs->sas_hold);
			if (dq.w < 0) dq = (trs_quat){ -dq.x, -dq.y, -dq.z, -dq.w };
			double s = sqrt(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
			double ang = 2 * atan2(s, dq.w);
			err = s > 1e-12 ? v3_scale(v3(dq.x, dq.y, dq.z), ang / s) : v3(0, 0, 0);
		}
		/* PD per axis with a bandwidth the actuators can deliver */
		double I[3] = { Ix, Iy, Iz }, a[3] = { avail.x, avail.y, avail.z };
		double e[3] = { err.x, err.y, err.z }, om[3] = { wb.x, wb.y, wb.z }, out[3];
		for (int k = 0; k < 3; k++) {
			double wn = fmin(2.5, sqrt(fmax(a[k], 1e-9) / fmax(I[k], 1e-9) * 0.6));
			double t = I[k] * (wn * wn * e[k] - 2.0 * 0.9 * wn * om[k]);
			out[k] = clampd(t, -a[k], a[k]);
		}
		cmd = v3(out[0], out[1], out[2]);
	}
	if (manual) {
		trs_v3 man = v3(vs->input.x * avail.x, vs->input.z * avail.y, vs->input.y * avail.z);
		if (fabs(vs->input.x) > 1e-6) cmd.x = man.x;
		if (fabs(vs->input.z) > 1e-6) cmd.y = man.y;
		if (fabs(vs->input.y) > 1e-6) cmd.z = man.z;
		if (vs->sas == TRS_SAS_STABILITY)
			vs->sas_hold = vs->q; /* recapture while the pilot steers */
	}
	(void)Iw;
	return cmd;
}

static void integrate_attitude(trs_vessel *vs, trs_v3 torque_local, double h) {
	trs_v3 wb = quat_rotate(quat_conj(vs->q), vs->w);
	trs_v3 Iw = m3_mulv(&vs->inertia, wb);
	trs_m3 Ii = m3_inverse(&vs->inertia);
	trs_v3 wdot = m3_mulv(&Ii, v3_sub(torque_local, v3_cross(wb, Iw)));
	wb = v3_madd(wb, wdot, h);
	vs->w = quat_rotate(vs->q, wb);
	vs->q = quat_integrate(vs->q, vs->w, h);
}

/* ---- translational RK4 with linear mass loss ------------------------------------- */

typedef struct {
	const trs_world *w;
	const trs_vessel *vs;
	trs_v3 thrust_world; /* N, constant over the sub-step */
	double m0, mdot, t0;
} dyn_ctx;

static void accel(const dyn_ctx *cx, double t, trs_v3 r, trs_v3 v, trs_v3 *a, trs_v3 *a_ng) {
	double mu = cx->w->sys.bodies[cx->vs->body].mu;
	double rl2 = v3_len2(r), rl = sqrt(rl2);
	double m = cx->m0 - cx->mdot * (t - cx->t0);
	trs_v3 fa, fg;
	aero(cx->w, cx->vs, r, v, &fa, NULL);
	ground(cx->w, cx->vs, t, r, v, &fg, NULL, NULL);
	trs_v3 ng = v3_scale(v3_add(v3_add(cx->thrust_world, fa), fg), 1.0 / m);
	if (cx->vs->accel)
		ng = v3_add(ng, cx->vs->accel(cx->vs->accel_user, cx->vs->body, t, r, v));
	*a = v3_add(v3_scale(r, -mu / (rl2 * rl)), ng);
	if (a_ng)
		*a_ng = ng;
}

static void rk4_mass(const dyn_ctx *cx, trs_vessel *vs, double t, double h, trs_v3 *sensed) {
	trs_v3 r = vs->r, v = vs->v, a1, a2, a3, a4, ng;
	accel(cx, t, r, v, &a1, &ng);
	trs_v3 r2 = v3_madd(r, v, 0.5 * h), v2 = v3_madd(v, a1, 0.5 * h);
	accel(cx, t + 0.5 * h, r2, v2, &a2, NULL);
	trs_v3 r3 = v3_madd(r, v2, 0.5 * h), v3_ = v3_madd(v, a2, 0.5 * h);
	accel(cx, t + 0.5 * h, r3, v3_, &a3, NULL);
	trs_v3 r4 = v3_madd(r, v3_, h), v4 = v3_madd(v, a3, h);
	accel(cx, t + h, r4, v4, &a4, NULL);
	vs->r = v3_madd(r, v3_add(v3_add(v, v3_scale(v2, 2)), v3_add(v3_scale(v3_, 2), v4)), h / 6.0);
	vs->v = v3_madd(v, v3_add(v3_add(a1, v3_scale(a2, 2)), v3_add(v3_scale(a3, 2), a4)), h / 6.0);
	if (sensed)
		*sensed = ng;
}

/* ---- parachutes ---------------------------------------------------------------- */

static void update_chutes(const trs_world *w, trs_vessel *vs) {
	trs_craft *c = vs->craft;
	const trs_body *b = &w->sys.bodies[vs->body];
	trs_v3 vair = v3_sub(vs->v, v3_cross(trs_body_omega(b), vs->r));
	double sp = v3_len(vair);
	for (int i = 0; i < c->count; i++) {
		const trs_part_def *d = trs_craft_def(c, i);
		trs_part *p = &c->parts[i];
		if (!d->is_parachute)
			continue;
		if (p->chute == TRS_CHUTE_ARMED && vs->pressure >= d->chute_min_pressure && sp <= d->chute_max_speed)
			p->chute = TRS_CHUTE_SEMI;
		if (p->chute == TRS_CHUTE_SEMI && vs->altitude <= d->chute_full_alt)
			p->chute = TRS_CHUTE_FULL;
	}
}

/* ---- heating ------------------------------------------------------------------- */

#define SIGMA_SB 5.670374419e-8
#define SG_K 1.7415e-4 /* Sutton-Graves constant, SI (air) */
#define CP_AIR 1005.0  /* J/(kg K) */

/* Each part's temperature is that of its hottest skin element, per unit
 * area: C dT/dt = f h (T_rec - T) - e sigma (T^4 - T_amb^4).
 * The film coefficient h = 2 cp K sqrt(rho / r_nose) v is chosen so a cold
 * wall sees exactly the Sutton-Graves stagnation flux K sqrt(rho/r) v^3;
 * heating stops at the recovery temperature T_rec = T_amb + v^2 / (2 cp), so
 * subsonic flight never heats anything. f is the element's exposure: its
 * leading end face (scaled by how much of it is uncovered and how squarely
 * it meets the flow) or its side (5% of stagnation head-on, more at angle of
 * attack, less in the wake of a wider part just upstream). No conduction
 * between parts. Over max_temp the part fails. */
static void heat(const trs_world *w, trs_vessel *vs, double h) {
	trs_craft *c = vs->craft;
	const trs_body *b = &w->sys.bodies[vs->body];
	double rho = vs->density;
	trs_v3 vair = v3_sub(vs->v, v3_cross(trs_body_omega(b), vs->r));
	double sp = v3_len(vair);
	trs_v3 dl = sp > 1e-9 ? quat_rotate(quat_conj(vs->q), v3_scale(vair, 1.0 / sp)) : v3(0, 0, 0);
	double t_rec = TRS_T_AMBIENT + sp * sp / (2 * CP_AIR);
	for (int i = 0; i < c->count; i++) {
		trs_part *p = &c->parts[i];
		const trs_part_def *d = trs_craft_def(c, i);
		double q = 0;
		if (rho > 0 && sp > 1) {
			double cs = v3_dot(dl, part_axis(p));
			double exposed = (cs > 0 ? vs->face_top[i] : vs->face_bottom[i]) / fmax(d->area_axial, 1e-6);
			double wake = cs > 0 ? vs->wake_top[i] : vs->wake_bottom[i];
			double f = fmax(clampd(exposed, 0, 1) * fabs(cs), (0.05 + 0.4 * sqrt(fmax(0, 1 - cs * cs))) * wake);
			double hc = 2 * CP_AIR * SG_K * sqrt(rho / fmax(d->radius, 0.05)) * sp;
			q = f * hc * (t_rec - p->temp);
		}
		double t4 = p->temp * p->temp * p->temp * p->temp;
		double a4 = TRS_T_AMBIENT * TRS_T_AMBIENT * TRS_T_AMBIENT * TRS_T_AMBIENT;
		q -= d->emissivity * SIGMA_SB * (t4 - a4);
		p->temp += q * h / d->skin_heat;
		if (p->temp > d->max_temp) {
			int seen = 0;
			for (int k = 0; k < vs->npending; k++)
				seen |= vs->pending_uid[k] == p->uid;
			if (!seen && vs->npending < TRS_MAX_PENDING) {
				vs->pending_uid[vs->npending] = p->uid;
				vs->pending_speed[vs->npending++] = TRS_DESTROY_OVERHEAT;
			}
		}
	}
}

/* ---- the tick ------------------------------------------------------------------- */

void trs_vessel_physics_tick(trs_world *w, trs_vessel *vs, double t, double h) {
	if (vs->dirty)
		trs_vessel_refresh(w, (int)(vs - w->vessels));
	const trs_body *b = &w->sys.bodies[vs->body];
	vs->altitude = v3_len(vs->r) - b->radius;
	trs_atmosphere_at(b, vs->altitude, &vs->pressure, &vs->density);
	update_chutes(w, vs);

	engine_set es;
	gather_engines(vs, vs->pressure, &es);

	/* attitude once per tick from start-of-tick torques */
	trs_v3 fa, aero_torque;
	aero(w, vs, vs->r, vs->v, &fa, &aero_torque);
	trs_v3 fg, ground_torque;
	contact_report rep;
	ground(w, vs, t, vs->r, vs->v, &fg, &ground_torque, &rep);
	vs->contacts = rep.touching;
	if (rep.touching)
		vs->max_impact = fmax(vs->max_impact, rep.max_speed);
	trs_v3 torque = v3_add(v3_add(control_torque(w, vs, es.gimbal_authority), aero_torque), es.torque_local);
	torque = v3_add(torque, ground_torque);
	if (es.n) {
		/* gimbal trims out thrust misalignment up to its authority */
		double tm = v3_len(es.torque_local);
		if (tm > 0)
			torque = v3_sub(torque, v3_scale(es.torque_local, fmin(1.0, es.gimbal_authority / tm)));
	}

	/* translational: sub-step at flameouts so propellant never overdraws */
	double done = 0, dv = 0;
	trs_v3 sensed = v3(0, 0, 0);
	for (int guard = 0; guard < 16 && done < h; guard++) {
		gather_engines(vs, vs->pressure, &es);
		double sub = h - done;
		int to_empty = 0;
		if (es.n) {
			double tau = time_to_flameout(vs, &es);
			if (tau < sub) {
				sub = tau;
				to_empty = 1;
			}
		}
		dyn_ctx cx = { w, vs, quat_rotate(vs->q, es.force_local), vs->mass, es.total_mdot, t + done };
		trs_v3 s;
		rk4_mass(&cx, vs, t + done, sub, &s);
		if (es.n) {
			double f = v3_len(es.force_local);
			/* exact integral of F/m(t) over the sub-step */
			dv += f / es.total_mdot * log(vs->mass / (vs->mass - es.total_mdot * sub));
			burn(vs, &es, sub, to_empty);
			recentre(vs);
		}
		sensed = s;
		done += sub;
		if (sub <= 0 && !to_empty)
			break;
	}
	vs->dv_expended += dv;
	vs->sensed_accel = sensed;
	integrate_attitude(vs, torque, h);

	/* landed: touching and moving with the surface */
	if (rep.touching) {
		trs_v3 vs_rel = v3_sub(vs->v, v3_cross(trs_body_omega(b), vs->r));
		int was = vs->landed;
		vs->landed = v3_len(vs_rel) < 0.3;
		if (vs->landed && !was)
			trs_world_push_event(w, (trs_world_event){ TRS_WEV_LANDED, t + h, (int)(vs - w->vessels), vs->body, -1, "", 0 });
	} else {
		if (vs->landed)
			trs_world_push_event(w, (trs_world_event){ TRS_WEV_LIFTOFF, t + h, (int)(vs - w->vessels), vs->body, -1, "", 0 });
		vs->landed = 0;
	}
	heat(w, vs, h);
	/* queue every over-tolerance part, by uid (indices shift as parts go) */
	for (int q = 0; q < rep.ncrash && vs->npending < TRS_MAX_PENDING; q++) {
		vs->pending_uid[vs->npending] = vs->craft->parts[rep.crash_part[q]].uid;
		vs->pending_speed[vs->npending++] = rep.crash_speed[q];
	}
}

/* ---- staging --------------------------------------------------------------------- */

void trs_vessel_momentum(const trs_world *w, int id, trs_v3 *p, trs_v3 *L) {
	const trs_vessel *vs = &w->vessels[id];
	trs_v3 r, v;
	trs_vessel_state(w, id, &r, &v);
	double m = vs->craft ? vs->mass : 1.0;
	*p = v3_scale(v, m);
	*L = v3_cross(r, *p);
	if (vs->craft) {
		trs_m3 Iw = m3_rotate_tensor(vs->q, &vs->inertia);
		*L = v3_add(*L, m3_mulv(&Iw, vs->w));
	}
}

static trs_v3 world_point(const trs_vessel *vs, trs_v3 local) {
	return v3_add(vs->r, quat_rotate(vs->q, v3_sub(local, vs->cm)));
}

int trs_world_stage(trs_world *w, int id, int *new_ids, int max) {
	trs_vessel *vs = &w->vessels[id];
	if (!vs->craft || vs->next_stage < 0)
		return 0;
	if (vs->mode != TRS_MODE_PHYSICS)
		trs_vessel_set_mode(w, id, TRS_MODE_PHYSICS);
	if (vs->clamped) {
		vs->clamped = 0;
		vs->sas_hold = vs->q;
	}
	int s = vs->next_stage--;
	trs_craft *c = vs->craft;
	for (int i = 0; i < c->count; i++) {
		if (c->parts[i].stage != s)
			continue;
		const trs_part_def *d = trs_craft_def(c, i);
		if (d->propellant != TRS_PROP_NONE)
			c->parts[i].engine_on = 1;
		if (d->is_leg)
			c->parts[i].legs_deployed = 1;
	}
	int made = 0;
	for (int guard = 0; guard < 256; guard++) {
		c = w->vessels[id].craft;
		int dec = -1;
		for (int i = 0; i < c->count; i++)
			if (c->parts[i].stage == s && trs_craft_def(c, i)->decoupler && !c->parts[i].fired) {
				dec = i;
				break;
			}
		if (dec < 0)
			break;
		c->parts[dec].fired = 1;
		vs = &w->vessels[id];

		/* separation geometry, before the split */
		const trs_part *dp = &c->parts[dec];
		const trs_part_def *dd = trs_craft_def(c, dec);
		unsigned char *mask = calloc((size_t)c->count, 1);
		trs_craft_detach_set(c, dec, mask);
		int detaching_decoupler = mask[dec];
		free(mask);
		trs_v3 n_local, p_local;
		if (dd->decoupler == TRS_DECOUPLER_RADIAL) {
			n_local = quat_rotate(dp->rot, v3(0, 0, 1)); /* outward */
			p_local = dp->pos;
		} else {
			const trs_attach_node *top = trs_part_node(dd, "top");
			trs_v3 up = quat_rotate(dp->rot, v3(0, 1, 0));
			p_local = v3_add(dp->pos, quat_rotate(dp->rot, top ? top->pos : v3(0, 0, 0)));
			n_local = detaching_decoupler ? v3_neg(up) : up; /* push the detached side away */
		}
		double J = dd->eject_impulse;

		trs_craft piece;
		if (!trs_craft_split(c, dec, &piece))
			continue;
		if (w->nvessels >= TRS_MAX_VESSELS) {
			trs_craft_free(&piece);
			continue;
		}
		double m_old = vs->mass;
		trs_v3 cm_old = vs->cm, r_old = vs->r, v_old = vs->v, w_old = vs->w;
		trs_quat q = vs->q;
		(void)m_old;

		trs_craft *pc = malloc(sizeof(trs_craft));
		*pc = piece;
		int nid = trs_world_add_vessel(w, vs->body, r_old, v_old);
		vs = &w->vessels[id];
		trs_vessel *nv = &w->vessels[nid];
		nv->craft = pc;
		nv->q = q;
		nv->w = w_old;
		nv->sas = TRS_SAS_OFF;
		nv->next_stage = s - 1;
		nv->dirty = 1;
		trs_vessel_refresh(w, nid);
		trs_vessel_refresh(w, id);

		/* rigid-body kinematics: each piece keeps the velocity its CoM had */
		trs_v3 dA = quat_rotate(q, v3_sub(vs->cm, cm_old));
		trs_v3 dB = quat_rotate(q, v3_sub(nv->cm, cm_old));
		vs->r = v3_add(r_old, dA);
		vs->v = v3_add(v_old, v3_cross(w_old, dA));
		nv->r = v3_add(r_old, dB);
		nv->v = v3_add(v_old, v3_cross(w_old, dB));
		/* equal and opposite ejection impulse at the separation plane */
		trs_v3 n = quat_rotate(q, v3_norm(n_local));
		trs_v3 P = v3_add(r_old, quat_rotate(q, v3_sub(p_local, cm_old)));
		trs_v3 Jv = v3_scale(n, J);
		nv->v = v3_madd(nv->v, Jv, 1.0 / nv->mass);
		vs->v = v3_madd(vs->v, Jv, -1.0 / vs->mass);
		trs_m3 IA = m3_rotate_tensor(q, &vs->inertia), IB = m3_rotate_tensor(q, &nv->inertia);
		trs_m3 IAi = m3_inverse(&IA), IBi = m3_inverse(&IB);
		nv->w = v3_add(nv->w, m3_mulv(&IBi, v3_cross(v3_sub(P, nv->r), Jv)));
		vs->w = v3_add(vs->w, m3_mulv(&IAi, v3_cross(v3_sub(P, vs->r), v3_neg(Jv))));
		if (made < max && new_ids)
			new_ids[made] = nid;
		made++;
	}
	return made < max ? made : max;
}

int trs_vessel_deploy_chutes(trs_world *w, int id) {
	trs_vessel *vs = &w->vessels[id];
	if (!vs->craft)
		return 0;
	int n = 0;
	for (int i = 0; i < vs->craft->count; i++)
		if (trs_craft_def(vs->craft, i)->is_parachute && vs->craft->parts[i].chute == TRS_CHUTE_STOWED) {
			vs->craft->parts[i].chute = TRS_CHUTE_ARMED;
			n++;
		}
	return n;
}

/* ---- destruction, launch ------------------------------------------------------- */

/* Spawns a vessel for a detached piece, moving rigidly with the parent. */
static int spawn_piece(trs_world *w, int parent_id, trs_craft *piece, trs_v3 cm_old) {
	trs_vessel *vs = &w->vessels[parent_id];
	trs_v3 r_old = vs->r, v_old = vs->v, w_old = vs->w;
	trs_quat q = vs->q;
	int body = vs->body;
	int nid = trs_world_add_vessel(w, body, r_old, v_old);
	if (nid < 0) {
		trs_craft_free(piece);
		return -1;
	}
	trs_vessel *nv = &w->vessels[nid];
	nv->craft = malloc(sizeof(trs_craft));
	*nv->craft = *piece;
	nv->q = q;
	nv->w = w_old;
	nv->next_stage = trs_craft_max_stage(piece);
	trs_vessel_refresh(w, nid);
	trs_v3 d = quat_rotate(q, v3_sub(nv->cm, cm_old));
	nv->r = v3_add(r_old, d);
	nv->v = v3_add(v_old, v3_cross(w_old, d));
	return nid;
}

/* A part going up with propellant in it is a bomb: every part (on any
 * vessel in the same SOI) whose centre is inside the blast radius is queued
 * for destruction next tick, so failures cascade the way real ones do. The
 * radius grows with the cube root of the propellant mass. */
static void blast(trs_world *w, int body, trs_v3 at, double fuel, int skip_uid) {
	if (fuel < 50)
		return;
	double R = 1.6 * cbrt(fuel / 100.0);
	for (int j = 0; j < w->nvessels; j++) {
		trs_vessel *o = &w->vessels[j];
		if (!o->alive || !o->craft || o->body != body || o->mode != TRS_MODE_PHYSICS)
			continue;
		if (v3_len(v3_sub(o->r, at)) > R + o->bound_radius + 1)
			continue;
		for (int k = 0; k < o->craft->count && o->npending < TRS_MAX_PENDING; k++) {
			const trs_part *pk = &o->craft->parts[k];
			if (pk->uid == skip_uid || v3_len(v3_sub(world_point(o, pk->pos), at)) > R)
				continue;
			int seen = 0;
			for (int q = 0; q < o->npending; q++)
				seen |= o->pending_uid[q] == pk->uid;
			if (!seen) {
				o->pending_uid[o->npending] = pk->uid;
				o->pending_speed[o->npending++] = TRS_DESTROY_BLAST;
			}
		}
	}
}

void trs_vessel_destroy_part(trs_world *w, int id, int part, double speed) {
	trs_vessel *vs = &w->vessels[id];
	trs_craft *c = vs->craft;
	if (!c || part < 0 || part >= c->count)
		return;
	{
		const trs_part_def *d = trs_craft_def(c, part);
		double fuel = (d->fuel_liquid > 0 || d->fuel_solid > 0) ? c->parts[part].fuel : 0;
		blast(w, vs->body, world_point(vs, c->parts[part].pos), fuel, c->parts[part].uid);
		vs = &w->vessels[id];
		c = vs->craft;
	}
	trs_world_event ev = { TRS_WEV_PART_DESTROYED, w->t, id, -1, c->parts[part].uid, "", speed };
	snprintf(ev.part, sizeof ev.part, "%s", trs_craft_def(c, part)->id);
	trs_world_push_event(w, ev);
	trs_v3 cm_old = vs->cm, r_old = vs->r, v_old = vs->v;
	trs_craft pieces[32];
	int n = trs_craft_remove_part(c, part, pieces, 32);
	for (int k = 0; k < n; k++) {
		int nid = spawn_piece(w, id, &pieces[k], cm_old);
		if (nid >= 0)
			trs_world_push_event(w, (trs_world_event){ TRS_WEV_VESSEL_CREATED, w->t, id, nid, -1, "", 0 });
	}
	vs = &w->vessels[id];
	if (vs->craft->count == 0) {
		trs_world_remove_vessel(w, id);
		return;
	}
	trs_vessel_refresh(w, id);
	trs_v3 d = quat_rotate(vs->q, v3_sub(vs->cm, cm_old));
	vs->r = v3_add(r_old, d);
	vs->v = v3_add(v_old, v3_cross(vs->w, d));
}

int trs_world_launch(trs_world *w, const trs_craft *c, int body, double lat, double lon) {
	const trs_body *b = &w->sys.bodies[body];
	trs_v3 up = v3(cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat));
	trs_v3 east = v3_norm(v3_cross(v3(0, 0, 1), up));
	trs_v3 south = v3_cross(east, up);
	int id = trs_world_add_craft(w, c, body, v3_scale(up, b->radius * 2), v3(0, 0, 0), quat_identity());
	if (id < 0)
		return id;
	trs_vessel *vs = &w->vessels[id];
	double min_y = INFINITY;
	for (int k = 0; k < vs->ncontact; k++)
		min_y = fmin(min_y, vs->contact_pt[k].y);
	double h = b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, up) : 0;
	vs->mode = TRS_MODE_LANDED;
	vs->fixed_r = v3_scale(up, b->radius + h + (vs->cm.y - min_y) + 0.01);
	vs->fixed_q = quat_from_basis(east, up, south);
	vs->clamped = 1;
	vs->landed = 1;
	vs->sas_hold = vs->fixed_q;
	/* keep r/v/q coherent for anything that reads them directly */
	vs->r = trs_body_from_fixed(b, w->t, vs->fixed_r);
	vs->v = v3_cross(trs_body_omega(b), vs->r);
	vs->q = quat_normalize(quat_mul(trs_body_quat(b, w->t), vs->fixed_q));
	vs->w = trs_body_omega(b);
	return id;
}
