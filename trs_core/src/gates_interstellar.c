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

/* test-interstellar: a ship coasting out past Austra's heliopause goes
 * between the stars, its proper velocity the gamma v of the speed it had;
 * inside a star's heliopause the Stillwater Drive pushes at most 1 g and
 * not past 1% of light speed, unfelt, and on rails (time warp) just as in
 * physics; out between the stars it flies
 * Austra to Lantern at 100 g, turning halfway, and stops where the closed
 * form puts it on both clocks, warped by its own clock as the game does;
 * the world save keeps it; and nudged on it crosses Lantern's heliopause
 * into ordinary flight there, the same place in the galaxy. */
#include "trs/gate.h"
#include "trs/relativity.h"
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int trs_gate_load_catalog(trs_log *out, const char *data_dir, trs_part_catalog *cat);

static void ship(trs_craft *c, const trs_part_catalog *cat) {
	trs_craft_init(c, cat);
	int pi = trs_catalog_find(cat, "probe_core"), hi = trs_catalog_find(cat, "hearth_core"), di = trs_catalog_find(cat, "stillwater_drive");
	const trs_part_def *pd = &cat->defs[pi], *hd = &cat->defs[hi], *dd = &cat->defs[di];
	trs_craft_add(c, hi, -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	trs_craft_add(c, pi, 0, "bottom", "top", v3(0, hd->height * 0.5 + pd->height * 0.5, 0), quat_identity(), -1, 0);
	trs_craft_add(c, di, 0, "top", "bottom", v3(0, -hd->height * 0.5 - dd->height * 0.5, 0), quat_identity(), -1, 0);
}

static int saw(const trs_world *w, trs_world_event_kind k, int vessel) {
	for (int i = 0; i < w->nevents; i++)
		if (w->events[i].kind == k && w->events[i].vessel == vessel)
			return 1;
	return 0;
}

trs_gate_status trs_gate_interstellar(trs_log *out, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	char path[1024], err[256];
	if (!trs_gate_load_catalog(out, data_dir, &cat))
		return TRS_GATE_FAIL;
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_system_load(&sys, path, err, sizeof err)) {
		trs_log_printf(out, "cannot load system: %s\n", err);
		return TRS_GATE_FAIL;
	}
	int austra = trs_system_find(&sys, "austra"), lantern = trs_system_find(&sys, "lantern"), floe = trs_system_find(&sys, "floe");
	if (lantern < 0) {
		trs_log_printf(out, "no Lantern in the galaxy\n");
		return TRS_GATE_FAIL;
	}
	const trs_body *A = &sys.bodies[austra], *L = &sys.bodies[lantern];
	trs_v3 to_l = v3_norm(v3_sub(L->galactic, A->galactic));
	trs_craft c;
	ship(&c, &cat);
	int failed = 0;
	trs_world *w = malloc(sizeof(trs_world));

	/* 1. coasting out past the heliopause, on rails */
	{
		trs_world_init(w, &sys, 0);
		double v0 = 30000;
		int id = trs_world_add_craft(w, &c, austra, v3_scale(to_l, A->soi - 1e7), v3_scale(to_l, v0), quat_identity());
		trs_vessel_set_mode(w, id, TRS_MODE_RAILS);
		trs_event ev;
		for (int k = 0; k < 20; k++)
			trs_world_warp(w, 3600, -1, &ev);
		trs_vessel *vs = &w->vessels[id];
		double g = 1.0 / sqrt(1 - v3_len2(vs->v) / (TRS_C * TRS_C));
		int ok = vs->mode == TRS_MODE_INTERSTELLAR && saw(w, TRS_WEV_INTERSTELLAR, id) && fabs(v3_len(vs->u) - g * v3_len(vs->v)) < 1e-9 &&
			 v3_len(vs->r) > A->soi;
		failed += !ok;
		trs_log_printf(out, "%s coasting out at %.1f km/s: between the stars past %.0f Gm (mode %d), proper velocity %.6f km/s = gamma v\n",
				ok ? "ok  " : "FAIL", v3_len(vs->v) / 1000, A->soi / 1e9, vs->mode, v3_len(vs->u) / 1000);
		trs_world_free(w);
	}
	/* 2. inside Austra's heliopause: 1 g, unfelt; nothing past 1% of c */
	{
		trs_world_init(w, &sys, 0);
		const trs_body *F = &sys.bodies[floe];
		trs_v3 fr, fv;
		trs_body_state_abs(&sys, floe, 0, &fr, &fv);
		trs_v3 r = v3_add(fr, v3(0, 0, F->soi * 3)); /* well clear of Floe, around Austra */
		int a = trs_world_add_craft(w, &c, austra, r, fv, quat_identity());
		int b = trs_world_add_craft(w, &c, austra, v3_add(r, v3(1e6, 0, 0)), fv, quat_identity());
		trs_world_set_drive(w, a, 1.0);
		trs_v3 va0 = w->vessels[a].v, vb0 = w->vessels[b].v;
		for (int k = 0; k < 50; k++)
			trs_world_step_physics(w);
		trs_v3 dv = v3_sub(v3_sub(w->vessels[a].v, va0), v3_sub(w->vessels[b].v, vb0));
		double felt = v3_len(w->vessels[a].sensed_accel);
		trs_v3 nose = quat_rotate(w->vessels[a].q, v3(0, 1, 0));
		/* 3. at 1.01% of c along the nose, the drive adds nothing */
		trs_v3 fast = v3_add(fv, v3_scale(nose, 0.0101 * TRS_C));
		int f = trs_world_add_craft(w, &c, austra, v3_add(r, v3(2e6, 0, 0)), fast, quat_identity());
		int g = trs_world_add_craft(w, &c, austra, v3_add(r, v3(3e6, 0, 0)), fast, quat_identity());
		trs_world_set_drive(w, f, 1.0);
		trs_v3 vf0 = w->vessels[f].v, vg0 = w->vessels[g].v;
		for (int k = 0; k < 50; k++)
			trs_world_step_physics(w);
		double extra = v3_len(v3_sub(v3_sub(w->vessels[f].v, vf0), v3_sub(w->vessels[g].v, vg0)));
		int ok = fabs(v3_dot(dv, nose) - 9.80665) < 1e-6 && felt < 1e-9 && extra < 1e-6;
		failed += !ok;
		trs_log_printf(out, "%s inside the heliopause, full drive for 1 s: +%.6f m/s along the nose (1 g = 9.80665), felt %.2e m/s2; at 1.01%% of c: +%.2e m/s\n",
				ok ? "ok  " : "FAIL", v3_dot(dv, nose), felt, extra);
		trs_world_free(w);
	}
	/* 3b. the same push through warp: on rails the drive's path is
	 * stepped, and comes out where ten minutes of physics put it */
	{
		const trs_body *F = &sys.bodies[floe];
		trs_v3 fr, fv;
		trs_body_state_abs(&sys, floe, 0, &fr, &fv);
		trs_v3 r = v3_add(fr, v3(0, 0, F->soi * 3));
		trs_quat q = quat_from_to(v3(0, 1, 0), v3_norm(v3(1, 1, 0)));
		trs_v3 rp, vp, rr, vr;
		trs_world_init(w, &sys, 0);
		int a = trs_world_add_craft(w, &c, austra, r, fv, q);
		trs_world_set_drive(w, a, 1.0);
		for (int k = 0; k < 600 * TRS_PHYSICS_HZ; k++)
			trs_world_step_physics(w);
		rp = w->vessels[a].r, vp = w->vessels[a].v;
		trs_world_free(w);
		trs_world_init(w, &sys, 0);
		int b = trs_world_add_craft(w, &c, austra, r, fv, q);
		trs_world_set_drive(w, b, 1.0);
		trs_vessel_set_mode(w, b, TRS_MODE_RAILS);
		trs_event ev;
		trs_world_warp(w, 600, b, &ev);
		trs_vessel_state(w, b, &rr, &vr);
		trs_v3 moved = v3_sub(rp, r);
		double dr = v3_len(v3_sub(rr, rp)), dvv = v3_len(v3_sub(vr, vp));
		int ok = dr < 50 && dvv < 0.05 && v3_len(moved) > 1.5e6;
		failed += !ok;
		trs_log_printf(out, "%s full drive for 10 min on rails (warp) against in physics: %.0f km on, %.2f m and %.4f m/s apart\n",
				ok ? "ok  " : "FAIL", v3_len(moved) / 1000, dr, dvv);
		trs_world_free(w);
	}
	/* 4. Austra to Lantern at 100 g, warped by the ship's clock */
	{
		trs_world_init(w, &sys, 0);
		trs_v3 start = v3_scale(to_l, A->soi * 1.01);
		int id = trs_world_add_craft(w, &c, austra, start, v3(0, 0, 0), quat_from_to(v3(0, 1, 0), to_l));
		trs_world_step_physics(w);
		trs_vessel *vs = &w->vessels[id];
		vs->w = v3(0, 0, 0);
		double t0 = w->t, d = v3_len(v3_sub(L->galactic, v3_add(vs->r, A->galactic))) - 1.05 * L->soi;
		double amax = trs_vessel_drive_max(vs, NULL), tau, t, vp;
		trs_rel_flip_burn(d, amax, &tau, &t, &vp);
		trs_v3 x0 = v3_add(vs->r, A->galactic);
		trs_world_set_drive(w, id, 1.0);
		int chunks = 400;
		double home = 0, peak = 0;
		for (int half = 0; half < 2; half++) {
			vs->q = quat_from_to(v3(0, 1, 0), half ? v3_neg(to_l) : to_l);
			for (int k = 0; k < chunks; k++) {
				home += trs_world_warp_ship(w, id, tau / 2 / chunks);
				peak = fmax(peak, v3_len(vs->v));
			}
		}
		trs_world_set_drive(w, id, 0);
		double gone = v3_len(v3_sub(v3_add(vs->r, sys.bodies[vs->body].galactic), x0));
		double ship_t = (w->t - t0) - vs->tau_lag;
		int ok = vs->mode == TRS_MODE_INTERSTELLAR && fabs(gone - d) / d < 1e-8 && fabs((w->t - t0) - t) / t < 1e-8 && fabs(ship_t - tau) / tau < 1e-8 &&
			 v3_len(vs->v) < 50 && fabs(home - (w->t - t0)) < 1e-3 && fabs(peak - vp) / vp < 1e-6;
		failed += !ok;
		trs_log_printf(out, "%s Austra to Lantern at %.0f g: %.4f ly in %.3f ship days (want %.3f) and %.5f home years (want %.5f), peak %.8f c (want %.8f); "
				"stops %.3e of the way off at %.2f m/s\n", ok ? "ok  " : "FAIL", amax / 9.80665, d / TRS_LIGHT_YEAR, ship_t / 86400, tau / 86400,
				(w->t - t0) / TRS_YEAR, t / TRS_YEAR, peak / TRS_C, vp / TRS_C, fabs(gone - d) / d, v3_len(vs->v));

		/* 5. through the world save */
		trs_json *j = trs_world_to_json(w);
		char *text = trs_json_write(j);
		trs_json_free(j);
		trs_world *b = malloc(sizeof(trs_world));
		trs_world_init(b, &sys, 0);
		trs_json *j2 = trs_json_parse(text, err, sizeof err);
		int loaded = j2 && trs_world_from_json(b, &cat, j2, err, sizeof err);
		trs_json_free(j2);
		const trs_vessel *bv = &b->vessels[id];
		ok = loaded && bv->mode == TRS_MODE_INTERSTELLAR && bv->tau_lag == vs->tau_lag && bv->u.x == vs->u.x && bv->r.y == vs->r.y;
		failed += !ok;
		trs_log_printf(out, "%s through the world save: between the stars, ship clock %.6f yr behind home\n", ok ? "ok  " : "FAIL", bv->tau_lag / TRS_YEAR);
		free(text);
		trs_world_free(b);
		free(b);

		/* 6. drifting on in through Lantern's heliopause */
		trs_v3 gal0 = v3_add(vs->r, sys.bodies[vs->body].galactic);
		vs->u = v3_scale(to_l, 2e5);
		vs->v = trs_rel_velocity(vs->u);
		double need = 0.06 * L->soi / v3_len(vs->v);
		trs_event ev;
		trs_world_warp(w, need, -1, &ev);
		trs_v3 gal1 = v3_add(vs->r, sys.bodies[vs->body].galactic), want = v3_add(gal0, v3_scale(vs->v, need));
		ok = vs->body == lantern && vs->mode == TRS_MODE_PHYSICS && v3_len(vs->r) < L->soi && v3_len(v3_sub(gal1, want)) < 1e-6 * v3_len(want);
		failed += !ok;
		trs_log_printf(out, "%s drifting on at 200 km/s: in Lantern's heliopause (%s, %.3f Gm from the star), %.0f m from where it was headed\n",
				ok ? "ok  " : "FAIL", sys.bodies[vs->body].name, v3_len(vs->r) / 1e9, v3_len(v3_sub(gal1, want)));
		trs_world_free(w);
	}
	/* 7. a course to Lantern, flown by warping in big steps of ship time:
	 *    from rest, and from 0.1 c along the way */
	for (int k = 0; k < 2; k++) {
		trs_world_init(w, &sys, 0);
		trs_v3 start = v3_scale(to_l, A->soi * 1.01);
		int id = trs_world_add_craft(w, &c, austra, start, v3_scale(to_l, k ? 0.1 * TRS_C / sqrt(1 - 0.01) * sqrt(1 - 0.01) : 0), quat_identity());
		trs_world_step_physics(w);
		trs_vessel *vs = &w->vessels[id];
		double t0 = w->t, lag0 = vs->tau_lag, tau = 0, t = 0;
		int set = trs_world_set_course(w, id, lantern, &tau, &t);
		trs_v3 aim = v3_sub(L->galactic, v3_scale(vs->course_dir, 1.05 * L->soi));
		int steps = 0;
		while (vs->course && steps < 1000) {
			trs_world_warp_ship(w, id, 1.0e6);
			steps++;
		}
		trs_v3 here = v3_add(vs->r, sys.bodies[vs->body].galactic);
		double off = v3_len(v3_sub(here, aim)), home = w->t - t0, ship_t = home - (vs->tau_lag - lag0);
		int ok = set && !vs->course && vs->drive == 0 && vs->mode == TRS_MODE_INTERSTELLAR && off < 1e-8 * v3_len(aim) && v3_len(vs->v) < 1.0 &&
			 fabs(ship_t - tau) < 1e-6 * tau && fabs(home - t) < 1e-6 * t;
		failed += !ok;
		trs_log_printf(out, "%s course to Lantern from %s, in %d warps of 11.6 ship days: %.3f ship days (planned %.3f), %.5f home years (planned %.5f); "
				"stops %.0f km from the aim point at %.4f m/s\n", ok ? "ok  " : "FAIL", k ? "0.1 c" : "rest", steps, ship_t / 86400, tau / 86400,
				home / TRS_YEAR, t / TRS_YEAR, off / 1000, v3_len(vs->v));
		trs_world_free(w);
	}
	trs_craft_free(&c);
	free(w);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
