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

/* M2 gates: delta-v readouts vs. flown burns, and momentum across staging. */
#include "trs/craft.h"
#include "trs/gate.h"
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *STOCK_CRAFT[] = { "hopper", "orbiter", "krill_lander" };
#define NSTOCK 3

int trs_gate_load_catalog(trs_log *log, const char *data_dir, trs_part_catalog *cat) {
	char path[1024], err[256];
	snprintf(path, sizeof path, "%s/parts/stock_parts.json", data_dir);
	if (!trs_catalog_load(cat, path, err, sizeof err)) {
		trs_log_printf(log, "cannot load parts: %s\n", err);
		return 0;
	}
	return 1;
}

int trs_gate_load_craft(trs_log *log, const char *data_dir, const char *name, const trs_part_catalog *cat,
		trs_craft *c) {
	char path[1024], err[256];
	snprintf(path, sizeof path, "%s/craft/%s.craft.json", data_dir, name);
	trs_json *j = trs_json_load_file(path, err, sizeof err);
	if (!j) {
		trs_log_printf(log, "cannot load %s: %s\n", path, err);
		return 0;
	}
	int ok = trs_craft_from_json(c, cat, j, err, sizeof err);
	trs_json_free(j);
	if (!ok)
		trs_log_printf(log, "bad craft %s: %s\n", name, err);
	return ok;
}

static int load_floe_system(trs_log *log, const char *data_dir, trs_system *sys) {
	char path[1024], err[256];
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_system_load(sys, path, err, sizeof err)) {
		trs_log_printf(log, "cannot load system: %s\n", err);
		return 0;
	}
	return 1;
}

/* Deep space: one negligible-mass body, so measured |dv| is pure thrust. */
static const char *VOID_SYSTEM = "{\"name\":\"void\",\"bodies\":[{\"id\":\"void\",\"mu\":1e-6,\"radius\":1}]}";

/* Flight-side version of the readout's staging rule, on the live vessel. */
static int stage_burn_done(const trs_vessel *vs) {
	const trs_craft *c = vs->craft;
	int s1 = vs->next_stage; /* the stage that would fire next */
	unsigned char *drop = calloc((size_t)c->count, 1), *tmp = calloc((size_t)c->count, 1);
	for (int i = 0; i < c->count; i++)
		if (c->parts[i].stage == s1 && trs_craft_def(c, i)->decoupler) {
			trs_craft_detach_set(c, i, tmp);
			for (int k = 0; k < c->count; k++)
				drop[k] |= tmp[k];
		}
	int lit = 0, drop_engines = 0, drop_lit = 0;
	for (int i = 0; i < c->count; i++) {
		const trs_part_def *d = trs_craft_def(c, i);
		if (!c->parts[i].engine_on || d->propellant == TRS_PROP_NONE)
			continue;
		int fed = trs_craft_engine_fed(c, vs->fuel_group, i);
		if (drop[i]) {
			drop_engines = 1;
			drop_lit |= fed;
		}
		lit |= fed;
	}
	free(drop);
	free(tmp);
	return !lit || (drop_engines && !drop_lit);
}

