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
/* Which end of part i its node `name` sits on: +1 top, -1 bottom, 0 a
 * side mount or no such node. */
static int node_end(const trs_craft *c, int i, const char *name) {
	const trs_attach_node *n = name[0] ? trs_part_node(trs_craft_def(c, i), name) : NULL;
	return !n ? 0 : n->dir.y > 0.5 ? 1 : n->dir.y < -0.5 ? -1 : 0;
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
		if (d->is_wheel) {
			local[m++] = v3(0, 0, d->wheel_length); /* off the hull, always down */
			leg = 1;
		} else if (d->is_leg) {
			local[m++] = trs_leg_foot(d, p->legs_deployed ? 1.0 : 0.0);
			leg = p->legs_deployed;
		} else if (d->plate) {
			/* fins and wings: the root's ends and the tip */
			local[m++] = v3(0, d->height * 0.5, 0);
			local[m++] = v3(0, -d->height * 0.5, 0);
			local[m++] = v3(0, -d->height * 0.5, d->fin_span);
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
	free(vs->face_top);
	free(vs->face_bottom);
	free(vs->wake_top);
	free(vs->wake_bottom);
	int n = c->count ? c->count : 1;
	vs->face_top = malloc(sizeof(double) * (size_t)n);
	vs->face_bottom = malloc(sizeof(double) * (size_t)n);
	vs->wake_top = malloc(sizeof(double) * (size_t)n);
	vs->wake_bottom = malloc(sizeof(double) * (size_t)n);
	trs_fuel_net_free(&vs->fuel);
	trs_fuel_net_build(&vs->fuel, c);
	/* what is stacked on each end of each part: the area it covers there,
	 * and the widest neighbour (several engines can hang under one mount) */
	double *cover_top = calloc((size_t)n, sizeof(double)), *cover_bottom = calloc((size_t)n, sizeof(double));
	double *wide_top = calloc((size_t)n, sizeof(double)), *wide_bottom = calloc((size_t)n, sizeof(double));
	for (int k = 0; k < c->count; k++) {
		int p = c->parts[k].parent;
		if (p < 0)
			continue;
		int ek = node_end(c, k, c->parts[k].node), ep = node_end(c, p, c->parts[k].parent_node);
		double ak = trs_craft_def(c, k)->area_axial, ap = trs_craft_def(c, p)->area_axial;
		if (ek && ep) {
			(ek > 0 ? cover_top : cover_bottom)[k] += ap;
			(ek > 0 ? wide_top : wide_bottom)[k] = fmax((ek > 0 ? wide_top : wide_bottom)[k], ap);
			(ep > 0 ? cover_top : cover_bottom)[p] += ak;
			(ep > 0 ? wide_top : wide_bottom)[p] = fmax((ep > 0 ? wide_top : wide_bottom)[p], ak);
		}
	}
	for (int i = 0; i < c->count; i++) {
		const trs_part_def *d = trs_craft_def(c, i);
		/* parts with no node on a face (nosecone tip, chute) keep it exposed */
		vs->face_top[i] = fmax(0, d->area_axial - cover_top[i]);
		vs->face_bottom[i] = fmax(0, d->area_axial - cover_bottom[i]);
		/* a part right behind a wider one sits in its wake: side heating
		 * scales with the share of the frontal area it would present */
		vs->wake_top[i] = wide_top[i] > 0 ? fmin(1, d->area_axial / fmax(wide_top[i], 1e-6)) : 1;
		vs->wake_bottom[i] = wide_bottom[i] > 0 ? fmin(1, d->area_axial / fmax(wide_bottom[i], 1e-6)) : 1;
	}
	free(cover_top);
	free(cover_bottom);
	free(wide_top);
	free(wide_bottom);
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
		if (!p->engine_on || d->propellant == TRS_PROP_NONE || !trs_craft_engine_fed(c, &vs->fuel, i))
			continue;
		double thr = d->propellant == TRS_PROP_SOLID ? 1.0 : vs->throttle;
		if (thr <= 0)
			continue;
		double air = trs_engine_air_factor(d, vs->density, vs->air_speed);
		if (air <= 0)
			continue; /* a jet in vacuum, or past its top speed */
		int k = es->n++;
		es->idx[k] = i;
		es->mdot[k] = trs_part_mdot(c, i) * thr * air;
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

static double clampd(double x, double lo, double hi) {
	return x < lo ? lo : x > hi ? hi : x;
}

/* RCS mixer. Each nozzle pushes opposite its exhaust, so it can add to a
 * turn (its torque about the CoM along the commanded rotation) and to a
 * slide (its push along the commanded translation); it fires at the sum of
 * the two shares, clamped to full. Opposite blocks cancel each other's
 * side effects. rot is per local axis (X pitch, Y roll, Z yaw), tr is the
 * vessel-local direction to move, both in [-1, 1]. Returns force and
 * torque (vessel-local) and the monopropellant flow; writes each nozzle's
 * throttle when `fire` is set. */
static void rcs_mix(trs_vessel *vs, trs_v3 rot, trs_v3 tr, double pressure, int fire, trs_v3 *force, trs_v3 *torque,
		double *mdot) {
	trs_craft *c = vs->craft;
	trs_v3 F = v3(0, 0, 0), T = v3(0, 0, 0);
	double md = 0;
	for (int i = 0; i < c->count; i++) {
		const trs_part_def *d = trs_craft_def(c, i);
		trs_part *p = &c->parts[i];
		for (int k = 0; k < d->rcs_n; k++) {
			trs_v3 f = v3_neg(quat_rotate(p->rot, d->rcs_dir[k]));
			trs_v3 at = v3_add(p->pos, quat_rotate(p->rot, d->rcs_pos[k]));
			trs_v3 t = v3_cross(v3_sub(at, vs->cm), f);
			double tl = v3_len(t);
			double r = tl > 1e-6 ? v3_dot(rot, t) / tl : 0;
			double thr = clampd(fmax(r, 0) + fmax(v3_dot(tr, f), 0), 0, 1);
			if (fire)
				p->rcs_fire[k] = thr;
			if (thr <= 0)
				continue;
			double m = thr * d->rcs_thrust / (d->rcs_isp_vac * TRS_G0);
			double isp = d->rcs_isp_vac + (d->rcs_isp_sl - d->rcs_isp_vac) * (pressure / TRS_P_REF);
			double fn = m * fmax(isp, 0.01 * d->rcs_isp_vac) * TRS_G0; /* as trs_engine_isp */
			F = v3_madd(F, f, fn);
			T = v3_madd(T, t, fn);
			md += m;
		}
	}
	*force = F;
	*torque = T;
	*mdot = md;
}

/* Torque the RCS can put on each local axis (the weaker way round). */
static trs_v3 rcs_authority(trs_vessel *vs) {
	double a[3];
	for (int k = 0; k < 3; k++) {
		double e[3] = { 0, 0, 0 };
		e[k] = 1;
		trs_v3 F, Tp, Tn;
		double m;
		rcs_mix(vs, v3(e[0], e[1], e[2]), v3(0, 0, 0), vs->pressure, 0, &F, &Tp, &m);
		rcs_mix(vs, v3(-e[0], -e[1], -e[2]), v3(0, 0, 0), vs->pressure, 0, &F, &Tn, &m);
		double tp[3] = { Tp.x, Tp.y, Tp.z }, tn[3] = { Tn.x, Tn.y, Tn.z };
		a[k] = fmax(0, fmin(tp[k], -tn[k]));
	}
	return v3(a[0], a[1], a[2]);
}

static int rcs_armed(const trs_vessel *vs) {
	return vs->rcs && trs_craft_mono(vs->craft, NULL) > 0;
}

/* Liquid draw per fuel group (kg/s) for the lit engines; rate[ngroups]. */
static void group_rates(const trs_vessel *vs, const engine_set *es, double *rate) {
	const trs_craft *c = vs->craft;
	double *md = calloc((size_t)(c->count ? c->count : 1), sizeof(double));
	for (int k = 0; k < es->n; k++)
		md[es->idx[k]] = es->mdot[k];
	trs_fuel_rates(&vs->fuel, c, md, rate);
	free(md);
}

/* Seconds until the first drawn propellant pool among the lit engines runs dry. */
static double time_to_flameout(const trs_vessel *vs, const engine_set *es) {
	const trs_craft *c = vs->craft;
	double tau = INFINITY;
	for (int k = 0; k < es->n; k++) {
		int i = es->idx[k];
		if (trs_craft_def(c, i)->propellant == TRS_PROP_SOLID)
			tau = fmin(tau, c->parts[i].fuel / es->mdot[k]);
	}
	int ng = vs->fuel.ngroups > 0 ? vs->fuel.ngroups : 1;
	double *rate = calloc((size_t)ng, sizeof(double));
	double *gf = calloc((size_t)ng, sizeof(double));
	group_rates(vs, es, rate);
	trs_fuel_levels(&vs->fuel, c, gf);
	for (int g = 0; g < vs->fuel.ngroups; g++)
		if (rate[g] > 0)
			tau = fmin(tau, gf[g] / rate[g]);
	free(rate);
	free(gf);
	return tau;
}

static void burn(trs_vessel *vs, const engine_set *es, double h, int to_empty) {
	trs_craft *c = vs->craft;
	int ng = vs->fuel.ngroups > 0 ? vs->fuel.ngroups : 1;
	double *rate = calloc((size_t)ng, sizeof(double));
	group_rates(vs, es, rate);
	for (int k = 0; k < es->n; k++) {
		int i = es->idx[k];
		if (trs_craft_def(c, i)->propellant == TRS_PROP_SOLID) {
			c->parts[i].fuel -= es->mdot[k] * h;
			if (c->parts[i].fuel < FUEL_EPS * 10 || (to_empty && c->parts[i].fuel < 1e-6))
				c->parts[i].fuel = 0;
		}
	}
	for (int g = 0; g < vs->fuel.ngroups; g++)
		if (rate[g] > 0) {
			double f = trs_craft_group_fuel(c, vs->fuel.group, g), draw = rate[g] * h;
			trs_craft_group_draw(c, vs->fuel.group, g, (to_empty && draw > f * (1 - 1e-9)) ? f : draw);
		}
	free(rate);
}

/* Total aerodynamic force (SOI frame) and torque about the CoM (vessel-local).
 *
 * Each part sees the air at its own velocity (the CoM's plus the spin's
 * w x arm), so a tumbling craft is damped the way a real one is. A body's
 * force splits into an axial part on the face meeting the air and a
 * cross-flow part (Newtonian, ~sin^2 of the angle of attack) acting at the
 * part's centre of pressure, plus, on a tapering nose, a slender-body normal
 * force linear in the angle. Fins are lifting plates: they push only along
 * their normal, linearly in the angle until they stall, which is what keeps
 * rockets pointed.
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
	trs_v3 damp = v3(0, 0, 0); /* rotational damping torque, vessel-local */
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
		trs_v3 f_nose = v3(0, 0, 0), nose_at = arm;
		if (d->plate) {
			/* a fin lifts like a low aspect-ratio wing at small angles (normal
			 * force linear in the angle), going over to flat-plate cross-flow
			 * as it stalls toward 90 degrees */
			trs_v3 n = quat_rotate(pt->rot, v3(1, 0, 0));
			double cn = v3_dot(dl, n);
			double cnf = d->lift_slope * cn * sqrt(fmax(0, 1 - cn * cn)) + d->cd_side * cn * fabs(cn);
			f_cross = v3_scale(n, -qd * d->area_side * cnf);
		} else {
			trs_v3 perp = v3_sub(dl, v3_scale(ax, cs));
			f_cross = v3_scale(perp, -qd * d->cd_side * d->area_side * v3_len(perp));
			/* a tapering nose meeting the air at an angle pushes sideways in
			 * proportion to the angle (slender-body theory: N = q dA sin 2a,
			 * dA the cross-section it grows by), centred two thirds of the way
			 * down the taper. It sits ahead of the CoM on a rocket, so without
			 * fins aft to outweigh it a rocket turned off the airflow keeps
			 * turning. Blunt leading faces (heat shields, flat tops) do not. */
			double r_lead = cs > 0 ? d->radius_top : d->radius_bottom, r_tail = cs > 0 ? d->radius_bottom : d->radius_top;
			if (r_lead < r_tail - 1e-9) {
				double exposed = cs > 0 ? vs->face_top[i] : vs->face_bottom[i];
				double grow = fmin(exposed, TRS_PI * (r_tail * r_tail - r_lead * r_lead));
				f_nose = v3_scale(perp, -qd * grow * 2 * fabs(cs));
				nose_at = v3_add(arm, v3_scale(ax, (cs > 0 ? -1 : 1) * d->height / 6));
			}
		}
		trs_v3 f_chute = v3(0, 0, 0);
		if (d->is_parachute) {
			f_chute = v3_scale(dl, -qd * pt->chute_cda);
		}
		trs_v3 f = v3_add(v3_add(v3_add(f_axial, f_cross), f_chute), f_nose);
		f_local_total = v3_add(f_local_total, f);
		if (torque_local) {
			trs_v3 cop = v3_add(arm, v3_scale(ax, d->cop * d->height));
			*torque_local = v3_add(*torque_local, v3_add(v3_cross(arm, v3_add(f_axial, f_chute)), v3_cross(cop, f_cross)));
			*torque_local = v3_add(*torque_local, v3_cross(nose_at, f_nose));
			/* the part's own extent resists turning end over end: linearised
			 * cross-flow on a spinning slab of side area A and length L, with
			 * an empirical factor standing in for the pitch-damping derivative */
			double wa = v3_dot(wl, ax);
			trs_v3 w_perp = v3_sub(wl, v3_scale(ax, wa));
			double cq = 0.5 * rho * sp * d->cd_side * d->area_side;
			double L = fmax(d->height, 2 * d->radius);
			damp = v3_sub(damp, v3_scale(w_perp, cq * L * L));
			damp = v3_sub(damp, v3_scale(ax, cq * d->radius * d->radius * 0.1 * wa));
		}
	}
	if (torque_local) {
		/* applied over a whole tick, damping may at most halve the spin it
		 * acts on: more would reverse it and grow (a small, fast-spinning
		 * piece of debris in thick air) */
		double lim[3] = { vs->inertia.m[0][0], vs->inertia.m[1][1], vs->inertia.m[2][2] };
		double wv[3] = { wl.x, wl.y, wl.z }, dv[3] = { damp.x, damp.y, damp.z };
		for (int k = 0; k < 3; k++) {
			double cap = 0.5 * lim[k] * fabs(wv[k]) / TRS_PHYSICS_DT;
			dv[k] = fmax(-cap, fmin(cap, dv[k]));
		}
		*torque_local = v3_add(*torque_local, v3(dv[0], dv[1], dv[2]));
	}
	*force = quat_rotate(vs->q, f_local_total);
}

