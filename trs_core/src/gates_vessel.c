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

static const char *STOCK_CRAFT[] = { "hopper", "orbiter", "krill_lander", "rookery_heavy", "puffin_ferry", "igloo_station", "sidecar" };
#define NSTOCK 7

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
		int fed = trs_craft_engine_fed(c, &vs->fuel, i);
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

	/* the stock craft, then the Sidecar with its side engines limited to
	 * 70%: the core then drains its side tanks at a different rate */
	for (int k = 0; k <= NSTOCK; k++) {
		trs_craft c;
		if (!trs_gate_load_craft(log, data_dir, STOCK_CRAFT[k < NSTOCK ? k : NSTOCK - 1], &cat, &c)) {
			failed++;
			continue;
		}
		if (k == NSTOCK) {
			for (int i = 0; i < c.count; i++)
				if (c.parts[i].sym != 0 && trs_craft_def(&c, i)->propellant != TRS_PROP_NONE)
					c.parts[i].limit = 0.7;
			snprintf(c.name, sizeof c.name, "Sidecar, side engines at 70%%");
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
			if (i == 0 && w->vessels[id].fuel.nlines > 0) {
				/* feed lines: the side tanks run dry first, the core is untouched */
				const trs_vessel *vs = &w->vessels[id];
				const trs_craft *vc = vs->craft;
				double side = 0, core = 0, core_cap = 0, core_mdot = 0;
				unsigned char *fed = calloc((size_t)vc->count, 1);
				for (int k = 0; k < vs->fuel.nlines; k++)
					for (int q = 0; q < vc->count; q++)
						if (vs->fuel.group[q] == vs->fuel.group[vs->fuel.line_part[k]])
							fed[q] = 1;
				int core_g = vs->fuel.line_target[0] >= 0 ? vs->fuel.group[vs->fuel.line_target[0]] : -1;
				for (int q = 0; q < vc->count; q++) {
					if (vs->fuel.group[q] == core_g && vc->parts[q].engine_on && trs_craft_def(vc, q)->propellant == TRS_PROP_LIQUID)
						core_mdot += trs_engine_mdot(trs_craft_def(vc, q));
					if (trs_craft_def(vc, q)->fuel_liquid <= 0)
						continue;
					if (fed[q])
						side += vc->parts[q].fuel;
					else if (vs->fuel.group[q] == core_g) {
						core += vc->parts[q].fuel;
						core_cap += trs_craft_def(vc, q)->fuel_liquid;
					}
				}
				free(fed);
				/* the core only ever burns its own tanks in the remainder of
				 * the tick in which the side tanks ran dry */
				double tick = core_mdot * TRS_PHYSICS_DT;
				int ok = core_g >= 0 && side == 0 && core_cap > 0 && core <= core_cap && core_cap - core <= tick;
				failed += !ok;
				trs_log_printf(log, "  %s feed lines: side tanks %.3f kg left when their engines quit, core %.2f of %.0f kg (one tick of core flow %.2f kg)\n",
						ok ? "ok  " : "FAIL", side, core, core_cap, tick);
			}
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
	/* 1b. The canopy inflates rather than snapping open: every tick its drag
	 *     area grows by exactly full_cda / inflate_time * dt (capped at the
	 *     state's size), full deployment comes at full_altitude above the
	 *     ground (not sea level), and the opening shock stays far below what
	 *     an instant opening would give. */
	{
		trs_craft c;
		trs_craft_init(&c, &cat);
		int pod = trs_craft_add(&c, trs_catalog_find(&cat, "pod_1"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
		trs_craft_add(&c, trs_catalog_find(&cat, "parachute"), pod, "bottom", "top", v3(0, 0.675, 0), quat_identity(),
				0, 0);
		const trs_part_def *cd = &cat.defs[trs_catalog_find(&cat, "parachute")];
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = drop_at(w, &c, floe, 9000, 280);
		trs_vessel_deploy_chutes(w, id);
		trs_vessel *vs = &w->vessels[id];
		double rate = cd->chute_full_cda / cd->chute_inflate, worst = 0, peak_g = 0, instant_g = 0;
		double full_agl = -1, ground = 0;
		int prev_state = vs->craft->parts[1].chute;
		for (int s = 0; s < 50 * 900 && !vs->landed; s++) {
			double before = vs->craft->parts[1].chute_cda;
			trs_v3 vair = v3_sub(vs->v, v3_cross(trs_body_omega(F), vs->r));
			double q = 0.5 * vs->density * v3_len2(vair);
			trs_world_step_physics(w);
			const trs_part *cp = &vs->craft->parts[1];
			double target = cp->chute == TRS_CHUTE_SEMI ? cd->chute_semi_cda : cp->chute == TRS_CHUTE_FULL ? cd->chute_full_cda : 0;
			double want = fmin(target, before + rate * TRS_PHYSICS_DT);
			worst = fmax(worst, fabs(cp->chute_cda - want));
			if (vs->altitude - trs_body_terrain_at(F, w->t, vs->r) > 30) /* not the touchdown */
				peak_g = fmax(peak_g, v3_len(vs->sensed_accel) / TRS_G0);
			if (cp->chute == TRS_CHUTE_FULL && prev_state != TRS_CHUTE_FULL) {
				ground = trs_body_terrain_at(F, w->t, vs->r);
				full_agl = vs->altitude - ground;
				instant_g = q * cd->chute_full_cda / vs->mass / TRS_G0;
			}
			prev_state = cp->chute;
		}
		int ok = worst < 1e-12 && full_agl <= cd->chute_full_alt && full_agl > cd->chute_full_alt - 5 &&
				peak_g < 0.5 * instant_g && peak_g < 10 && vs->landed;
		failed += !ok;
		trs_log_printf(log, "%s chute inflation: area law err %.1e m^2, full at %.0f m above ground (%.0f m terrain), "
				"peak %.1f g vs %.1f g if it snapped open\n", ok ? "ok  " : "FAIL", worst, full_agl, ground, peak_g, instant_g);
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
	/* 5. Fins keep a rocket pointed: the Orbiter coasting through thick fast
	 *    air (no thrust, no SAS) and knocked 6 deg off the airflow swings
	 *    back with its fins and tumbles without them (the nose pushes
	 *    sideways ahead of the centre of mass). */
	{
		double end_aoa[2] = { 0, 0 }; /* worst angle: fins after the first 2 s, bare throughout */
		for (int k = 0; k < 2; k++) {
			trs_craft c;
			if (!trs_gate_load_craft(log, data_dir, "orbiter", &cat, &c))
				return TRS_GATE_FAIL;
			if (k == 1)
				for (int i = c.count - 1; i >= 0; i--)
					if (!strcmp(trs_craft_def(&c, i)->id, "fin")) {
						trs_craft pieces[4];
						int n = trs_craft_remove_part(&c, i, pieces, 4);
						for (int p = 0; p < n; p++)
							trs_craft_free(&pieces[p]);
					}
			/* by 9 km the first stage has burnt most of its propellant (the
			 * stock ascent arrives there with ~35% left in it) */
			for (int i = 0; i < c.count; i++)
				if (trs_craft_def(&c, i)->fuel_liquid > 0 && c.parts[i].pos.y < -4)
					c.parts[i].fuel *= 0.35;
			trs_world *w = malloc(sizeof(trs_world));
			trs_world_init(w, &sys, 0);
			/* 9 km up at 330 m/s, climbing 45 deg, nose 6 deg above the flow */
			trs_v3 up = v3(1, 0, 0), east = v3(0, 0, 1);
			trs_v3 r = v3_scale(up, F->radius + trs_body_terrain_at(F, 0, v3_scale(up, F->radius)) + 9000);
			trs_v3 dir = v3_norm(v3_add(up, east));
			trs_v3 v = v3_add(v3_cross(trs_body_omega(F), r), v3_scale(dir, 330));
			double a6 = 6 * TRS_PI / 180;
			trs_v3 nose = v3_add(v3_scale(dir, cos(a6)), v3_scale(v3_norm(v3_sub(up, east)), sin(a6)));
			int id = trs_world_add_craft(w, &c, floe, r, v, quat_from_to(v3(0, 1, 0), nose));
			trs_vessel *vs = &w->vessels[id];
			vs->sas = TRS_SAS_OFF;
			vs->w = trs_body_omega(F);
			for (int s = 0; s < 50 * 8; s++) {
				trs_world_step_physics(w);
				trs_v3 vair = v3_sub(vs->v, v3_cross(trs_body_omega(F), vs->r));
				double aoa = acos(fmax(-1, fmin(1, v3_dot(v3_norm(vair), quat_rotate(vs->q, v3(0, 1, 0)))))) * 180 / TRS_PI;
				if (k == 1 || s >= 50 * 2) /* fins: once it has swung back */
					end_aoa[k] = fmax(end_aoa[k], aoa);
			}
			trs_world_free(w);
			free(w);
			trs_craft_free(&c);
		}
		int ok = end_aoa[0] < 6 && end_aoa[1] > 60;
		failed += !ok;
		trs_log_printf(log, "%s Orbiter knocked 6 deg off the flow at 9 km, 330 m/s, hands off for 8 s: with fins it swings back "
				"(worst %.1f deg after 2 s), without it tumbles (worst %.0f deg)\n", ok ? "ok  " : "FAIL", end_aoa[0], end_aoa[1]);
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
		if (c->parts[i].stage > 1)
			c->parts[i].stage = -1; /* nothing to fire past the lander */
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
	/* 5. A fuelled Orbiter toppling onto the ice: the crash takes the crew
	 *    pod with it (a failed launch is fatal), and the wreck scatters
	 *    without flinging anything off like a shell. */
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
		double fastest = 0;
		for (int s = 0; s < 50 * 10; s++) {
			trs_world_step_physics(w);
			for (int k = 0; k < w->nvessels; k++)
				if (w->vessels[k].alive && w->vessels[k].craft) {
					double sp = surface_speed(w, k);
					fastest = fmax(fastest, sp);
				}
		}
		int pod_lost = 0, blasted = 0, destroyed = 0;
		for (int e = 0; e < w->nevents; e++)
			if (w->events[e].kind == TRS_WEV_PART_DESTROYED) {
				destroyed++;
				pod_lost |= !strcmp(w->events[e].part, "pod_1");
				blasted += w->events[e].speed < 0;
			}
		/* the wreckage scatters, but nothing is flung off like a shell */
		int ok = pod_lost && destroyed >= 5 && fastest < 60;
		failed += !ok;
		trs_log_printf(log, "%s toppled Orbiter: %d parts destroyed (%d by blast), crew pod %s, fastest debris %.0f m/s\n",
				ok ? "ok  " : "FAIL", destroyed, blasted, pod_lost ? "lost" : "survived", fastest);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	/* 6. A full tank going up takes its neighbours with it; the same tank
	 *    empty just breaks off. */
	for (int full = 1; full >= 0; full--) {
		trs_craft c;
		if (!trs_gate_load_craft(log, data_dir, "orbiter", &cat, &c))
			return TRS_GATE_FAIL;
		int tank = -1;
		for (int i = 0; i < c.count && tank < 0; i++)
			if (trs_craft_def(&c, i)->fuel_liquid > 0)
				tank = i;
		if (!full)
			c.parts[tank].fuel = 0;
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		trs_v3 r = v3(F->radius + 150000, 0, 0);
		int id = trs_world_add_craft(w, &c, floe, r, v3(0, sqrt(F->mu / v3_len(r)), 0), quat_identity());
		trs_vessel_destroy_part(w, id, tank, 30);
		for (int s = 0; s < 5; s++)
			trs_world_step_physics(w);
		int blasted = 0, destroyed = 0;
		for (int e = 0; e < w->nevents; e++)
			if (w->events[e].kind == TRS_WEV_PART_DESTROYED) {
				destroyed++;
				blasted += w->events[e].speed == TRS_DESTROY_BLAST;
			}
		int ok = full ? blasted >= 2 : blasted == 0 && destroyed == 1;
		failed += !ok;
		trs_log_printf(log, "%s %s tank destroyed in orbit: %d parts lost, %d of them to the blast\n", ok ? "ok  " : "FAIL",
				full ? "full" : "empty", destroyed, blasted);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}

/* ---- heating -------------------------------------------------------------------- */

/* Pod (+ stowed chute, + optional heat shield underneath) entering Floe's
 * atmosphere at 70 km with speed v (m/s) and flight path angle gamma (deg,
 * negative = descending), shield-first, free to weathervane. Runs until
 * 3 km above the datum or the pod is lost; reports each part's peak. */
static void entry_run(const trs_system *sys, const trs_part_catalog *cat, int shield, double v, double gamma,
		int *pod_lost, double *peak_pod, double *peak_shield, double *peak_frac) {
	int floe = trs_system_find(sys, "floe");
	const trs_body *F = &sys->bodies[floe];
	trs_craft c;
	trs_craft_init(&c, cat);
	int pod = trs_craft_add(&c, trs_catalog_find(cat, "pod_1"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	trs_craft_add(&c, trs_catalog_find(cat, "parachute"), pod, "bottom", "top", v3(0, 0.675, 0), quat_identity(), -1, 0);
	if (shield)
		trs_craft_add(&c, trs_catalog_find(cat, "heat_shield"), pod, "top", "bottom", v3(0, -0.58, 0), quat_identity(), -1, 0);
	trs_world *w = malloc(sizeof(trs_world));
	trs_world_init(w, sys, 0);
	double g = gamma * TRS_PI / 180;
	trs_v3 r = v3(F->radius + 70000, 0, 0);
	trs_v3 vel = v3(v * sin(g), v * cos(g), 0);
	/* shield first: the nose (+Y) points back along the velocity */
	int id = trs_world_add_craft(w, &c, floe, r, vel, quat_from_to(v3(0, 1, 0), v3_scale(v3_norm(vel), -1)));
	trs_vessel_set_mode(w, id, TRS_MODE_PHYSICS);
	w->vessels[id].sas = TRS_SAS_OFF;
	*pod_lost = 0;
	*peak_pod = *peak_shield = *peak_frac = 0;
	for (int s = 0; s < 50 * 1500 && !*pod_lost; s++) {
		trs_world_step_physics(w);
		trs_vessel *vs = &w->vessels[id];
		for (int e = 0; e < w->nevents; e++)
			*pod_lost |= w->events[e].kind == TRS_WEV_PART_DESTROYED && !strcmp(w->events[e].part, "pod_1");
		w->nevents = 0;
		if (*pod_lost || !vs->alive)
			break;
		for (int i = 0; i < vs->craft->count; i++) {
			const trs_part_def *d = trs_craft_def(vs->craft, i);
			double T = vs->craft->parts[i].temp;
			if (!strcmp(d->id, "pod_1"))
				*peak_pod = fmax(*peak_pod, T);
			if (!strcmp(d->id, "heat_shield"))
				*peak_shield = fmax(*peak_shield, T);
			*peak_frac = fmax(*peak_frac, T / d->max_temp);
		}
		if (vs->altitude < 3000)
			break;
	}
	trs_world_free(w);
	free(w);
	trs_craft_free(&c);
}

/* Stack-attaches part `id` under `parent` (its top node to the parent's
 * bottom node). Returns the new index. */
static int stack_below(trs_craft *c, const trs_part_catalog *cat, int parent, const char *id, int stage) {
	int di = trs_catalog_find(cat, id);
	const trs_attach_node *pb = trs_part_node(trs_craft_def(c, parent), "bottom");
	const trs_attach_node *ct = trs_part_node(&cat->defs[di], "top");
	trs_v3 pos = v3_sub(v3_add(c->parts[parent].pos, pb->pos), ct->pos);
	return trs_craft_add(c, di, parent, "top", "bottom", pos, quat_identity(), stage, 0);
}

trs_gate_status trs_gate_heating(trs_log *log, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	if (!trs_gate_load_catalog(log, data_dir, &cat) || !load_floe_system(log, data_dir, &sys))
		return TRS_GATE_FAIL;
	int floe = trs_system_find(&sys, "floe");
	const trs_body *F = &sys.bodies[floe];
	int failed = 0;

	/* 1. Every tick of a fast, low dive, the pod's temperature change equals
	 *    the heating law evaluated independently from the same state. */
	{
		trs_craft c;
		trs_craft_init(&c, &cat);
		trs_craft_add(&c, trs_catalog_find(&cat, "pod_1"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
		const trs_part_def *pd = &cat.defs[trs_catalog_find(&cat, "pod_1")];
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = drop_at(w, &c, floe, 30000, 1800);
		trs_vessel *vs = &w->vessels[id];
		vs->q = quat_from_to(v3(0, 1, 0), v3(1, 0, 0)); /* shield (bottom) down, into the flow */
		double worst = 0, peak = 0;
		trs_world_step_physics(w); /* the first tick fills the density cache */
		for (int s = 0; s < 50 * 20; s++) {
			double T0 = vs->craft->parts[0].temp;
			trs_world_step_physics(w);
			/* heat() uses the tick-start density and the end-of-tick motion */
			double rho_used = vs->density;
			trs_v3 vair = v3_sub(vs->v, v3_cross(trs_body_omega(F), vs->r));
			double sp = v3_len(vair);
			double cs = v3_dot(v3_norm(vair), quat_rotate(vs->q, v3(0, 1, 0)));
			/* a lone pod: both end faces fully exposed */
			double f = fmax(fabs(cs), 0.05 + 0.4 * sqrt(fmax(0, 1 - cs * cs)));
			double hc = 2 * 1005.0 * 1.7415e-4 * sqrt(rho_used / pd->radius) * sp;
			double t_rec = TRS_T_AMBIENT + sp * sp / (2 * 1005.0);
			double q = f * hc * (t_rec - T0) - pd->emissivity * 5.670374419e-8 * (pow(T0, 4) - pow(TRS_T_AMBIENT, 4));
			double want = T0 + q * (1.0 / 50.0) / pd->skin_heat;
			double T1 = vs->craft->parts[0].temp;
			worst = fmax(worst, fabs(T1 - want) / fmax(fabs(want - T0), 1e-3));
			peak = fmax(peak, T1);
			if (vs->altitude < 8000)
				break;
		}
		int ok = worst < 1e-9;
		failed += !ok;
		trs_log_printf(log, "%s pod diving from 30 km at 1.8 km/s: dT per tick vs heating law rel err %.1e (peak %.0f K)\n",
				ok ? "ok  " : "FAIL", worst, peak);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	/* 2. Under a parachute nothing heats: below the recovery temperature. */
	{
		trs_craft c;
		trs_craft_init(&c, &cat);
		int pod = trs_craft_add(&c, trs_catalog_find(&cat, "pod_1"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
		trs_craft_add(&c, trs_catalog_find(&cat, "parachute"), pod, "bottom", "top", v3(0, 0.675, 0), quat_identity(), 0, 0);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = drop_at(w, &c, floe, 3000, 150);
		trs_vessel_deploy_chutes(w, id);
		trs_vessel *vs = &w->vessels[id];
		double peak = 0;
		for (int s = 0; s < 50 * 60; s++) {
			trs_world_step_physics(w);
			peak = fmax(peak, vs->craft->parts[0].temp);
		}
		double bound = TRS_T_AMBIENT + 150.0 * 150.0 / (2 * 1005.0);
		int ok = peak <= bound;
		failed += !ok;
		trs_log_printf(log, "%s chute descent from 3 km at 150 m/s: pod peaks %.2f K, recovery bound %.2f K\n",
				ok ? "ok  " : "FAIL", peak, bound);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	/* 3. Entries: a shallow return from low orbit is survivable bare; a steep,
	 *    fast return from Krill burns up a bare pod and is survived behind a
	 *    heat shield. */
	{
		struct { const char *what; int shield; double v, gamma; int want_lost; } cases[] = {
			{ "low-orbit return, bare pod   ", 0, 2300, -1.5, 0 },
			{ "Krill return, bare pod       ", 0, 3200, -14.0, 1 },
			{ "Krill return, with heat shield", 1, 3200, -14.0, 0 },
		};
		for (int k = 0; k < 3; k++) {
			int lost;
			double pp, ps, pf;
			entry_run(&sys, &cat, cases[k].shield, cases[k].v, cases[k].gamma, &lost, &pp, &ps, &pf);
			int ok = lost == cases[k].want_lost;
			failed += !ok;
			trs_log_printf(log, "%s %s %.1f km/s at %+.1f deg: pod %s, peaks pod %.0f K%s, hottest part %.0f%% of limit\n",
					ok ? "ok  " : "FAIL", cases[k].what, cases[k].v / 1000, cases[k].gamma, lost ? "LOST" : "survives",
					pp, "", 100 * pf);
			if (cases[k].shield)
				trs_log_printf(log, "     shield peaked at %.0f K\n", ps);
		}
	}
	/* 4. Exhaust: an engine fired straight into another engine below it
	 *    cooks it within seconds; the same engine with nothing below it
	 *    heats nothing. In orbit, so the air plays no part; then again
	 *    flying nose-first through the air at 5 km, where the jet's heat
	 *    adds to the air's (the air must not mask it). */
	for (int k = 0; k < 3; k++) {
		trs_craft c;
		trs_craft_init(&c, &cat);
		int pod = trs_craft_add(&c, trs_catalog_find(&cat, "pod_1"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
		int tank = stack_below(&c, &cat, pod, "tank_l", -1);
		int eng = stack_below(&c, &cat, tank, "engine_small", 0);
		if (k != 1)
			stack_below(&c, &cat, eng, "engine_small", 0);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		trs_v3 r = v3(F->radius + (k == 2 ? 5000 : 150000), 0, 0);
		trs_v3 v = k == 2 ? v3_add(v3_cross(trs_body_omega(F), r), v3(0, 200, 0)) : v3(0, sqrt(F->mu / v3_len(r)), 0);
		int id = trs_world_add_craft(w, &c, floe, r, v, quat_identity());
		w->vessels[id].throttle = 1;
		trs_world_stage(w, id, NULL, 0);
		double worst = 0, lost_t = -1;
		for (int s = 0; s < 50 * 20 && lost_t < 0; s++) {
			trs_world_step_physics(w);
			const trs_craft *cc = w->vessels[id].craft;
			for (int i = 0; i < cc->count; i++)
				worst = fmax(worst, (cc->parts[i].temp - TRS_T_AMBIENT) / (trs_craft_def(cc, i)->max_temp - TRS_T_AMBIENT));
			for (int e = 0; e < w->nevents; e++)
				if (w->events[e].kind == TRS_WEV_PART_DESTROYED && lost_t < 0)
					lost_t = s * TRS_PHYSICS_DT;
		}
		int ok = k != 1 ? lost_t > 0 && lost_t < 10 : lost_t < 0 && worst < 0.05;
		failed += !ok;
		if (k != 1)
			trs_log_printf(log, "%s engine firing into a second engine below it%s: the lower one burns up after %.1f s\n",
					ok ? "ok  " : "FAIL", k == 2 ? ", at 200 m/s through the air at 5 km" : "", lost_t);
		else
			trs_log_printf(log, "%s the same engine with nothing below it: 20 s at full thrust, hottest part %.1f%% of its headroom\n",
					ok ? "ok  " : "FAIL", 100 * worst);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}

/* Two single-tank craft side by side in a low Floe orbit, closing at
 * `closing` m/s along the line between them. */
static void pair_in_orbit(trs_world *w, const trs_craft *c, int floe, double closing, int *a, int *b) {
	const trs_body *F = &w->sys.bodies[floe];
	trs_v3 r0 = v3(F->radius + 150000, 0, 0);
	trs_v3 v0 = v3(0, sqrt(F->mu / v3_len(r0)), 0);
	trs_quat q = quat_from_to(v3(0, 1, 0), v3(1, 0, 0));
	*a = trs_world_add_craft(w, c, floe, v3_add(r0, v3(0, 0, -0.75)), v3_add(v0, v3(0, 0, closing * 0.5)), q);
	*b = trs_world_add_craft(w, c, floe, v3_add(r0, v3(0, 0, 0.75)), v3_sub(v0, v3(0, 0, closing * 0.5)), q);
	w->vessels[*a].sas = TRS_SAS_OFF;
	w->vessels[*b].sas = TRS_SAS_OFF;
}

trs_gate_status trs_gate_collision(trs_log *log, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	if (!trs_gate_load_catalog(log, data_dir, &cat) || !load_floe_system(log, data_dir, &sys))
		return TRS_GATE_FAIL;
	int floe = trs_system_find(&sys, "floe");
	const trs_body *F = &sys.bodies[floe];
	int failed = 0;

	/* 1. On the pad, a stage that only decouples leaves the rocket clamped,
	 *    and the capsule it lets go sits back down on it instead of
	 *    falling through. */
	{
		trs_craft c;
		trs_gate_load_craft(log, data_dir, "orbiter", &cat, &c);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = trs_world_launch(w, &c, floe, F->site_lat, F->site_lon);
		w->vessels[id].next_stage = 0; /* the capsule decoupler first, as a mis-staged rocket would */
		int pieces[4];
		int n = trs_world_stage(w, id, pieces, 4);
		int pod = -1, base = -1;
		for (int k = -1; k < n; k++) {
			int vid = k < 0 ? id : pieces[k];
			for (int i = 0; i < w->vessels[vid].craft->count; i++) {
				if (!strcmp(trs_craft_def(w->vessels[vid].craft, i)->id, "pod_1"))
					pod = vid;
				if (!strcmp(trs_craft_def(w->vessels[vid].craft, i)->id, "engine_lifter"))
					base = vid;
			}
		}
		double h0 = pod >= 0 ? agl(w, pod) : 0, low = h0;
		for (int s = 0; s < 50 * 15; s++) {
			trs_world_step_physics(w);
			low = fmin(low, agl(w, pod));
		}
		int destroyed = count_events(w, TRS_WEV_PART_DESTROYED);
		int held = base >= 0 && w->vessels[base].clamped && w->vessels[base].mode == TRS_MODE_LANDED;
		int ok = n == 1 && pod >= 0 && pod != base && held && low > h0 - 0.3 && fabs(agl(w, pod) - h0) < 0.3 &&
				surface_speed(w, pod) < 0.05 && destroyed == 0;
		failed += !ok;
		trs_log_printf(log, "%s capsule dropped on its clamped rocket: rocket held %s, capsule lowest %+.3f m, rests at %+.3f m "
				"from where it let go (%.3f m/s), %d parts broken\n", ok ? "ok  " : "FAIL", held ? "yes" : "NO", low - h0,
				pod >= 0 ? agl(w, pod) - h0 : 0, pod >= 0 ? surface_speed(w, pod) : 0, destroyed);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	/* 2. Two tanks bumping at 2 m/s in orbit: every contact force has its
	 *    equal and opposite partner, they never sink into each other more
	 *    than the contact allows, and they come apart again, slower. */
	{
		trs_craft c;
		trs_craft_init(&c, &cat);
		trs_craft_add(&c, trs_catalog_find(&cat, "tank_l"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int a, b;
		pair_in_orbit(w, &c, floe, 2.0, &a, &b);
		/* start them just out of reach so they close in */
		w->vessels[a].r = v3_add(w->vessels[a].r, v3(0, 0, -0.3));
		w->vessels[b].r = v3_add(w->vessels[b].r, v3(0, 0, 0.3));
		double worst_sum = 0, worst_f = 0, closest = 1e9;
		double r_sum = 2 * trs_craft_def(&c, 0)->radius;
		for (int s = 0; s < 50 * 4; s++) {
			trs_world_vessel_contacts(w);
			trs_v3 fs = v3_add(w->vessels[a].ext_force, w->vessels[b].ext_force);
			worst_sum = fmax(worst_sum, v3_len(fs));
			worst_f = fmax(worst_f, v3_len(w->vessels[a].ext_force));
			trs_world_step_physics(w);
			closest = fmin(closest, v3_len(v3_sub(w->vessels[a].r, w->vessels[b].r)));
		}
		double sep = v3_dot(v3_sub(w->vessels[b].v, w->vessels[a].v), v3_norm(v3_sub(w->vessels[b].r, w->vessels[a].r)));
		int ok = worst_f > 0 && worst_sum <= 1e-9 * worst_f && closest > r_sum - 0.16 && sep > 0.2 && sep < 2.0;
		failed += !ok;
		trs_log_printf(log, "%s tanks bumping at 2 m/s in orbit: action-reaction residual %.1e of %.0f N, closest %.3f m "
				"(touching at %.3f), part again at %.2f m/s\n", ok ? "ok  " : "FAIL", worst_sum, worst_f, closest, r_sum, sep);
		trs_world_free(w);
		free(w);
		/* 3. The same at 30 m/s breaks them. */
		w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		pair_in_orbit(w, &c, floe, 30.0, &a, &b);
		w->vessels[a].r = v3_add(w->vessels[a].r, v3(0, 0, -0.3));
		w->vessels[b].r = v3_add(w->vessels[b].r, v3(0, 0, 0.3));
		for (int s = 0; s < 50 * 1; s++)
			trs_world_step_physics(w);
		int destroyed = count_events(w, TRS_WEV_PART_DESTROYED);
		ok = destroyed > 0;
		failed += !ok;
		trs_log_printf(log, "%s tanks colliding at 30 m/s (crash tolerance %.0f m/s): %d broken\n", ok ? "ok  " : "FAIL",
				trs_craft_def(&c, 0)->crash_tolerance, destroyed);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
	}
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}

/* ---- RCS ------------------------------------------------------------------- */

static double rocket_dv(double ve, double m0, double m1) {
	return ve * log(m0 / m1);
}

/* Probe core, mono tank and a small tank, with four RCS blocks around the
 * mono tank at the CoM's height (so the arms are all radial). */
static void rcs_craft(trs_craft *c, const trs_part_catalog *cat, double *arm) {
	trs_craft_init(c, cat);
	int pc = trs_craft_add(c, trs_catalog_find(cat, "probe_core"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	int mt = stack_below(c, cat, pc, "mono_tank", -1);
	stack_below(c, cat, mt, "tank_s", -1);
	double m;
	trs_v3 cm;
	trs_craft_mass_props(c, &m, &cm, NULL);
	int bi = trs_catalog_find(cat, "rcs_block");
	double R = trs_craft_def(c, mt)->radius + cat->defs[bi].radius;
	for (int k = 0; k < 4; k++) {
		trs_quat q = quat_axis_angle(v3(0, 1, 0), k * TRS_PI / 2);
		trs_v3 out = quat_rotate(q, v3(0, 0, 1));
		trs_craft_add(c, bi, mt, "", "", v3(out.x * R, cm.y, out.z * R), q, -1, 1);
	}
	*arm = R;
}

static int rcs_world(trs_world *w, const trs_system *sys, const trs_craft *c, int floe) {
	const trs_body *F = &sys->bodies[floe];
	trs_world_init(w, sys, 0);
	trs_v3 r = v3(F->radius + 300000, 0, 0);
	int id = trs_world_add_craft(w, c, floe, r, v3(0, sqrt(F->mu / v3_len(r)), 0), quat_identity());
	w->vessels[id].sas = TRS_SAS_OFF;
	return id;
}

trs_gate_status trs_gate_rcs(trs_log *log, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	if (!trs_gate_load_catalog(log, data_dir, &cat) || !load_floe_system(log, data_dir, &sys))
		return TRS_GATE_FAIL;
	int floe = trs_system_find(&sys, "floe");
	int failed = 0;
	const trs_part_def *bd = &cat.defs[trs_catalog_find(&cat, "rcs_block")];
	double T = bd->rcs_thrust, isp = bd->rcs_isp_vac, mdot1 = T / (isp * TRS_G0);
	double arm;
	trs_craft c;
	rcs_craft(&c, &cat, &arm);

	/* 1. Sliding forward for 5 s: the four rear-facing jets, the rocket
	 *    equation on the mono burnt, and no turn. Measured against an
	 *    identical craft coasting beside it (the ~60 m they drift apart
	 *    leaves a tidal difference of a few parts in 10^5). */
	{
		trs_world *a = malloc(sizeof(trs_world)), *b = malloc(sizeof(trs_world));
		int ia = rcs_world(a, &sys, &c, floe), ib = rcs_world(b, &sys, &c, floe);
		double m0 = a->vessels[ia].mass, mono0 = trs_craft_mono(a->vessels[ia].craft, NULL);
		a->vessels[ia].rcs = 1;
		a->vessels[ia].translate = v3(0, 1, 0);
		for (int s = 0; s < 250; s++) {
			trs_world_step_physics(a);
			trs_world_step_physics(b);
		}
		double used = mono0 - trs_craft_mono(a->vessels[ia].craft, NULL);
		double dv = v3_len(v3_sub(a->vessels[ia].v, b->vessels[ib].v));
		double want_used = 4 * mdot1 * 5, want_dv = rocket_dv(isp * TRS_G0, m0, m0 - want_used);
		double spin = v3_len(a->vessels[ia].w);
		int ok = fabs(used / want_used - 1) < 1e-9 && fabs(dv / want_dv - 1) < 1e-4 && spin < 1e-9;
		failed += !ok;
		trs_log_printf(log, "%s translate 5 s: mono used %.6f kg (4 jets x %.6f kg/s: %.6f), dv %.6f m/s (rocket eq. %.6f), spin %.1e rad/s\n",
				ok ? "ok  " : "FAIL", used, mdot1, want_used, dv, want_dv, spin);
		trs_world_free(a);
		trs_world_free(b);
		free(a);
		free(b);
	}
	/* 2. Full pitch for 1 s: RCS adds two jets' torque (arm x thrust each)
	 *    on top of the probe core's wheel. (Draining mono walks the CoM a
	 *    hair off the blocks' plane, so the side jets add a trickle.) */
	{
		double om[2], Ixx = 0, used = 0;
		for (int on = 0; on < 2; on++) {
			trs_world *w = malloc(sizeof(trs_world));
			int id = rcs_world(w, &sys, &c, floe);
			trs_vessel *vs = &w->vessels[id];
			vs->rcs = on;
			vs->input = v3(1, 0, 0);
			Ixx = vs->inertia.m[0][0];
			double mono0 = trs_craft_mono(vs->craft, NULL);
			for (int s = 0; s < 50; s++)
				trs_world_step_physics(w);
			vs = &w->vessels[id];
			om[on] = quat_rotate(quat_conj(vs->q), vs->w).x;
			if (on)
				used = mono0 - trs_craft_mono(vs->craft, NULL);
			trs_world_free(w);
			free(w);
		}
		double want = 2 * arm * T / Ixx;
		int ok = fabs((om[1] - om[0]) / want - 1) < 0.01 && fabs(used / (2 * mdot1) - 1) < 1e-3;
		failed += !ok;
		trs_log_printf(log, "%s pitch 1 s: wheel alone %.5f rad/s, with RCS %.5f (+%.5f, 2 jets x %.0f N x %.3f m / I %.0f = %.5f), mono %.6f kg\n",
				ok ? "ok  " : "FAIL", om[0], om[1], om[1] - om[0], T, arm, Ixx, want, used);
	}
	/* 3. Disarmed RCS ignores the translation keys; armed, it runs the mono
	 *    dry and stops, never below empty. */
	{
		trs_world *w = malloc(sizeof(trs_world));
		int id = rcs_world(w, &sys, &c, floe);
		trs_vessel *vs = &w->vessels[id];
		double mono0 = trs_craft_mono(vs->craft, NULL);
		vs->translate = v3(0, 1, 0);
		for (int s = 0; s < 100; s++)
			trs_world_step_physics(w);
		vs = &w->vessels[id];
		double idle = mono0 - trs_craft_mono(vs->craft, NULL);
		vs->rcs = 1;
		double t_dry = -1;
		for (int s = 0; s < 50 * 300 && t_dry < 0; s++) {
			trs_world_step_physics(w);
			if (trs_craft_mono(w->vessels[id].craft, NULL) <= 0)
				t_dry = (s + 1) * TRS_PHYSICS_DT;
		}
		trs_world_step_physics(w);
		vs = &w->vessels[id];
		double left = trs_craft_mono(vs->craft, NULL), want_t = mono0 / (4 * mdot1);
		int ok = idle == 0 && left == 0 && fabs(t_dry - want_t) <= TRS_PHYSICS_DT && v3_len(vs->rcs_force) == 0;
		failed += !ok;
		trs_log_printf(log, "%s disarmed: %.3f kg used; armed: dry after %.2f s (mono / 4 jets = %.2f s), %.3f kg left, force after %.1f N\n",
				ok ? "ok  " : "FAIL", idle, t_dry, want_t, left, v3_len(vs->rcs_force));
		trs_world_free(w);
		free(w);
	}
	trs_craft_free(&c);
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}

/* ---- persistence ----------------------------------------------------------- */

static char *world_text(const trs_world *w) {
	trs_json *j = trs_world_to_json(w);
	char *s = trs_json_write_compact(j);
	trs_json_free(j);
	return s;
}

trs_gate_status trs_gate_persist(trs_log *log, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	if (!trs_gate_load_catalog(log, data_dir, &cat) || !load_floe_system(log, data_dir, &sys))
		return TRS_GATE_FAIL;
	int floe = trs_system_find(&sys, "floe");
	const trs_body *F = &sys.bodies[floe];
	int failed = 0;
	trs_craft orb, hop;
	if (!trs_gate_load_craft(log, data_dir, "orbiter", &cat, &orb) || !trs_gate_load_craft(log, data_dir, "hopper", &cat, &hop))
		return TRS_GATE_FAIL;

	/* A world mid-flight: an Orbiter 20 s into its climb (boosters burning,
	 * SAS holding, RCS armed), a Hopper coasting on rails in orbit, and a
	 * second Hopper clamped to the pad far round the planet. */
	trs_world *a = malloc(sizeof(trs_world)), *b = malloc(sizeof(trs_world));
	trs_world_init(a, &sys, 0);
	int ro = trs_world_launch(a, &orb, floe, F->site_lat, F->site_lon);
	trs_world_launch(a, &hop, floe, F->site_lat + 0.3, F->site_lon + 2.0);
	trs_v3 r = v3(F->radius + 120000, 0, 0);
	int rh = trs_world_add_craft(a, &hop, floe, r, v3(0, sqrt(F->mu / v3_len(r)), 0), quat_identity());
	trs_vessel_set_mode(a, rh, TRS_MODE_RAILS);
	a->vessels[ro].throttle = 1;
	a->vessels[ro].rcs = 1;
	a->vessels[ro].sas = TRS_SAS_STABILITY;
	trs_world_stage(a, ro, NULL, 0);
	for (int s = 0; s < 50 * 20; s++)
		trs_world_step_physics(a);
	char *saved = world_text(a);

	/* 1. Load it into a fresh world: saving that again gives the same text. */
	char err[256] = "";
	trs_json *j = trs_json_parse(saved, err, sizeof err);
	trs_world_init(b, &sys, 0);
	int loaded = j && trs_world_from_json(b, &cat, j, err, sizeof err);
	trs_json_free(j);
	char *again = loaded ? world_text(b) : NULL;
	int same = again && !strcmp(saved, again);
	failed += !same;
	int nv = 0;
	for (int i = 0; i < a->nvessels; i++)
		nv += a->vessels[i].alive;
	trs_log_printf(log, "%s saved %d vessels at T+%.0f s (%zu bytes); loaded %s, saved again identical %s\n", same ? "ok  " : "FAIL",
			nv, a->t, strlen(saved), loaded ? "yes" : err, same ? "yes" : "no");
	free(again);

	/* 2. Both worlds fly on for 60 s, staging at the same tick: every vessel
	 *    and part stays bit-identical. */
	int diverged = -1;
	for (int s = 0; s < 50 * 60 && loaded && diverged < 0; s++) {
		if (s == 50 * 10) {
			trs_world_stage(a, ro, NULL, 0);
			trs_world_stage(b, ro, NULL, 0);
		}
		trs_world_step_physics(a);
		trs_world_step_physics(b);
		if (s % 50 == 49) {
			char *ta = world_text(a), *tb = world_text(b);
			if (strcmp(ta, tb))
				diverged = s;
			free(ta);
			free(tb);
		}
	}
	int ok = loaded && diverged < 0;
	failed += !ok;
	trs_log_printf(log, "%s flown on 60 s side by side (a stage at 10 s): %s; Orbiter at %.1f km, %.1f m/s\n", ok ? "ok  " : "FAIL",
			diverged < 0 ? "identical every second" : "DIVERGED", (v3_len(a->vessels[ro].r) - F->radius) / 1000, v3_len(a->vessels[ro].v));

	/* 3. A bad save is refused and leaves the world as it was. */
	char *before = world_text(b);
	trs_json *bad = trs_json_parse("{\"format\":\"trs-world\",\"t\":5,\"vessels\":[{\"id\":0,\"body_id\":\"floe\",\"craft\":{\"parts\":[{\"part\":\"no_such_part\"}]}}]}", err, sizeof err);
	int refused = !trs_world_from_json(b, &cat, bad, err, sizeof err);
	trs_json_free(bad);
	char *after = world_text(b);
	ok = refused && !strcmp(before, after);
	failed += !ok;
	trs_log_printf(log, "%s a save naming an unknown part is refused (%s), world unchanged %s\n", ok ? "ok  " : "FAIL", err,
			!strcmp(before, after) ? "yes" : "no");
	free(before);
	free(after);

	free(saved);
	trs_world_free(a);
	trs_world_free(b);
	free(a);
	free(b);
	trs_craft_free(&orb);
	trs_craft_free(&hop);
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}

/* ---- docking ------------------------------------------------------------------- */

/* A docking port on top of a probe core, a tank and an engine below. */
static void dock_craft(trs_craft *c, const trs_part_catalog *cat, const char *name, int *port) {
	trs_craft_init(c, cat);
	snprintf(c->name, sizeof c->name, "%s", name);
	int pc = trs_craft_add(c, trs_catalog_find(cat, "probe_core"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	int di = trs_catalog_find(cat, "dock_port");
	const trs_attach_node *pt = trs_part_node(trs_craft_def(c, pc), "top"), *db = trs_part_node(&cat->defs[di], "bottom");
	*port = trs_craft_add(c, di, pc, "bottom", "top", v3_sub(pt->pos, db->pos), quat_identity(), -1, 0);
	int tk = stack_below(c, cat, pc, "tank_s", -1);
	stack_below(c, cat, tk, "engine_small", 0);
}

/* Total momentum and angular momentum about the given point, frame moving at V. */
static void pair_momentum(const trs_world *w, int a, int b, trs_v3 R, trs_v3 V, trs_v3 *P, trs_v3 *L) {
	*P = v3(0, 0, 0);
	*L = v3(0, 0, 0);
	int ids[2] = { a, b };
	for (int k = 0; k < (b >= 0 ? 2 : 1); k++) {
		const trs_vessel *vs = &w->vessels[ids[k]];
		trs_m3 I = m3_rotate_tensor(vs->q, &vs->inertia);
		*P = v3_add(*P, v3_scale(vs->v, vs->mass));
		*L = v3_add(*L, v3_add(m3_mulv(&I, vs->w), v3_scale(v3_cross(v3_sub(vs->r, R), v3_sub(vs->v, V)), vs->mass)));
	}
}

/* Two probes nose to nose `gap` m apart in orbit, closing at `speed`, the
 * second turned `tilt` rad off face-to-face. */
static void dock_pair(trs_world *w, const trs_craft *ca, const trs_craft *cb, int floe, double gap, double speed, double tilt,
		int *a, int *b) {
	const trs_body *F = &w->sys.bodies[floe];
	trs_v3 r0 = v3(F->radius + 200000, 0, 0), v0 = v3(0, sqrt(F->mu / v3_len(r0)), 0);
	trs_quat qa = quat_from_to(v3(0, 1, 0), v3(0, 0, 1)); /* nose along +Z */
	trs_quat qb = quat_mul(quat_axis_angle(v3(1, 0, 0), tilt), quat_from_to(v3(0, 1, 0), v3(0, 0, -1)));
	*a = trs_world_add_craft(w, ca, floe, r0, v0, qa);
	trs_vessel *A = &w->vessels[*a];
	/* put B's port face `gap` beyond A's along +Z */
	double ha = 0, hb = 0;
	for (int i = 0; i < ca->count; i++)
		if (trs_craft_def(ca, i)->is_dock)
			ha = ca->parts[i].pos.y + 0.15 - A->cm.y;
	*b = trs_world_add_craft(w, cb, floe, r0, v0, qb);
	trs_vessel *B = &w->vessels[*b];
	for (int i = 0; i < cb->count; i++)
		if (trs_craft_def(cb, i)->is_dock)
			hb = cb->parts[i].pos.y + 0.15 - B->cm.y;
	B->r = v3_add(r0, v3(0, 0, ha + gap + hb));
	B->v = v3_add(v0, v3(0, 0, -speed));
	A->sas = B->sas = TRS_SAS_OFF;
}

trs_gate_status trs_gate_docking(trs_log *log, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	if (!trs_gate_load_catalog(log, data_dir, &cat) || !load_floe_system(log, data_dir, &sys))
		return TRS_GATE_FAIL;
	int floe = trs_system_find(&sys, "floe");
	int failed = 0, pa, pb;
	trs_craft ca, cb;
	dock_craft(&ca, &cat, "Chaser", &pa);
	dock_craft(&cb, &cat, "Station", &pb);
	/* the chaser's own tank is dry: after docking the station's feeds it */
	for (int i = 0; i < ca.count; i++)
		if (trs_craft_def(&ca, i)->fuel_liquid > 0)
			ca.parts[i].fuel = 0;

	/* 1. Closing at 0.3 m/s, lined up: they latch into one vessel with
	 *    twice the parts; momentum and angular momentum (about the pair's
	 *    CoM) are kept through the latch; fuel flows across. */
	{
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int a, b;
		dock_pair(w, &ca, &cb, floe, 1.0, 0.3, 0, &a, &b);
		w->vessels[b].w = v3(0.01, 0.002, 0); /* a little tumble to carry over */
		double t_dock = -1, t0 = w->t;
		trs_v3 P0, L0, P1, L1;
		int n0 = ca.count + cb.count;
		for (int s = 0; s < 50 * 30 && t_dock < 0; s++) {
			trs_world_step_physics(w);
			for (int e = 0; e < w->nevents; e++)
				if (w->events[e].kind == TRS_WEV_DOCKED)
					t_dock = w->t - t0;
			w->nevents = 0;
		}
		const trs_vessel *Mv = &w->vessels[a];
		int alive_b = w->vessels[b].alive;
		trs_fuel_net net;
		trs_fuel_net_build(&net, Mv->craft);
		int eng = -1;
		for (int i = 0; i < Mv->craft->count && eng < 0; i++)
			if (trs_craft_def(Mv->craft, i)->propellant == TRS_PROP_LIQUID)
				eng = i; /* the chaser's engine comes first */
		int fed = eng >= 0 && trs_craft_engine_fed(Mv->craft, &net, eng);
		trs_fuel_net_free(&net);
		/* the latch alone, on the same pair placed in range: momentum about
		 * the pair's CoM (in its frame) before and after */
		{
			trs_world *w2 = malloc(sizeof(trs_world));
			trs_world_init(w2, &sys, 0);
			int a2, b2;
			dock_pair(w2, &ca, &cb, floe, 0.2, 0.3, 0, &a2, &b2);
			w2->vessels[b2].w = v3(0.01, 0.002, 0);
			const trs_vessel *A = &w2->vessels[a2], *B = &w2->vessels[b2];
			double M = A->mass + B->mass;
			trs_v3 R = v3_scale(v3_add(v3_scale(A->r, A->mass), v3_scale(B->r, B->mass)), 1.0 / M);
			trs_v3 V = v3_scale(v3_add(v3_scale(A->v, A->mass), v3_scale(B->v, B->mass)), 1.0 / M);
			pair_momentum(w2, a2, b2, R, V, &P0, &L0);
			int ia = -1, ib = -1;
			for (int i = 0; i < A->craft->count; i++)
				if (trs_craft_def(A->craft, i)->is_dock)
					ia = i;
			for (int i = 0; i < B->craft->count; i++)
				if (trs_craft_def(B->craft, i)->is_dock)
					ib = i;
			trs_world_dock(w2, a2, ia, b2, ib);
			const trs_vessel *J = &w2->vessels[a2];
			pair_momentum(w2, a2, -1, J->r, V, &P1, &L1);
			trs_world_free(w2);
			free(w2);
		}
		double dP = v3_len(v3_sub(P1, P0)) / v3_len(P0);
		double dL = v3_len(v3_sub(L1, L0)) / fmax(v3_len(L0), 1e-9);
		int ok = t_dock > 2 && t_dock < 3 && !alive_b && Mv->craft->count == n0 && dP < 1e-12 && dL < 1e-9 && fed;
		failed += !ok;
		trs_log_printf(log, "%s latched after %.2f s (1 m gap at 0.3 m/s): 1 vessel of %d parts, momentum kept to %.1e, spin to %.1e, chaser's engine fed by the station %s\n",
				ok ? "ok  " : "FAIL", t_dock, Mv->craft->count, dP, dL, fed ? "yes" : "no");

		/* 2. Undocking: two vessels again, named as before, pushed apart at
		 *    J (1/mA + 1/mB); they don't latch again until 2 m apart. */
		int port = -1;
		for (int i = 0; i < Mv->craft->count; i++)
			if (trs_craft_def(Mv->craft, i)->is_dock)
				port = i;
		double ma = 0, mb = 0;
		trs_craft_mass_props(&ca, &ma, NULL, NULL);
		trs_craft_mass_props(&cb, &mb, NULL, NULL);
		trs_v3 vbefore = w->vessels[a].v;
		int nid = trs_world_undock(w, a, port);
		int ok2 = nid >= 0;
		double sep = 0, want = 0;
		if (ok2) {
			const trs_vessel *A2 = &w->vessels[a], *B2 = &w->vessels[nid];
			/* the push, after the rigid spin both pieces carry away */
			trs_v3 spin = v3_cross(w->vessels[a].w, v3_sub(B2->r, A2->r));
			sep = v3_len(v3_sub(v3_sub(B2->v, A2->v), spin));
			want = trs_craft_def(A2->craft, 1)->dock_impulse * (1 / A2->mass + 1 / B2->mass);
			ok2 = !strcmp(A2->craft->name, "Chaser") && !strcmp(B2->craft->name, "Station") && A2->craft->count == ca.count
					&& B2->craft->count == cb.count && fabs(sep / want - 1) < 1e-6;
			(void)vbefore;
		}
		/* they drift apart; no re-latch on the way */
		int relatched = 0;
		for (int s = 0; s < 50 * 20; s++) {
			trs_world_step_physics(w);
			for (int e = 0; e < w->nevents; e++)
				relatched |= w->events[e].kind == TRS_WEV_DOCKED;
			w->nevents = 0;
		}
		int rearmed = 1;
		for (int i = 0; ok2 && i < w->vessels[a].craft->count; i++)
			if (w->vessels[a].craft->parts[i].dock_wait_uid)
				rearmed = 0;
		ok2 = ok2 && !relatched && rearmed;
		failed += !ok2;
		trs_log_printf(log, "%s undocked: 'Chaser' and 'Station' again, parting at %.4f m/s (J(1/mA+1/mB) = %.4f), no re-latch while close, re-armed past 2 m %s\n",
				ok2 ? "ok  " : "FAIL", sep, want, rearmed ? "yes" : "no");
		trs_world_free(w);
		free(w);
	}
	/* 3. Too fast, or too far off face to face: no latch while passing
	 *    through the capture range (what happens when they then hit is the
	 *    collision model's business). */
	struct { const char *what; double speed, tilt; } bad[] = { { "closing at 2 m/s", 2.0, 0 }, { "25 deg off", 0.3, 25 * TRS_PI / 180 } };
	for (int k = 0; k < 2; k++) {
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int a, b, docked = 0;
		dock_pair(w, &ca, &cb, floe, 1.0, bad[k].speed, bad[k].tilt, &a, &b);
		/* until the faces would touch: 1 m at the closing speed */
		for (int s = 0; s < (int)(50 * 1.0 / bad[k].speed) - 2; s++) {
			trs_world_step_physics(w);
			for (int e = 0; e < w->nevents; e++)
				docked |= w->events[e].kind == TRS_WEV_DOCKED;
			w->nevents = 0;
		}
		failed += docked;
		trs_log_printf(log, "%s %s: no latch through the capture range\n", docked ? "FAIL" : "ok  ", bad[k].what);
		trs_world_free(w);
		free(w);
	}
	trs_craft_free(&ca);
	trs_craft_free(&cb);
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