trs_gate_status trs_gate_deltav(trs_log *log, const char *data_dir) {
	trs_part_catalog cat;
	trs_system floe_sys, void_sys;
	char err[256];
	if (!trs_gate_load_catalog(log, data_dir, &cat) || !load_floe_system(log, data_dir, &floe_sys))
		return TRS_GATE_FAIL;
	if (!trs_system_load_json(&void_sys, VOID_SYSTEM, err, sizeof err)) {
		trs_log_printf(log, "void system: %s\n", err);
		return TRS_GATE_FAIL;
	}
	int floe = trs_system_find(&floe_sys, "floe");
	double g_floe = floe_sys.bodies[floe].mu / pow(floe_sys.bodies[floe].radius, 2);
	int failed = 0;

	for (int k = 0; k < NSTOCK; k++) {
		trs_craft c;
		if (!trs_gate_load_craft(log, data_dir, STOCK_CRAFT[k], &cat, &c)) {
			failed++;
			continue;
		}
		trs_stage_stats st[32];
		int ns = trs_craft_stage_stats(&c, trs_craft_max_stage(&c), g_floe, TRS_P_REF, st, 32);
		double total_read = 0;
		for (int i = 0; i < ns; i++)
			total_read += st[i].dv_vac;

		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &void_sys, 0);
		int id = trs_world_add_craft(w, &c, 0, v3(1e9, 0, 0), v3(0, 0, 0), quat_identity());
		w->vessels[id].throttle = 1.0;
		w->vessels[id].sas = TRS_SAS_STABILITY;
		double total_meas = 0, worst = 0;
		int ticks = 0;
		trs_log_printf(log, "%s (%d parts, %.0f kg)\n", c.name, c.count, trs_craft_mass(&c));
		for (int i = 0; i < ns; i++) {
			int new_ids[16];
			int made = trs_world_stage(w, id, new_ids, 16);
			for (int m = 0; m < made; m++) /* debris is not part of the measurement */
				trs_world_remove_vessel(w, new_ids[m]);
			trs_v3 v0 = w->vessels[id].v;
			while (!stage_burn_done(&w->vessels[id]) && ticks < 50 * 3600) {
				trs_world_step_physics(w);
				ticks++;
			}
			double meas = v3_len(v3_sub(w->vessels[id].v, v0));
			total_meas += meas;
			double e = st[i].dv_vac > 1 ? fabs(meas - st[i].dv_vac) / st[i].dv_vac : fabs(meas - st[i].dv_vac);
			int ok = st[i].dv_vac > 1 ? e < 0.005 : meas < 0.05;
			if (st[i].dv_vac > 1 && e > worst)
				worst = e;
			failed += !ok;
			if (st[i].dv_vac > 1 || meas > 0.05)
				trs_log_printf(log, "  %s stage %d: readout %8.2f m/s  flown %8.2f m/s  err %.4f%%  (TWR %.2f, %.1f s)\n",
						ok ? "ok  " : "FAIL", st[i].stage, st[i].dv_vac, meas, 100 * e, st[i].twr_sl, st[i].burn_time);
		}
		double te = fabs(total_meas - total_read) / total_read;
		failed += te >= 0.005;
		trs_log_printf(log, "  %s total:   readout %8.2f m/s  flown %8.2f m/s  err %.4f%% (worst stage %.4f%%)\n",
				te < 0.005 ? "ok  " : "FAIL", total_read, total_meas, 100 * te, 100 * worst);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	trs_log_printf(log, "tolerance 0.5%% per stage; vacuum, full throttle, SAS hold\n");
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}

static void totals(const trs_world *w, trs_v3 *P, trs_v3 *L, double *m, int *parts, int *vessels) {
	*P = v3(0, 0, 0);
	*L = v3(0, 0, 0);
	*m = 0;
	*parts = 0;
	*vessels = 0;
	for (int i = 0; i < w->nvessels; i++) {
		if (!w->vessels[i].alive)
			continue;
		trs_v3 p, l;
		trs_vessel_momentum(w, i, &p, &l);
		*P = v3_add(*P, p);
		*L = v3_add(*L, l);
		*m += w->vessels[i].mass;
		*parts += w->vessels[i].craft->count;
		(*vessels)++;
	}
}