/* ---- ground contact ----------------------------------------------------------- */

#define CONTACT_OMEGA 22.0  /* rad/s: hull stiffness, well inside RK4 stability at 50 Hz */
#define CONTACT_ZETA 0.8
#define RADIAL_CLEAR_S 2.0 /* s a radially dropped piece passes through its core */
#define GROUND_SKIP 2.0 /* m: see ground() */
#define CONTACT_ROT_OMEGA 15.0 /* rad/s: cap on the contacts' stiffness against turning */
#define RUNWAY_EAST 400.0 /* m from the pad to the runway's west end */
#define CONTACT_DAMP_STEP 0.5 /* cap on (rotational damping rate) x tick */
#define CONTACT_TRANS_OMEGA 35.0 /* rad/s: cap on all the contacts' stiffness together, for the RK4 step */
#define FRICTION_MU 0.5
#define SLIP_REG 0.08 /* m/s: friction regularisation */

#define MAX_CRASH 64

typedef struct {
	int touching;
	int ncrash;          /* parts hitting faster than their tolerance */
	int crash_part[MAX_CRASH];
	double crash_speed[MAX_CRASH];
	double max_speed;
	trs_v3 friction_torque; /* the part of the torque from friction, vessel-local */
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
		rep->friction_torque = v3(0, 0, 0);
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
	/* The contact torque is held over the tick (attitude steps once), so the
	 * springs' combined stiffness against turning must stay well inside what
	 * that step can follow: a long rocket landing on one end would otherwise
	 * see sum k |arm x n|^2 / I far past 1/h^2 and spin up without bound.
	 * Each spring turns the craft about its own axis u = arm x n, against
	 * the inertia about that axis: sum k u.I^-1.u bounds the fastest turning
	 * mode (it is the trace of I^-1 K). Measuring against the smallest
	 * inertia instead would make a rocket lying on its side (where the
	 * springs turn it end over end, its largest inertia) so soft it sinks.
	 * All the springs together are also held inside what the translational
	 * step can follow. */
	double rot_k = 0, k_sum = 0, rot_c = 0;
	trs_m3 i_inv = m3_inverse(&vs->inertia);
	/* Depth of each contact point. The terrain is sampled only near the
	 * ground: a point standing well clear of the local ground plane (more
	 * than GROUND_SKIP plus its sideways distance from the vessel, i.e. any
	 * slope steeper than 45 degrees off that plane) cannot be touching. */
	double *depths = malloc(sizeof(double) * (size_t)(vs->ncontact > 0 ? vs->ncontact : 1));
	/* a walking penguin's feet move under it at the speed it wants (up to
	 * its walking speed), so friction pushes it toward that speed over the
	 * ground, no harder than friction can */
	trs_v3 feet = v3(0, 0, 0);
	if (trs_vessel_is_eva(vs)) {
		trs_v3 up, north, east;
		trs_local_frame(r, &up, &north, &east);
		feet = v3_add(v3_scale(east, vs->walk.x), v3_scale(north, vs->walk.y));
		feet = v3_sub(feet, v3_scale(nrm, v3_dot(feet, nrm)));
		double fl = v3_len(feet), top = trs_craft_def(c, 0)->eva_walk;
		if (fl > top)
			feet = v3_scale(feet, top / fl);
	}
	trs_v3 g0 = v3_scale(r, (b->radius + h_here) / rl);
	for (int k = 0; k < vs->ncontact; k++) {
		trs_v3 arm = quat_rotate(vs->q, v3_sub(vs->contact_pt[k], vs->cm));
		trs_v3 p = v3_add(r, arm);
		trs_v3 dp = v3_sub(p, g0);
		double above = v3_dot(dp, nrm);
		if (above > GROUND_SKIP + v3_len(v3_sub(dp, v3_scale(nrm, above)))) {
			depths[k] = -1;
			continue;
		}
		double pl = v3_len(p);
		depths[k] = (b->radius + trs_body_terrain_at(b, t, p) - pl) * v3_dot(nrm, v3_scale(p, 1.0 / pl));
		if (depths[k] > 0) {
			double kk = vs->contact_leg[k] ? trs_craft_def(c, vs->contact_part[k])->leg_spring : k_hull;
			double ck = vs->contact_leg[k] ? trs_craft_def(c, vs->contact_part[k])->leg_damper : c_hull;
			trs_v3 u = quat_rotate(quat_conj(vs->q), v3_cross(arm, nrm));
			double ui = v3_dot(u, m3_mulv(&i_inv, u));
			rot_k += kk * ui;
			rot_c += ck * ui;
			k_sum += kk;
		}
	}
	double soften = rot_k > 0 ? fmin(1, CONTACT_ROT_OMEGA * CONTACT_ROT_OMEGA / rot_k) : 1;
	if (k_sum > 0)
		soften = fmin(soften, CONTACT_TRANS_OMEGA * CONTACT_TRANS_OMEGA * m / k_sum);
	/* the dampers too: a turning rate is damped by sum c u.I^-1.u per
	 * second, and an explicit step overshoots (and chatters) once that
	 * times the tick passes about 1 */
	double damp = sqrt(soften);
	if (rot_c * damp * TRS_PHYSICS_DT > CONTACT_DAMP_STEP)
		damp = CONTACT_DAMP_STEP / (rot_c * TRS_PHYSICS_DT);
	for (int k = 0; k < vs->ncontact; k++) {
		double depth = depths[k];
		if (depth <= 0)
			continue;
		trs_v3 arm = quat_rotate(vs->q, v3_sub(vs->contact_pt[k], vs->cm));
		trs_v3 p = v3_add(r, arm);
		trs_v3 vp = v3_sub(v3_add(v, v3_cross(vs->w, arm)), v3_cross(omega_b, p));
		double vn = v3_dot(vp, nrm);
		double kk = k_hull, cc = c_hull;
		const trs_part_def *d = trs_craft_def(c, vs->contact_part[k]);
		if (vs->contact_leg[k]) {
			kk = d->leg_spring;
			cc = d->leg_damper;
		}
		kk *= soften;
		cc *= damp;
		double fn = kk * depth - cc * vn;
		if (fn < 0)
			fn = 0;
		trs_v3 vt = v3_sub(v3_sub(vp, v3_scale(nrm, vn)), feet);
		double vtl = v3_len(vt);
		trs_v3 ft = v3_scale(vt, -FRICTION_MU * fn / fmax(vtl, SLIP_REG));
		if (d->is_wheel) {
			/* a wheel rolls along the craft's nose (steered, ahead of the
			 * centre of mass, toward the turn the pilot asks for about the
			 * ground's normal) and grips across it */
			trs_v3 nose = quat_rotate(vs->q, v3(0, 1, 0));
			trs_v3 roll = v3_sub(nose, v3_scale(nrm, v3_dot(nose, nrm)));
			double rl = v3_len(roll);
			roll = rl > 1e-6 ? v3_scale(roll, 1.0 / rl) : v3_norm(v3_cross(nrm, quat_rotate(vs->q, v3(1, 0, 0))));
			if (d->wheel_steer > 0 && c->parts[vs->contact_part[k]].pos.y > vs->cm.y) {
				trs_v3 want = quat_rotate(vs->q, v3(vs->input.x, vs->input.z, vs->input.y));
				double turn = clampd(v3_dot(want, nrm), -1, 1) * d->wheel_steer;
				roll = v3_add(v3_scale(roll, cos(turn)), v3_scale(v3_cross(nrm, roll), sin(turn)));
			}
			double vr = v3_dot(vt, roll);
			trs_v3 vs_side = v3_sub(vt, v3_scale(roll, vr));
			double vsl = v3_len(vs_side);
			double mu_r = vs->brakes ? d->wheel_mu_brake : d->wheel_mu_roll;
			ft = v3_add(v3_scale(roll, -mu_r * fn * vr / fmax(fabs(vr), SLIP_REG)),
					v3_scale(vs_side, -d->wheel_mu_side * fn / fmax(vsl, SLIP_REG)));
		}
		trs_v3 f = v3_add(v3_scale(nrm, fn), ft);
		*force = v3_add(*force, f);
		if (torque_local)
			*torque_local = v3_add(*torque_local, quat_rotate(quat_conj(vs->q), v3_cross(arm, f)));
		if (rep) {
			rep->friction_torque = v3_add(rep->friction_torque, quat_rotate(quat_conj(vs->q), v3_cross(arm, ft)));
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
	free(depths);
}

/* ---- attitude ------------------------------------------------------------------ */


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
	/* in the orbit plane, square to the velocity, away from the body (as
	 * the maneuver nodes' radial axis) */
	case TRS_SAS_RADIAL_OUT: return v3_norm(v3_cross(v3_norm(v), v3_norm(h)));
	case TRS_SAS_RADIAL_IN: return v3_neg(v3_norm(v3_cross(v3_norm(v), v3_norm(h))));
	case TRS_SAS_SURF_PROGRADE: return v3_norm(vs_rel);
	case TRS_SAS_SURF_RETROGRADE: return v3_neg(v3_norm(vs_rel));
	case TRS_SAS_TARGET: return vs->sas_target;
	case TRS_SAS_UPRIGHT: return v3_norm(r);
	default: return quat_rotate(vs->q, v3(0, 1, 0));
	}
}

