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

void trs_vessel_refresh(trs_world *w, int id) {
	(void)w;
	trs_vessel *vs = &w->vessels[id];
	trs_craft *c = vs->craft;
	if (!c)
		return;
	free(vs->fuel_group);
	free(vs->face_top);
	free(vs->face_bottom);
	int n = c->count ? c->count : 1;
	vs->fuel_group = malloc(sizeof(int) * (size_t)n);
	vs->face_top = malloc(sizeof(double) * (size_t)n);
	vs->face_bottom = malloc(sizeof(double) * (size_t)n);
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
	}
	trs_craft_mass_props(c, &vs->mass, &vs->cm, &vs->inertia);
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

/* Total aerodynamic force (SOI frame) and torque about the CoM (vessel-local). */
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
	trs_v3 vair = v3_sub(v, v3_cross(trs_body_omega(b), r));
	double sp = v3_len(vair);
	if (sp < 1e-9)
		return;
	trs_v3 dir_world = v3_scale(vair, 1.0 / sp);
	trs_v3 d_local = quat_rotate(quat_conj(vs->q), dir_world);
	double qdyn = 0.5 * rho * sp * sp;
	const trs_craft *c = vs->craft;
	trs_v3 f_local_total = v3(0, 0, 0);
	for (int i = 0; i < c->count; i++) {
		const trs_part *pt = &c->parts[i];
		const trs_part_def *d = trs_craft_def(c, i);
		double cs = v3_dot(d_local, part_axis(pt));
		double face = cs > 0 ? d->cd_top * vs->face_top[i] : d->cd_bottom * vs->face_bottom[i];
		double cda = cs * cs * face + (1 - cs * cs) * d->cd_side * d->area_side;
		if (d->is_parachute) {
			if (pt->chute == TRS_CHUTE_SEMI)
				cda += d->chute_semi_cda;
			else if (pt->chute == TRS_CHUTE_FULL)
				cda += d->chute_full_cda;
		}
		trs_v3 f = v3_scale(d_local, -qdyn * cda);
		f_local_total = v3_add(f_local_total, f);
		if (torque_local)
			*torque_local = v3_add(*torque_local, v3_cross(v3_sub(pt->pos, vs->cm), f));
	}
	*force = quat_rotate(vs->q, f_local_total);
}

/* ---- attitude ------------------------------------------------------------------ */

static double clampd(double x, double lo, double hi) {
	return x < lo ? lo : x > hi ? hi : x;
}

static trs_v3 control_torque(trs_vessel *vs, double gimbal_authority) {
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

	if (vs->sas == TRS_SAS_TARGET || (vs->sas == TRS_SAS_STABILITY && !manual)) {
		trs_v3 err; /* rotation vector from current to target, local frame */
		if (vs->sas == TRS_SAS_TARGET && v3_len2(vs->sas_target) > 0) {
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
	trs_v3 fa;
	aero(cx->w, cx->vs, r, v, &fa, NULL);
	trs_v3 ng = v3_scale(v3_add(cx->thrust_world, fa), 1.0 / m);
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
	trs_v3 torque = v3_add(v3_add(control_torque(vs, es.gimbal_authority), aero_torque), es.torque_local);
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
	if (vs->mode == TRS_MODE_RAILS)
		trs_vessel_set_mode(w, id, TRS_MODE_PHYSICS);
	int s = vs->next_stage--;
	trs_craft *c = vs->craft;
	for (int i = 0; i < c->count; i++) {
		if (c->parts[i].stage != s)
			continue;
		const trs_part_def *d = trs_craft_def(c, i);
		if (d->propellant != TRS_PROP_NONE)
			c->parts[i].engine_on = 1;
		if (d->is_parachute && c->parts[i].chute == TRS_CHUTE_STOWED)
			c->parts[i].chute = TRS_CHUTE_ARMED;
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
		(void)world_point;
		if (made < max && new_ids)
			new_ids[made] = nid;
		made++;
	}
	return made < max ? made : max;
}