trs_gate_status trs_gate_staging(trs_log *log, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	if (!trs_gate_load_catalog(log, data_dir, &cat) || !load_floe_system(log, data_dir, &sys))
		return TRS_GATE_FAIL;
	int floe = trs_system_find(&sys, "floe");
	double R = sys.bodies[floe].radius + 100e3, vc = sqrt(sys.bodies[floe].mu / R);
	int failed = 0;
	for (int k = 0; k < NSTOCK; k++) {
		trs_craft c;
		if (!trs_gate_load_craft(log, data_dir, STOCK_CRAFT[k], &cat, &c)) {
			failed++;
			continue;
		}
		for (int spin = 0; spin < 2; spin++) {
			trs_world *w = malloc(sizeof(trs_world));
			trs_world_init(w, &sys, 0);
			trs_quat q = quat_axis_angle(v3(0.3, 1, 0.2), 0.7);
			int id = trs_world_add_craft(w, &c, floe, v3(R, 0, 0), v3(0, vc, 0), q);
			if (spin)
				w->vessels[id].w = v3(0.21, -0.08, 0.35);
			trs_v3 P0, L0, P, L;
			double m0, m;
			int parts0, parts = 0, nv0, nv = 0;
			totals(w, &P0, &L0, &m0, &parts0, &nv0);
			double worst_p = 0, worst_l = 0, worst_sep = INFINITY;
			int splits = 0;
			while (w->vessels[id].next_stage >= 0) {
				trs_v3 vA0 = w->vessels[id].v;
				int new_ids[16];
				int made = trs_world_stage(w, id, new_ids, 16);
				splits += made;
				totals(w, &P, &L, &m, &parts, &nv);
				worst_p = fmax(worst_p, v3_len(v3_sub(P, P0)) / v3_len(P0));
				worst_l = fmax(worst_l, v3_len(v3_sub(L, L0)) / v3_len(L0));
				/* the halves must actually move apart */
				for (int n = 0; n < made; n++) {
					trs_v3 rel_v = v3_sub(w->vessels[new_ids[n]].v, w->vessels[id].v);
					trs_v3 rel_r = v3_sub(w->vessels[new_ids[n]].r, w->vessels[id].r);
					worst_sep = fmin(worst_sep, v3_dot(rel_v, v3_norm(rel_r)) - (spin ? 1e9 : 0));
				}
				(void)vA0;
				if (fabs(m - m0) > 1e-9 * m0 || parts != parts0)
					failed++, trs_log_printf(log, "FAIL mass/part count not conserved\n");
			}
			int ok = worst_p < 1e-12 && worst_l < 1e-9 && (spin || worst_sep > 0) && splits > 0;
			failed += !ok;
			trs_log_printf(log, "%s %-13s %s: %d splits -> %d vessels, |dP|/P=%.1e |dL|/L=%.1e%s\n", ok ? "ok  " : "FAIL",
					c.name, spin ? "spinning" : "steady  ", splits, nv, worst_p, worst_l,
					spin ? "" : (worst_sep > 0 ? ", halves separate" : ", halves DO NOT separate"));
			trs_world_free(w);
			free(w);
		}
		trs_craft_free(&c);
	}
	trs_log_printf(log, "linear momentum tol 1e-12, angular (about Floe) 1e-9, mass and part count exact\n");
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}

/* Places a craft upright at an altitude above the terrain at Floe's
 * equator (lon 0), co-rotating. */
static int drop_at(trs_world *w, const trs_craft *c, int floe, double alt, double vdown) {
	const trs_body *b = &w->sys.bodies[floe];
	double ground = trs_body_terrain_at(b, w->t, v3(b->radius, 0, 0));
	trs_v3 r = v3(b->radius + ground + alt, 0, 0);
	trs_v3 v = v3_add(v3_cross(trs_body_omega(b), r), v3(-vdown, 0, 0));
	/* local +Y (nose) along radial-out */
	trs_quat q = quat_from_to(v3(0, 1, 0), v3(1, 0, 0));
	return trs_world_add_craft(w, c, floe, r, v, q);
}