/* SAS gains: target-rate per radian of error, and the rate loop's 1/s. The
 * rate loop is explicit, so SAS_KR * TRS_PHYSICS_DT stays well under 1. */
#define SAS_KP 4.0
#define SAS_KR 15.0
#define SAS_SETTLED 0.002 /* rad/s: a let-go turn counts as stopped */

/* a fin's torque per newton along its plate normal (local X), craft frame */
static trs_v3 fin_lever(const trs_craft *c, const trs_vessel *vs, int i) {
	return v3_cross(v3_sub(c->parts[i].pos, vs->cm), quat_rotate(c->parts[i].rot, v3(1, 0, 0)));
}

static trs_v3 control_torque(const trs_world *world, trs_vessel *vs, double gimbal_authority) {
	if (vs->sas >= TRS_SAS_PROGRADE) {
		trs_v3 d = trs_sas_direction(world, (int)(vs - world->vessels), vs->sas);
		if (v3_len2(d) > 0.5)
			vs->sas_target = d;
	}
	trs_craft *c = vs->craft;
	/* reaction torque that runs on electricity stops when it runs out; an
	 * uncrewed craft out of power can't be steered at all */
	double wheel = 0;
	int crewed = 0;
	for (int i = 0; i < c->count; i++) {
		const trs_part_def *d = trs_craft_def(c, i);
		crewed |= d->crew > 0;
		if (vs->powered || d->torque_kw_per_knm <= 0)
			wheel += d->torque;
	}
	vs->wheel_load = 0;
	/* movable fins steer in the air: each can push q S CNa sin(deflection)
	 * along its normal at its arm, so a rocket with more (and further aft)
	 * fins answers the controls better, and none of it works in vacuum */
	double fin[3] = { 0, 0, 0 };
	double air_from_nose = 1; /* +1 air meets the nose, -1 flying tail first */
	for (int i = 0; i < c->count; i++)
		c->parts[i].fin_flap = 0;
	if (vs->density > 0) {
		const trs_body *b = &world->sys.bodies[vs->body];
		trs_v3 vair = v3_sub(vs->v, v3_cross(trs_body_omega(b), vs->r));
		double q = 0.5 * vs->density * v3_len2(vair);
		air_from_nose = v3_dot(vair, quat_rotate(vs->q, v3(0, 1, 0))) < 0 ? -1 : 1;
		for (int i = 0; i < c->count; i++) {
			const trs_part_def *d = trs_craft_def(c, i);
			if (!d->plate || d->fin_deflect <= 0)
				continue;
			double f = q * d->area_side * d->lift_slope * sin(d->fin_deflect);
			trs_v3 tq = fin_lever(c, vs, i);
			fin[0] += f * fabs(tq.x), fin[1] += f * fabs(tq.y), fin[2] += f * fabs(tq.z);
		}
	}
	/* available torque per local axis: X pitch, Y roll, Z yaw; armed RCS
	 * adds its own, used only for what the rest can't give (mono is
	 * precious) */
	trs_v3 own = v3(wheel + gimbal_authority + fin[0], wheel + fin[1], wheel + gimbal_authority + fin[2]);
	trs_v3 rcs_a = rcs_armed(vs) ? rcs_authority(vs) : v3(0, 0, 0);
	trs_v3 avail = v3_add(own, rcs_a);
	trs_v3 wb = quat_rotate(quat_conj(vs->q), vs->w);
	trs_v3 Iw = m3_mulv(&vs->inertia, wb);
	double Ix = vs->inertia.m[0][0], Iy = vs->inertia.m[1][1], Iz = vs->inertia.m[2][2];
	trs_v3 cmd = v3(0, 0, 0);
	if (!crewed && !vs->powered)
		return cmd; /* a dead probe: nothing answers the controls */
	int manual = fabs(vs->input.x) + fabs(vs->input.y) + fabs(vs->input.z) > 1e-6;
	/* Letting go of the stick under stability SAS stops the turn where it
	 * is (rate brought to rest, the hold point following the nose) and only
	 * then holds that attitude, so the nose does not swing back to where it
	 * was when the key came up. Braking ends once the rate is next to
	 * nothing, or stops falling (air pushing on it: holding takes over). */
	if (vs->sas == TRS_SAS_STABILITY && !manual && vs->sas_settle > 0) {
		double rate = v3_len(wb);
		vs->sas_settle = rate < SAS_SETTLED || rate >= vs->sas_settle ? 0 : rate;
		vs->sas_hold = vs->q;
	}

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
		/* per axis: turn toward the target at a rate that shrinks with the
		 * error and that half the available torque can always brake to rest
		 * in time, then drive the spin rate to it hard. The stiff inner loop
		 * is what holds a rocket whose nose lift wants to turn it off the
		 * airflow; the braking cap keeps big turns from overshooting. */
		double I[3] = { Ix, Iy, Iz }, a[3] = { avail.x, avail.y, avail.z };
		double e[3] = { err.x, err.y, err.z }, om[3] = { wb.x, wb.y, wb.z }, out[3];
		for (int k = 0; k < 3; k++) {
			double amax = fmax(a[k], 1e-9) / fmax(I[k], 1e-9); /* rad/s^2 */
			double kp = fmin(SAS_KP, sqrt(amax)); /* 1/s */
			double rate = fmin(kp * fabs(e[k]), sqrt(amax * fabs(e[k])));
			double want = e[k] < 0 ? -rate : rate;
			out[k] = clampd(I[k] * SAS_KR * (want - om[k]), -a[k], a[k]);
		}
		cmd = v3(out[0], out[1], out[2]);
	}
	if (manual) {
		trs_v3 man = v3(vs->input.x * avail.x, vs->input.z * avail.y, vs->input.y * avail.z);
		if (fabs(vs->input.x) > 1e-6) cmd.x = man.x;
		if (fabs(vs->input.z) > 1e-6) cmd.y = man.y;
		if (fabs(vs->input.y) > 1e-6) cmd.z = man.z;
		if (vs->sas == TRS_SAS_STABILITY) {
			vs->sas_hold = vs->q; /* recapture while the pilot steers */
			vs->sas_settle = 1e9;
		}
	}
	(void)Iw;
	double o[3] = { own.x, own.y, own.z }, ra[3] = { rcs_a.x, rcs_a.y, rcs_a.z }, cm[3] = { cmd.x, cmd.y, cmd.z }, u[3];
	double fs[3] = { 0, 0, 0 };
	for (int k = 0; k < 3; k++) {
		double base = clampd(cm[k], -o[k], o[k]);
		u[k] = ra[k] > 0 ? clampd((cm[k] - base) / ra[k], -1, 1) : 0;
		cm[k] = base;
		fs[k] = o[k] > 0 ? base / o[k] : 0; /* fins carry their share of it */
		vs->wheel_load += fabs(fs[k]) / 3; /* the wheels' share of their full effort */
	}
	vs->rcs_rot = v3(u[0], u[1], u[2]);
	if (fin[0] + fin[1] + fin[2] > 0) {
		/* each fin swings its flap by the axes it can help with; what they
		 * really push (cross-talk, saturation and all) replaces their share */
		const trs_body *b = &world->sys.bodies[vs->body];
		trs_v3 vair = v3_sub(vs->v, v3_cross(trs_body_omega(b), vs->r));
		double q = 0.5 * vs->density * v3_len2(vair);
		trs_v3 got = v3(0, 0, 0);
		for (int i = 0; i < c->count; i++) {
			const trs_part_def *d = trs_craft_def(c, i);
			if (!d->plate || d->fin_deflect <= 0)
				continue;
			trs_v3 tq = fin_lever(c, vs, i);
			double tl = v3_len(tq), a[3] = { tq.x, tq.y, tq.z }, ui = 0;
			for (int k = 0; k < 3; k++)
				if (fabs(a[k]) > 1e-9 * tl)
					ui += a[k] > 0 ? fs[k] : -fs[k];
			ui = clampd(ui, -1, 1);
			double sd = ui * sin(d->fin_deflect);
			got = v3_madd(got, tq, q * d->area_side * d->lift_slope * sd);
			/* force along +X wants the trailing edge toward -X into the airflow */
			c->parts[i].fin_flap = -air_from_nose * asin(sd);
		}
		cm[0] += got.x - fs[0] * fin[0];
		cm[1] += got.y - fs[1] * fin[1];
		cm[2] += got.z - fs[2] * fin[2];
	}
	return v3(cm[0], cm[1], cm[2]);
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

/* Canopies open over chute_inflate seconds rather than instantly: the drag
 * area grows linearly toward the state's size (at full_cda / chute_inflate
 * m^2 per second), so the opening shock is spread out the way a real
 * canopy's is. */
static void update_chutes(const trs_world *w, trs_vessel *vs, double t, double h) {
	trs_craft *c = vs->craft;
	const trs_body *b = &w->sys.bodies[vs->body];
	trs_v3 vair = v3_sub(vs->v, v3_cross(trs_body_omega(b), vs->r));
	double sp = v3_len(vair);
	double agl = -1; /* height above the ground below, found only when a chute needs it */
	for (int i = 0; i < c->count; i++) {
		const trs_part_def *d = trs_craft_def(c, i);
		trs_part *p = &c->parts[i];
		if (!d->is_parachute)
			continue;
		if (p->chute == TRS_CHUTE_ARMED && vs->pressure >= d->chute_min_pressure && sp <= d->chute_max_speed)
			p->chute = TRS_CHUTE_SEMI;
		if (p->chute == TRS_CHUTE_SEMI) {
			/* full deployment goes by the ground, not the datum: a capsule
			 * coming down on a high plateau must not hit it half-open */
			if (agl < 0) {
				double g = trs_body_terrain_at(b, t, vs->r);
				agl = vs->altitude - g;
			}
			if (agl <= d->chute_full_alt)
				p->chute = TRS_CHUTE_FULL;
		}
		double target = p->chute == TRS_CHUTE_SEMI ? d->chute_semi_cda : p->chute == TRS_CHUTE_FULL ? d->chute_full_cda : 0;
		double rate = d->chute_inflate > 0 ? d->chute_full_cda / d->chute_inflate : 1e12;
		p->chute_cda = target < p->chute_cda ? target : fmin(target, p->chute_cda + rate * h);
	}
}