trs_gate_status trs_gate_aero(trs_log *log, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	if (!trs_gate_load_catalog(log, data_dir, &cat) || !load_floe_system(log, data_dir, &sys))
		return TRS_GATE_FAIL;
	int floe = trs_system_find(&sys, "floe");
	const trs_body *F = &sys.bodies[floe];
	int failed = 0;

	/* 1. Pod under a full parachute settles to the analytic terminal speed. */
	{
		trs_craft c;
		trs_craft_init(&c, &cat);
		int pod = trs_craft_add(&c, trs_catalog_find(&cat, "pod_1"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
		trs_craft_add(&c, trs_catalog_find(&cat, "parachute"), pod, "bottom", "top", v3(0, 0.675, 0), quat_identity(),
				0, 0);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = drop_at(w, &c, floe, 3000, 150);
		trs_vessel_deploy_chutes(w, id);
		trs_vessel *vs = &w->vessels[id];
		double ground = trs_body_terrain_at(F, w->t, vs->r);
		for (int s = 0; s < 50 * 3600 && (vs->altitude - ground > 300 || vs->altitude == 0); s++) {
			trs_world_step_physics(w);
			ground = trs_body_terrain_at(F, w->t, vs->r);
		}
		/* analytic: m g = 1/2 rho v^2 CdA, CdA from the same part data */
		const trs_part_def *pd = &cat.defs[trs_catalog_find(&cat, "pod_1")];
		const trs_part_def *cd = &cat.defs[trs_catalog_find(&cat, "parachute")];
		double cda = pd->cd_bottom * pd->area_axial + cd->chute_full_cda +
				cd->cd_bottom * fmax(0, cd->area_axial - pd->area_axial);
		double r = v3_len(vs->r), g = F->mu / (r * r);
		trs_v3 vair = v3_sub(vs->v, v3_cross(trs_body_omega(F), vs->r));
		double vt = sqrt(2 * vs->mass * g / (vs->density * cda));
		double sp = v3_len(vair), e = fabs(sp - vt) / vt;
		int ok = e < 0.01 && vs->craft->parts[1].chute == TRS_CHUTE_FULL;
		failed += !ok;
		trs_log_printf(log, "%s parachute descent at %.0f m above terrain: %.3f m/s vs analytic %.3f m/s (err %.3f%%)\n",
				ok ? "ok  " : "FAIL", vs->altitude - ground, sp, vt, 100 * e);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	/* 2. Bare pod: in-flight aero acceleration equals the analytic force from
	 *    the part data, F = -q (Cd A cos|cos| along the axis + Cd_side A_side
	 *    sin^2 across it), checked directly every tick of a 15 s fall (the pod
	 *    is never at steady terminal speed). */
	{
		trs_craft c;
		trs_craft_init(&c, &cat);
		trs_craft_add(&c, trs_catalog_find(&cat, "pod_1"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
		const trs_part_def *pd = &cat.defs[trs_catalog_find(&cat, "pod_1")];
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = drop_at(w, &c, floe, 4000, 60);
		trs_vessel *vs = &w->vessels[id];
		double worst = 0, worst_drag = 0;
		for (int s = 0; s < 50 * 15; s++) {
			trs_v3 r0 = vs->r, v0 = vs->v; /* sensed_accel is evaluated at tick start */
			trs_quat q0 = vs->q;
			trs_world_step_physics(w);
			trs_v3 vair = v3_sub(v0, v3_cross(trs_body_omega(F), r0));
			double p, rho;
			trs_atmosphere_at(F, v3_len(r0) - F->radius, &p, &rho);
			double qd = 0.5 * rho * v3_len2(vair);
			trs_v3 dir = v3_norm(vair), ax = quat_rotate(q0, v3(0, 1, 0));
			double cs = v3_dot(dir, ax);
			trs_v3 perp = v3_sub(dir, v3_scale(ax, cs));
			double face = cs > 0 ? pd->cd_top * pd->area_axial : pd->cd_bottom * pd->area_axial;
			trs_v3 want = v3_scale(v3_add(v3_scale(ax, -qd * face * cs * fabs(cs)),
					v3_scale(perp, -qd * pd->cd_side * pd->area_side * v3_len(perp))), 1.0 / vs->mass);
			worst = fmax(worst, v3_len(v3_sub(vs->sensed_accel, want)) / v3_len(want));
			/* and it is (nearly) pure drag: the pod falls almost along its axis */
			worst_drag = fmax(worst_drag, 1 + v3_dot(v3_norm(vs->sensed_accel), dir));
		}
		int ok = worst < 1e-6 && worst_drag < 1e-5;
		failed += !ok;
		trs_log_printf(log, "%s bare pod aero over 15 s fall: |a - analytic| rel %.1e, off-drag %.1e (now %.1f m/s at %.0f m)\n",
				ok ? "ok  " : "FAIL", worst, worst_drag, v3_len(vs->v), vs->altitude);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	/* 3. On the pad, sensed thrust matches the readout's sea-level TWR. */
	for (int k = 0; k < NSTOCK; k++) {
		trs_craft c;
		if (!trs_gate_load_craft(log, data_dir, STOCK_CRAFT[k], &cat, &c)) {
			failed++;
			continue;
		}
		double g0 = F->mu / (F->radius * F->radius);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = drop_at(w, &c, floe, 50.0, 0); /* clear of the ground: thrust only */
		trs_vessel *vs = &w->vessels[id];
		/* readout at the pressure the engines actually see there */
		double p_here, rho_here;
		trs_atmosphere_at(F, v3_len(vs->r) - F->radius, &p_here, &rho_here);
		trs_stage_stats st[16];
		trs_craft_stage_stats(&c, trs_craft_max_stage(&c), g0, p_here, st, 16);
		vs->throttle = 1;
		trs_world_stage(w, id, NULL, 0);
		trs_world_step_physics(w);
		/* thrust-only part of the sensed acceleration, first tick (speed ~0 so no drag) */
		double twr = v3_len(vs->sensed_accel) / g0;
		(void)twr;
		double thrust_sim = v3_len(vs->sensed_accel) * vs->mass;
		double e = fabs(thrust_sim - st[0].thrust_sl) / st[0].thrust_sl;
		int ok = e < 0.005;
		failed += !ok;
		trs_log_printf(log, "%s %-13s thrust at %.0f Pa: %.1f kN vs readout %.1f kN (TWR %.3f), err %.3f%%\n",
				ok ? "ok  " : "FAIL", c.name, p_here, thrust_sim / 1e3, st[0].thrust_sl / 1e3, st[0].twr_sl, 100 * e);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	/* 4. Weathervaning: a lone nosecone and a lone pod, released tumbling at
	 *    90 deg to the airflow, settle point-first and heat-shield-first. */
	{
		const char *ids[2] = { "nosecone", "pod_1" };
		const double want_cs[2] = { 1, -1 }; /* nose along the flow / against */
		for (int k = 0; k < 2; k++) {
			trs_craft c;
			trs_craft_init(&c, &cat);
			trs_craft_add(&c, trs_catalog_find(&cat, ids[k]), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
			trs_world *w = malloc(sizeof(trs_world));
			trs_world_init(w, &sys, 0);
			int id = drop_at(w, &c, floe, 20000, 80);
			trs_vessel *vs = &w->vessels[id];
			vs->sas = TRS_SAS_OFF; /* free fall, no reaction wheel help */
			/* sideways, with a little spin */
			vs->q = quat_from_to(v3(0, 1, 0), v3(0, 1, 0));
			vs->w = v3_add(trs_body_omega(F), v3(0.3, 0, 0.5));
			for (int s = 0; s < 50 * 40; s++)
				trs_world_step_physics(w);
			trs_v3 vair = v3_sub(vs->v, v3_cross(trs_body_omega(F), vs->r));
			double cs = v3_dot(v3_norm(vair), quat_rotate(vs->q, v3(0, 1, 0)));
			double ang = acos(fmax(-1, fmin(1, cs * want_cs[k]))) * 180 / TRS_PI;
			/* pitch/yaw rate (roll about the axis may persist; it is barely damped) */
			trs_v3 wr = v3_sub(vs->w, trs_body_omega(F)), axw = quat_rotate(vs->q, v3(0, 1, 0));
			double spin = v3_len(v3_sub(wr, v3_scale(axw, v3_dot(wr, axw))));
			int ok = ang < 10 && spin < 0.5; /* settled: only a small residual nod */
			failed += !ok;
			trs_log_printf(log, "%s %-9s released sideways: after 40 s %.1f deg off %s-first, pitch rate %.3f rad/s\n",
					ok ? "ok  " : "FAIL", ids[k], ang, k == 0 ? "point" : "shield", spin);
			trs_world_free(w);
			free(w);
			trs_craft_free(&c);
		}
	}
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}

static double agl(const trs_world *w, int id) {
	trs_v3 r, v;
	trs_vessel_state(w, id, &r, &v);
	const trs_body *b = &w->sys.bodies[w->vessels[id].body];
	return v3_len(r) - b->radius - trs_body_terrain_at(b, w->t, r);
}

static double surface_speed(const trs_world *w, int id) {
	trs_v3 r, v;
	trs_vessel_state(w, id, &r, &v);
	const trs_body *b = &w->sys.bodies[w->vessels[id].body];
	return v3_len(v3_sub(v, v3_cross(trs_body_omega(b), r)));
}

static int count_events(const trs_world *w, trs_world_event_kind k) {
	int n = 0;
	for (int i = 0; i < w->nevents; i++)
		n += w->events[i].kind == k;
	return n;
}

/* The lander's top half (above the transfer-stage decoupler), legs out. */
static int lander_only(trs_log *log, const char *data_dir, const trs_part_catalog *cat, trs_craft *c) {
	if (!trs_gate_load_craft(log, data_dir, "krill_lander", cat, c))
		return 0;
	for (int i = 0; i < c->count; i++)
		if (!strcmp(trs_craft_def(c, i)->id, "engine_small")) {
			for (int k = i + 1; k < c->count; k++)
				if (c->parts[k].parent == i && trs_craft_def(c, k)->decoupler) {
					trs_craft pieces[8];
					int n = trs_craft_remove_part(c, k, pieces, 8);
					for (int p = 0; p < n; p++)
						trs_craft_free(&pieces[p]);
					break;
				}
			break;
		}
	for (int i = 0; i < c->count; i++) {
		if (trs_craft_def(c, i)->is_leg)
			c->parts[i].legs_deployed = 1;
		if (c->parts[i].stage > 0)
			c->parts[i].stage = -1; /* nothing to fire */
	}
	return 1;
}

/* Drops a craft upright onto a body at lat/lon from height agl0 at speed v. */
static int drop_on(trs_world *w, const trs_craft *c, int body, double agl0, double vdown) {
	const trs_body *b = &w->sys.bodies[body];
	trs_v3 up = v3(1, 0, 0);
	double ground = b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, up) : 0;
	trs_v3 r = v3_scale(up, b->radius + ground + agl0);
	trs_v3 v = v3_add(v3_cross(trs_body_omega(b), r), v3_scale(up, -vdown));
	int id = trs_world_add_craft(w, c, body, r, v, quat_from_to(v3(0, 1, 0), up));
	w->vessels[id].sas = TRS_SAS_STABILITY;
	return id;
}

trs_gate_status trs_gate_landing(trs_log *log, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	if (!trs_gate_load_catalog(log, data_dir, &cat) || !load_floe_system(log, data_dir, &sys))
		return TRS_GATE_FAIL;
	int floe = trs_system_find(&sys, "floe"), krill = trs_system_find(&sys, "krill");
	const trs_body *F = &sys.bodies[floe];
	int failed = 0;

	/* 1. Clamps hold on the pad; staging releases them and the rocket climbs. */
	{
		trs_craft c;
		trs_gate_load_craft(log, data_dir, "orbiter", &cat, &c);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = trs_world_launch(w, &c, floe, F->site_lat, F->site_lon);
		double h0 = agl(w, id);
		for (int s = 0; s < 50 * 10; s++)
			trs_world_step_physics(w);
		double h1 = agl(w, id), v1 = surface_speed(w, id);
		w->vessels[id].throttle = 1;
		trs_world_stage(w, id, NULL, 0);
		for (int s = 0; s < 50 * 10; s++)
			trs_world_step_physics(w);
		double h2 = agl(w, id);
		int ok = fabs(h1 - h0) < 1e-6 && v1 < 1e-6 && h2 > 100 && w->vessels[id].craft->count == c.count;
		failed += !ok;
		trs_log_printf(log, "%s pad: clamped 10 s drift %.1e m (speed %.1e), released -> %.0f m AGL after 10 s\n",
				ok ? "ok  " : "FAIL", fabs(h1 - h0), v1, h2);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	/* 2. Lander touches down on Krill at 3 m/s and settles intact. */
	{
		trs_craft c;
		lander_only(log, data_dir, &cat, &c);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = drop_on(w, &c, krill, 3.0, 3.0);
		int s;
		for (s = 0; s < 50 * 30 && !(w->vessels[id].landed && surface_speed(w, id) < 0.05); s++)
			trs_world_step_physics(w);
		int destroyed = count_events(w, TRS_WEV_PART_DESTROYED);
		int ok = w->vessels[id].landed && destroyed == 0 && w->vessels[id].craft->count == c.count;
		failed += !ok;
		trs_log_printf(log, "%s Krill touchdown at 3 m/s: settled in %.1f s, %d parts destroyed, %d contacts, %.2f m AGL\n",
				ok ? "ok  " : "FAIL", s * TRS_PHYSICS_DT, destroyed, w->vessels[id].contacts, agl(w, id));
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	/* 3. The same lander hitting at 40 m/s breaks apart. */
	{
		trs_craft c;
		lander_only(log, data_dir, &cat, &c);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		drop_on(w, &c, krill, 2.0, 40.0);
		for (int s = 0; s < 50 * 5; s++)
			trs_world_step_physics(w);
		int destroyed = count_events(w, TRS_WEV_PART_DESTROYED);
		int ok = destroyed >= 3;
		failed += !ok;
		trs_log_printf(log, "%s Krill impact at 40 m/s: %d parts destroyed, %d debris vessels\n", ok ? "ok  " : "FAIL",
				destroyed, count_events(w, TRS_WEV_VESSEL_CREATED));
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	/* 4. A pod under its chute lands intact on Floe. */
	{
		trs_craft c;
		trs_craft_init(&c, &cat);
		int pod = trs_craft_add(&c, trs_catalog_find(&cat, "pod_1"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
		trs_craft_add(&c, trs_catalog_find(&cat, "parachute"), pod, "bottom", "top", v3(0, 0.675, 0), quat_identity(), 0, 0);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = drop_on(w, &c, floe, 2500.0, 120.0);
		trs_vessel_deploy_chutes(w, id);
		int s;
		for (s = 0; s < 50 * 900 && !(w->vessels[id].landed && surface_speed(w, id) < 0.05); s++)
			trs_world_step_physics(w);
		int destroyed = count_events(w, TRS_WEV_PART_DESTROYED);
		int ok = w->vessels[id].alive && w->vessels[id].landed && destroyed == 0;
		failed += !ok;
		trs_log_printf(log, "%s chute landing on Floe: down after %.0f s, touchdown %.1f m/s, %d parts destroyed\n",
				ok ? "ok  " : "FAIL", s * TRS_PHYSICS_DT, w->vessels[id].max_impact, destroyed);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	/* 5. A fuelled Orbiter toppling onto the ice: the tanks burst and the
	 *    blast takes the crew pod with them (a failed launch is fatal). */
	{
		trs_craft c;
		if (!trs_gate_load_craft(log, data_dir, "orbiter", &cat, &c))
			return TRS_GATE_FAIL;
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = drop_on(w, &c, floe, 3.0, 9.0);
		trs_vessel *vs = &w->vessels[id];
		vs->sas = TRS_SAS_OFF;
		vs->q = quat_from_to(v3(0, 1, 0), v3(0, 1, 0)); /* lying along the horizon */
		for (int s = 0; s < 50 * 10; s++)
			trs_world_step_physics(w);
		int pod_lost = 0, blasted = 0, destroyed = 0;
		for (int e = 0; e < w->nevents; e++)
			if (w->events[e].kind == TRS_WEV_PART_DESTROYED) {
				destroyed++;
				pod_lost |= !strcmp(w->events[e].part, "pod_1");
				blasted += w->events[e].speed < 0;
			}
		int ok = pod_lost && blasted > 0;
		failed += !ok;
		trs_log_printf(log, "%s toppled Orbiter: %d parts destroyed (%d by blast), crew pod %s\n", ok ? "ok  " : "FAIL",
				destroyed, blasted, pod_lost ? "lost" : "survived");
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