/* ---- electricity --------------------------------------------------------------- */

/* Solar power (kW) on a vessel's panels now: each gives solar_kw x the
 * cosine of the sun off its face x (solar_ref_m / distance)^2, nothing
 * while any body stands between the vessel and the star. */
double trs_vessel_solar(const trs_world *w, const trs_vessel *vs, int *sunlit) {
	*sunlit = 0;
	if (!vs->craft)
		return 0;
	trs_v3 br, bv, vr, vv;
	trs_body_state_abs(&w->sys, vs->body, w->t, &br, &bv);
	trs_vessel_state(w, (int)(vs - w->vessels), &vr, &vv); /* in any mode */
	trs_v3 p = v3_add(br, vr); /* from the star */
	double d = v3_len(p);
	if (d <= w->sys.bodies[0].radius)
		return 0;
	trs_v3 to_sun = v3_scale(p, -1.0 / d);
	for (int k = 1; k < w->sys.count; k++) {
		trs_v3 ck, cv;
		trs_body_state_abs(&w->sys, k, w->t, &ck, &cv);
		trs_v3 rel = v3_sub(ck, p);
		double along = v3_dot(rel, to_sun);
		if (along <= 0 || along >= d)
			continue;
		double miss = v3_len(v3_sub(rel, v3_scale(to_sun, along)));
		if (miss < w->sys.bodies[k].radius)
			return 0; /* in its shadow */
	}
	*sunlit = 1;
	trs_quat q = vs->q;
	if (vs->mode == TRS_MODE_LANDED)
		q = quat_normalize(quat_mul(trs_body_quat(&w->sys.bodies[vs->body], w->t), vs->fixed_q));
	trs_v3 sl = quat_rotate(quat_conj(q), to_sun);
	double kw = 0;
	for (int i = 0; i < vs->craft->count; i++) {
		const trs_part_def *df = trs_craft_def(vs->craft, i);
		if (df->solar_kw <= 0)
			continue;
		trs_v3 n = quat_rotate(vs->craft->parts[i].rot, v3(0, 0, 1));
		double r = df->solar_ref_m / d;
		kw += df->solar_kw * fmax(0, v3_dot(n, sl)) * r * r;
	}
	return kw;
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
/* Exhaust hitting a part of the same craft (an engine stacked on top of
 * another, a tank hung under a nozzle): each lit engine's jet carries
 * P = F ve / 2 out of a nozzle at its bottom end, spreading in a cone of
 * PLUME_HALF_ANGLE; a part inside it takes PLUME_ETA of the jet power per
 * unit plume cross-section where the plume first meets it, scaled by how
 * much of the plume it covers. The part is its axis swept by its radius,
 * so a jet running alongside a long tank misses it. W/m^2 into the skin. */
#define PLUME_HALF_ANGLE (12.0 * TRS_PI / 180.0)
#define PLUME_ETA 0.05
#define PLUME_REACH 40.0
#define PLUME_SAMPLES 9

double trs_plume_flux(const trs_craft *c, int e, int i, double mdot, double isp) {
	if (e == i || mdot <= 0)
		return 0;
	const trs_part *pt = &c->parts[i];
	const trs_part_def *dt = trs_craft_def(c, i);
	const trs_part *pe = &c->parts[e];
	const trs_part_def *de = trs_craft_def(c, e);
	trs_v3 down = v3_neg(part_axis(pe));
	trs_v3 nozzle = v3_add(pe->pos, v3_scale(down, de->height * 0.5));
	double r0 = de->radius * 0.8, tn = tan(PLUME_HALF_ANGLE), r = dt->radius;
	/* cheap reject on the part's bounding sphere */
	trs_v3 rel = v3_sub(pt->pos, nozzle);
	double along = v3_dot(rel, down), half = dt->height * 0.5 + r;
	double off = v3_len(v3_sub(rel, v3_scale(down, along)));
	if (along + half <= 0.02 || along - half > PLUME_REACH || off - half >= r0 + fmax(0, along + half) * tn)
		return 0;
	/* the part as its axis swept by its radius, sampled along the axis:
	 * the strongest sample (most of the jet covered, nearest the nozzle) */
	trs_v3 ax = part_axis(pt);
	double best = 0;
	for (int k = 0; k < PLUME_SAMPLES; k++) {
		double y = dt->height * ((k + 0.5) / PLUME_SAMPLES - 0.5);
		trs_v3 rk = v3_add(rel, v3_scale(ax, y));
		double ak = v3_dot(rk, down);
		if (ak <= 0.02)
			continue; /* behind the nozzle */
		double near = fmax(0, ak - r);
		if (near > PLUME_REACH)
			continue;
		double ok = v3_len(v3_sub(rk, v3_scale(down, ak)));
		double rp = r0 + near * tn;
		if (ok >= rp + r)
			continue;
		double cover = fmin(1, (rp + r - ok) / (2 * fmin(rp, r)));
		best = fmax(best, cover / (rp * rp));
	}
	double ve = isp * TRS_G0;
	double power = 0.5 * mdot * ve * ve;
	return PLUME_ETA * power / TRS_PI * best;
}

double trs_plume_soak_temp(const trs_craft *c, int i, double flux) {
	double a4 = TRS_T_AMBIENT * TRS_T_AMBIENT * TRS_T_AMBIENT * TRS_T_AMBIENT;
	return pow(flux / (trs_craft_def(c, i)->emissivity * SIGMA_SB) + a4, 0.25);
}

static double plume_flux(const trs_vessel *vs, const engine_set *es, int i) {
	double q = 0;
	for (int k = 0; k < es->n; k++) {
		const trs_part_def *de = trs_craft_def(vs->craft, es->idx[k]);
		q += trs_plume_flux(vs->craft, es->idx[k], i, es->mdot[k], trs_engine_isp(de, vs->pressure));
	}
	return q;
}

/* How squarely a leading end takes the air head-on: a flat end takes the
 * full stagnation flux, a tapering one (nosecone, capsule shoulder) meets
 * it at a slant and takes sin(wall angle) of it over the area it grows by
 * (the Newtonian/Lees falloff along an inclined wall). Area-weighted. */
double trs_lead_slant(const trs_part_def *d, double cs) {
	double r_lead = cs > 0 ? d->radius_top : d->radius_bottom, r_tail = cs > 0 ? d->radius_bottom : d->radius_top;
	if (r_lead >= r_tail - 1e-9 || d->height <= 0)
		return 1;
	double flat = r_lead * r_lead, grow = r_tail * r_tail - r_lead * r_lead;
	double s = sin(atan2(r_tail - r_lead, d->height));
	return (flat + grow * s) / (flat + grow);
}

static void heat(const trs_world *w, trs_vessel *vs, double h) {
	trs_craft *c = vs->craft;
	const trs_body *b = &w->sys.bodies[vs->body];
	double rho = vs->density;
	trs_v3 vair = v3_sub(vs->v, v3_cross(trs_body_omega(b), vs->r));
	double sp = v3_len(vair);
	trs_v3 dl = sp > 1e-9 ? quat_rotate(quat_conj(vs->q), v3_scale(vair, 1.0 / sp)) : v3(0, 0, 0);
	double t_rec = TRS_T_AMBIENT + sp * sp / (2 * CP_AIR);
	engine_set es;
	gather_engines(vs, vs->pressure, &es);
	for (int i = 0; i < c->count; i++) {
		trs_part *p = &c->parts[i];
		const trs_part_def *d = trs_craft_def(c, i);
		double q = plume_flux(vs, &es, i);
		if (rho > 0 && sp > 1) {
			double cs = v3_dot(dl, part_axis(p));
			double exposed = (cs > 0 ? vs->face_top[i] : vs->face_bottom[i]) / fmax(d->area_axial, 1e-6);
			double wake = cs > 0 ? vs->wake_top[i] : vs->wake_bottom[i];
			double f = fmax(clampd(exposed, 0, 1) * fabs(cs) * trs_lead_slant(d, cs), (0.05 + 0.4 * sqrt(fmax(0, 1 - cs * cs))) * wake);
			double hc = 2 * CP_AIR * SG_K * sqrt(rho / fmax(d->radius, 0.05)) * sp;
			q += f * hc * (t_rec - p->temp);
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
	vs->air_speed = v3_len(v3_sub(vs->v, v3_cross(trs_body_omega(b), vs->r)));
	update_chutes(w, vs, t, h);

	engine_set es;
	gather_engines(vs, vs->pressure, &es);

	/* electricity: the sun on the panels now, and whether there is power
	 * to run the wheels and the probe this tick */
	double base_draw = 0, cap_ec;
	vs->ec_gen = trs_vessel_solar(w, vs, &vs->sunlit);
	for (int i = 0; i < vs->craft->count; i++)
		base_draw += trs_craft_def(vs->craft, i)->ec_draw;
	vs->powered = trs_craft_ec(vs->craft, &cap_ec) > 1e-9 || vs->ec_gen >= base_draw + 1e-12;

	/* attitude once per tick from start-of-tick torques */
	trs_v3 fa, aero_torque;
	aero(w, vs, vs->r, vs->v, &fa, &aero_torque);
	trs_v3 fg, ground_torque;
	contact_report rep;
	ground(w, vs, t, vs->r, vs->v, &fg, &ground_torque, &rep);
	vs->contacts = rep.touching;
	if (rep.touching)
		vs->max_impact = fmax(vs->max_impact, rep.max_speed);
	/* Friction can slow a turn to a stop within a tick but not past it:
	 * held over the whole tick, the friction torque of a craft that turns
	 * easily about some axis (a plane rocking on its wheels) would flip
	 * its spin every step and chatter. */
	if (rep.touching) {
		trs_v3 wl = quat_rotate(quat_conj(vs->q), v3_sub(vs->w, trs_body_omega(b)));
		trs_m3 ii = m3_inverse(&vs->inertia);
		trs_v3 dw = v3_scale(m3_mulv(&ii, rep.friction_torque), h);
		double wl2 = v3_len2(wl), along = v3_dot(dw, wl);
		if (wl2 > 1e-18 && -along > wl2) {
			double k = wl2 / -along;
			ground_torque = v3_sub(ground_torque, v3_scale(rep.friction_torque, 1 - k));
		}
	}
	trs_v3 torque = v3_add(v3_add(control_torque(w, vs, es.gimbal_authority), aero_torque), es.torque_local);
	double wheel_kw = 0;
	if (vs->powered)
		for (int i = 0; i < vs->craft->count; i++) {
			const trs_part_def *d = trs_craft_def(vs->craft, i);
			wheel_kw += d->torque_kw_per_knm * d->torque / 1000.0;
		}
	vs->ec_use = (vs->powered ? base_draw : 0) + wheel_kw * vs->wheel_load;
	trs_craft_ec_add(vs->craft, (vs->ec_gen - vs->ec_use) * h);
	torque = v3_add(v3_add(torque, ground_torque), vs->ext_torque);
	/* RCS: rotation share from the controller plus the pilot's translation,
	 * cut back evenly if the monopropellant would run out this tick */
	trs_v3 rcs_f = v3(0, 0, 0), rcs_t = v3(0, 0, 0);
	double rcs_md = 0;
	if (rcs_armed(vs)) {
		rcs_mix(vs, vs->rcs_rot, vs->translate, vs->pressure, 1, &rcs_f, &rcs_t, &rcs_md);
		double mono = trs_craft_mono(vs->craft, NULL);
		if (rcs_md * h > mono) {
			double k = mono / (rcs_md * h);
			rcs_f = v3_scale(rcs_f, k), rcs_t = v3_scale(rcs_t, k), rcs_md *= k;
			for (int i = 0; i < vs->craft->count; i++)
				for (int n = 0; n < TRS_MAX_NOZZLES; n++)
					vs->craft->parts[i].rcs_fire[n] *= k;
		}
		torque = v3_add(torque, rcs_t);
	} else {
		for (int i = 0; i < vs->craft->count; i++)
			memset(vs->craft->parts[i].rcs_fire, 0, sizeof vs->craft->parts[i].rcs_fire);
	}
	vs->rcs_force = rcs_f;
	vs->rcs_mdot = rcs_md;
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
		dyn_ctx cx = { w, vs, v3_add(quat_rotate(vs->q, v3_add(es.force_local, rcs_f)), vs->ext_force), vs->mass,
			es.total_mdot + rcs_md, t + done };
		trs_v3 s;
		rk4_mass(&cx, vs, t + done, sub, &s);
		if (rcs_md > 0) {
			double mt = es.total_mdot + rcs_md; /* both flows drain the mass */
			vs->rcs_dv_expended += v3_len(rcs_f) / mt * log(vs->mass / (vs->mass - mt * sub));
			trs_craft_mono_draw(vs->craft, rcs_md * sub);
			if (!es.n)
				recentre(vs);
		}
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

	/* a penguin jumps off the ground (the planet takes the other end) */
	if (vs->jump && rep.touching && trs_vessel_is_eva(vs))
		vs->v = v3_madd(vs->v, v3_norm(vs->r), trs_craft_def(vs->craft, 0)->eva_jump);
	vs->jump = 0;
	/* landed: touching and moving with the surface */
	if (rep.touching) {
		trs_v3 vs_rel = v3_sub(vs->v, v3_cross(trs_body_omega(b), vs->r));
		int was = vs->landed;
		vs->landed = v3_len(vs_rel) < 0.3 || trs_vessel_is_eva(vs); /* a penguin on its feet, walking or not */
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
	int s = vs->next_stage--;
	trs_craft *c = vs->craft;
	/* the launch clamps let go on the first stage that lights an engine; a
	 * stage that only decouples leaves the rest held on the pad */
	int lights = 0;
	for (int i = 0; i < c->count; i++)
		lights |= c->parts[i].stage == s && trs_craft_def(c, i)->propellant != TRS_PROP_NONE;
	int keep_clamped = vs->clamped && !lights;
	int pieces[256];
	unsigned char radial_piece[256];
	if (vs->clamped && lights) {
		vs->clamped = 0;
		vs->lifted = 1;
		vs->liftoff_t = w->t;
		vs->sas_hold = vs->q;
	}
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
		vs->fresh = 1;
		trs_vessel *nv = &w->vessels[nid];
		nv->lifted = vs->lifted, nv->liftoff_t = vs->liftoff_t;
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
		/* boosters dropped sideways tumble a little in the core's flow:
		 * give them a moment to swing clear before they can touch it */
		if (dd->decoupler == TRS_DECOUPLER_RADIAL) {
			trs_vessel_add_ghost(&w->vessels[id], nid, w->t + RADIAL_CLEAR_S);
			trs_vessel_add_ghost(&w->vessels[nid], id, w->t + RADIAL_CLEAR_S);
		}
		/* boosters dropped by one press tumble in the same airflow, and a
		 * pair of them may swing across the core at once: they pass
		 * through each other as they do the core */
		if (dd->decoupler == TRS_DECOUPLER_RADIAL)
			for (int k = 0; k < (made < 256 ? made : 256); k++)
				if (w->vessels[pieces[k]].alive && radial_piece[k]) {
					trs_vessel_add_ghost(&w->vessels[pieces[k]], nid, w->t + RADIAL_CLEAR_S);
					trs_vessel_add_ghost(&w->vessels[nid], pieces[k], w->t + RADIAL_CLEAR_S);
				}
		if (made < 256)
			radial_piece[made] = dd->decoupler == TRS_DECOUPLER_RADIAL;
		if (made < max && new_ids)
			new_ids[made] = nid;
		if (made < 256)
			pieces[made] = nid;
		made++;
	}
	if (keep_clamped) {
		/* the piece standing lowest on the pad is the one the clamps hold */
		int best = id;
		double low = 1e30;
		for (int k = -1; k < (made < 256 ? made : 256); k++) {
			int vid = k < 0 ? id : pieces[k];
			trs_vessel *pv = &w->vessels[vid];
			if (pv->dirty)
				trs_vessel_refresh(w, vid);
			trs_v3 up = v3_norm(pv->r);
			for (int q = 0; q < pv->ncontact; q++) {
				double hgt = v3_dot(world_point(pv, pv->contact_pt[q]), up);
				if (hgt < low) {
					low = hgt;
					best = vid;
				}
			}
		}
		trs_vessel *held = &w->vessels[best];
		held->v = v3_cross(trs_body_omega(&w->sys.bodies[held->body]), held->r);
		held->w = trs_body_omega(&w->sys.bodies[held->body]);
		trs_vessel_set_mode(w, best, TRS_MODE_LANDED);
		held->clamped = 1;
	}
	return made < max ? made : max;
}

int trs_world_action_group(trs_world *w, int id, int g) {
	trs_vessel *vs = &w->vessels[id];
	if (!vs->craft || g < 1 || g > 9)
		return 0;
	int bit = 1 << (g - 1), n = 0, brake = 0, port = -1;
	trs_craft *c = vs->craft;
	for (int i = 0; i < c->count; i++) {
		trs_part *p = &c->parts[i];
		const trs_part_def *d = trs_craft_def(c, i);
		if (!(p->groups & bit))
			continue;
		if (d->propellant != TRS_PROP_NONE && d->propellant != TRS_PROP_SOLID) {
			p->engine_on = !p->engine_on; /* a solid, once lit, can't be put out */
			n++;
		} else if (d->propellant == TRS_PROP_SOLID && !p->engine_on) {
			p->engine_on = 1;
			n++;
		}
		if (d->is_parachute && p->chute == TRS_CHUTE_STOWED) {
			p->chute = TRS_CHUTE_ARMED;
			n++;
		}
		if (d->is_leg) {
			p->legs_deployed = !p->legs_deployed;
			vs->dirty = 1; /* the feet move */
			n++;
		}
		if (d->is_wheel)
			brake = 1;
		if (d->is_dock && port < 0 && trs_craft_def(c, i)->is_dock && (p->parent >= 0 && trs_craft_def(c, p->parent)->is_dock))
			port = i; /* the joint between two ports: this one sits on the other */
	}
	if (brake) {
		vs->brakes = !vs->brakes;
		n++;
	}
	if (vs->mode == TRS_MODE_LANDED && n > 0 && !vs->clamped)
		trs_vessel_set_mode(w, id, TRS_MODE_PHYSICS);
	if (port >= 0 && trs_world_undock(w, id, port) >= 0)
		n++;
	return n;
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
	w->vessels[parent_id].fresh = 1;
	if (nid >= 0)
		w->vessels[nid].lifted = w->vessels[parent_id].lifted, w->vessels[nid].liftoff_t = w->vessels[parent_id].liftoff_t;
	if (nid < 0) {
		/* no room for another vessel: the piece is lost, and says so */
		for (int k = 0; k < piece->count; k++) {
			trs_world_event ev = { TRS_WEV_PART_DESTROYED, w->t, parent_id, -1, piece->parts[k].uid, "", TRS_DESTROY_BLAST };
			snprintf(ev.part, sizeof ev.part, "%s", trs_craft_def(piece, k)->id);
			trs_world_push_event(w, ev);
		}
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
		double fuel = ((d->fuel_liquid > 0 || d->fuel_solid > 0) ? c->parts[part].fuel : 0) + c->parts[part].mono;
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

/* The runway: RUNWAY_EAST m east of the pad along the flat shelf. A craft
 * with wheels lies nose east on them (the side its wheels stand out on
 * turned to the ground), brakes on, free to roll once they're off. */
int trs_world_launch_runway(trs_world *w, const trs_craft *c, int body, double lat, double lon) {
	const trs_body *b = &w->sys.bodies[body];
	trs_v3 site = v3(cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat));
	trs_v3 east0 = v3_norm(v3_cross(v3(0, 0, 1), site));
	double ang = RUNWAY_EAST / b->radius;
	trs_v3 up = v3_norm(v3_add(v3_scale(site, cos(ang)), v3_scale(east0, sin(ang))));
	trs_v3 east = v3_norm(v3_cross(v3(0, 0, 1), up));
	int id = trs_world_add_craft(w, c, body, v3_scale(up, b->radius * 2), v3(0, 0, 0), quat_identity());
	if (id < 0)
		return id;
	trs_vessel *vs = &w->vessels[id];
	/* which way the wheels stand out from the craft's axis */
	trs_v3 dn = v3(0, 0, 0);
	for (int k = 0; k < vs->ncontact; k++)
		if (trs_craft_def(vs->craft, vs->contact_part[k])->is_wheel) {
			trs_v3 a = v3_sub(vs->contact_pt[k], vs->cm);
			dn = v3_add(dn, v3(a.x, 0, a.z));
		}
	if (v3_len(dn) < 1e-6)
		dn = v3(0, 0, -1);
	dn = v3_norm(dn);
	/* local frame (x, y = nose, z) -> world: y to east, dn to -up */
	trs_v3 lx = v3_cross(v3(0, 1, 0), dn); /* completes (lx, y, dn) right-handed */
	trs_v3 wx = v3_cross(east, v3_neg(up));
	trs_quat q_local = quat_from_basis(lx, v3(0, 1, 0), dn);
	trs_quat q_world = quat_from_basis(wx, east, v3_neg(up));
	trs_quat q = quat_normalize(quat_mul(q_world, quat_conj(q_local)));
	double lowest = INFINITY;
	for (int k = 0; k < vs->ncontact; k++)
		lowest = fmin(lowest, v3_dot(quat_rotate(q, v3_sub(vs->contact_pt[k], vs->cm)), up));
	double h = b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, up) : 0;
	trs_v3 fixed_r = v3_scale(up, b->radius + h - lowest + 0.02);
	vs->r = trs_body_from_fixed(b, w->t, fixed_r);
	vs->v = v3_cross(trs_body_omega(b), vs->r);
	vs->q = quat_normalize(quat_mul(trs_body_quat(b, w->t), q));
	vs->w = trs_body_omega(b);
	vs->sas_hold = vs->q;
	vs->brakes = 1;
	vs->landed = 1;
	vs->lifted = 1; /* no clamps: the mission clock runs from here */
	vs->liftoff_t = w->t;
	return id;
}
