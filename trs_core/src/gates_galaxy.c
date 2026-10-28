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
/* test-galaxy: the stars past Lantern hold together, and what the game
 * says about them is true. Every star the home file lists is loaded, each
 * the distance from the others that the words say (the mission stories
 * and the systems' notes); every world's orbit lies inside its parent's
 * sphere of influence and clear of its siblings'; a locked world keeps
 * the longitude its data names turned to its parent all the way round;
 * Maelstrom has no ground (nothing can be set on it) and crushes a craft
 * that sinks past its crush depth while one above it lives; its rings are
 * wider than Floe; the water worlds' islands stand out of their seas as
 * the words say; and a course from each star on the thread to the next
 * arrives. */
#include "trs/gate.h"
#include "trs/json.h"
#include "trs/relativity.h"
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int trs_gate_load_catalog(trs_log *out, const char *data_dir, trs_part_catalog *cat);

#define LY 9.4607304725808e15

static double ly(const trs_system *s, const char *a, const char *b) {
	int i = trs_system_find(s, a), j = trs_system_find(s, b);
	if (i < 0 || j < 0)
		return -1;
	return v3_len(v3_sub(s->bodies[i].galactic, s->bodies[j].galactic)) / LY;
}

/* a body's entry in the data files (for what only the game reads: its look) */
static const trs_json *entry(trs_json **files, int nfiles, const char *id) {
	for (int f = 0; f < nfiles; f++) {
		const trs_json *bl = trs_json_get(files[f], "bodies");
		for (size_t k = 0; bl && k < bl->count; k++)
			if (!strcmp(trs_json_str(trs_json_at(bl, k), "id", ""), id))
				return trs_json_at(bl, k);
	}
	return NULL;
}

static double height_at(const trs_body *b, double lat_deg, double lon_deg) {
	double la = lat_deg * TRS_PI / 180, lo = lon_deg * TRS_PI / 180;
	return trs_terrain_height(&b->terrain, b->radius, v3(cos(la) * cos(lo), cos(la) * sin(lo), sin(la)));
}

/* the mean ground height on a ring `frac` of a landmark's radius out */
static double ring_mean(const trs_body *b, const trs_json *lm, double frac) {
	double la = trs_json_num(lm, "lat_deg", 0) * TRS_PI / 180, lo = trs_json_num(lm, "lon_deg", 0) * TRS_PI / 180;
	trs_v3 d = v3(cos(la) * cos(lo), cos(la) * sin(lo), sin(la));
	trs_v3 e = v3_norm(v3_cross(v3(0, 0, 1), d)), n = v3_cross(d, e);
	double ang = trs_json_num(lm, "radius_km", 10) * 1000 * frac / b->radius, sum = 0;
	for (int i = 0; i < 32; i++) {
		double a = i * TRS_TAU / 32;
		trs_v3 t = v3_add(v3_scale(e, cos(a)), v3_scale(n, sin(a)));
		sum += trs_terrain_height(&b->terrain, b->radius, v3_norm(v3_add(v3_scale(d, cos(ang)), v3_scale(t, sin(ang)))));
	}
	return sum / 32;
}

trs_gate_status trs_gate_galaxy(trs_log *out, const char *data_dir) {
	trs_system *sys = malloc(sizeof *sys);
	char path[1024], err[256];
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_system_load(sys, path, err, sizeof err)) {
		trs_log_printf(out, "cannot load the galaxy: %s\n", err);
		free(sys);
		return TRS_GATE_FAIL;
	}
	int failed = 0;
	/* the files themselves, for the looks */
	trs_json *files[16];
	int nfiles = 0;
	files[nfiles++] = trs_json_load_file(path, err, sizeof err);
	const trs_json *nb = trs_json_get(files[0], "neighbours");
	for (size_t k = 0; nb && k < nb->count && nfiles < 16; k++) {
		snprintf(path, sizeof path, "%s/bodies/%s", data_dir, trs_json_at(nb, k)->str);
		files[nfiles] = trs_json_load_file(path, err, sizeof err);
		if (files[nfiles])
			nfiles++;
	}

	/* 1. the stars, and how far apart the words say they are */
	int stars = 0;
	for (int i = 0; i < sys->count; i++)
		stars += sys->bodies[i].parent < 0;
	{
		int ok = stars >= 7 && sys->count >= 27;
		failed += !ok;
		trs_log_printf(out, "%s %d stars, %d bodies\n", ok ? "ok  " : "FAIL", stars, sys->count);
	}
	static const struct { const char *a, *b; double said, tol; const char *where; } claim[] = {
		{ "austra", "lantern", 4.2, 0.05, "To Lantern" },
		{ "lantern", "ember", 3.6, 0.05, "the Archive" },
		{ "austra", "ember", 6.7, 0.05, "Ember's note" },
		{ "ember", "brine", 5.0, 0.1, "the Lamp" },
		{ "ember", "marrow", 6.5, 0.1, "the Lamp" },
		{ "austra", "brine", 10.5, 0.05, "Brine's note" },
		{ "austra", "marrow", 8.8, 0.05, "Marrow's note" },
		{ "lantern", "marrow", 5.4, 0.05, "Marrow's note" },
		{ "brine", "tarn", 4.5, 0.05, "the Anchor" },
		{ "austra", "tarn", 13.0, 0.1, "Tarn's note" },
		{ "tarn", "hollow", 6.0, 0.15, "the Garden" },
		{ "austra", "hollow", 17.5, 0.05, "Hollow's note" },
	};
	for (size_t k = 0; k < sizeof claim / sizeof claim[0]; k++) {
		double d = ly(sys, claim[k].a, claim[k].b);
		int ok = d > 0 && fabs(d - claim[k].said) <= claim[k].tol;
		failed += !ok;
		trs_log_printf(out, "%s %s to %s: %.3f ly (%s says %.1f)\n", ok ? "ok  " : "FAIL", claim[k].a, claim[k].b, d, claim[k].where, claim[k].said);
	}

	/* 2. every orbit inside its parent's sphere, clear of its siblings' */
	{
		int bad = 0, n = 0;
		for (int i = 0; i < sys->count; i++) {
			const trs_body *b = &sys->bodies[i];
			if (b->parent < 0)
				continue;
			const trs_body *p = &sys->bodies[b->parent];
			double ap = b->orbit.a * (1 + b->orbit.e), pe = b->orbit.a * (1 - b->orbit.e);
			n++;
			if (ap + b->soi > p->soi || pe - b->soi < p->radius * 1.5 || b->soi < b->radius * 2) {
				trs_log_printf(out, "     %s: apoapsis %.3g + soi %.3g vs %s's %.3g, periapsis %.3g\n", b->id, ap, b->soi, p->id, p->soi, pe);
				bad++;
			}
			for (int j = i + 1; j < sys->count; j++) {
				const trs_body *c = &sys->bodies[j];
				if (c->parent != b->parent)
					continue;
				double gap = fabs(c->orbit.a - b->orbit.a) - b->orbit.a * b->orbit.e - c->orbit.a * c->orbit.e;
				if (gap < b->soi + c->soi) {
					trs_log_printf(out, "     %s and %s: orbits %.3g apart, spheres %.3g + %.3g\n", b->id, c->id, gap, b->soi, c->soi);
					bad++;
				}
			}
		}
		failed += bad > 0;
		trs_log_printf(out, "%s %d orbits: each inside its parent's sphere of influence and clear of its siblings'\n", bad ? "FAIL" : "ok  ", n);
	}

	/* 3. locked worlds keep their named face to the parent */
	for (int f = 0; f < nfiles; f++) {
		const trs_json *bl = trs_json_get(files[f], "bodies");
		for (size_t k = 0; bl && k < bl->count; k++) {
			const trs_json *e = trs_json_at(bl, k);
			const trs_json *dl = trs_json_get(e, "day_lon_deg");
			if (!dl)
				continue;
			int i = trs_system_find(sys, trs_json_str(e, "id", ""));
			const trs_body *b = &sys->bodies[i];
			double worst = 0;
			for (int q = 0; q < 24; q++) {
				double t = b->orbit.period * q / 24.0 * 3.7;
				trs_v3 r, v;
				trs_body_state(sys, i, t, &r, &v);
				trs_v3 to = trs_body_to_fixed(b, t, v3_scale(r, -1));
				double lon = atan2(to.y, to.x) * 180 / TRS_PI - dl->num;
				lon = fabs(fmod(lon + 540.0, 360.0) - 180.0);
				worst = fmax(worst, lon);
			}
			/* (an orbit with any eccentricity rocks a little: libration) */
			double allow = 2.2 * b->orbit.e * 180 / TRS_PI + 1e-6;
			int ok = worst <= allow;
			failed += !ok;
			trs_log_printf(out, "%s %s keeps %.0f deg east turned to %s: off by at most %.4f deg over its orbits (e %.3f allows %.4f)\n",
					ok ? "ok  " : "FAIL", b->name, dl->num, sys->bodies[b->parent].name, worst, b->orbit.e, allow);
		}
	}

	/* 4. a gas giant: no ground, and its depths crush */
	int mi = trs_system_find(sys, "maelstrom");
	trs_part_catalog cat;
	if (mi < 0 || !trs_gate_load_catalog(out, data_dir, &cat)) {
		failed++;
		trs_log_printf(out, "FAIL no Maelstrom (or no parts)\n");
	} else {
		const trs_body *M = &sys->bodies[mi];
		trs_craft c;
		trs_craft_init(&c, &cat);
		trs_craft_add(&c, trs_catalog_find(&cat, "probe_core"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, sys, 0);
		int set = trs_world_launch(w, &c, mi, 0, 0);
		double p_top, rho_top, p_deep, rho_deep;
		trs_atmosphere_at(M, 0, &p_top, &rho_top);
		trs_atmosphere_at(M, -M->crush_depth * 0.5, &p_deep, &rho_deep);
		/* one riding with the clouds just above its crush depth, one just below */
		int live[2];
		double depth[2] = { -M->crush_depth + 2000, -M->crush_depth - 2000 };
		for (int k = 0; k < 2; k++) {
			trs_v3 r = v3(M->radius + depth[k], 0, 0);
			live[k] = trs_world_add_craft(w, &c, mi, r, v3_cross(trs_body_omega(M), r), quat_identity());
		}
		int s0 = w->vessels[live[0]].serial, s1 = w->vessels[live[1]].serial;
		for (int k = 0; k < 10; k++)
			trs_world_step_physics(w);
		int above = w->vessels[live[0]].alive && w->vessels[live[0]].serial == s0;
		int below = w->vessels[live[1]].alive && w->vessels[live[1]].serial == s1 && w->vessels[live[1]].craft && w->vessels[live[1]].craft->count > 0;
		int ok = set < 0 && rho_deep > rho_top * 2 && above && !below;
		failed += !ok;
		trs_log_printf(out, "%s Maelstrom: nothing can be set on its clouds (%d); %.0f km down the air is %.1fx as dense as at the cloud tops; "
				"%.0f km down a probe lives, %.0f km down it is crushed\n", ok ? "ok  " : "FAIL", set, M->crush_depth * 0.5 / 1000,
				rho_deep / rho_top, -depth[0] / 1000, -depth[1] / 1000);
		trs_world_free(w);
		free(w);
		trs_craft_free(&c);
		/* its rings: wider than Floe ("rings you could drop Floe through") */
		const trs_json *rg = trs_json_get(trs_json_get(entry(files, nfiles, "maelstrom"), "look"), "rings");
		double width = trs_json_num(rg, "outer", 0) - trs_json_num(rg, "inner", 0), floe = 2 * sys->bodies[trs_system_find(sys, "floe")].radius;
		ok = rg && width > floe && trs_json_num(rg, "inner", 0) > M->radius;
		failed += !ok;
		trs_log_printf(out, "%s Maelstrom's rings: %.0f km wide, Floe %.0f km across\n", ok ? "ok  " : "FAIL", width / 1000, floe / 1000);
		/* the storm: wider than Floe ("a storm wider than Floe") */
		const trs_json *lms = trs_json_get(entry(files, nfiles, "maelstrom"), "landmarks");
		double storm = lms ? 2 * trs_json_num(trs_json_at(lms, 0), "radius_km", 0) * 1000 : 0;
		ok = storm > floe;
		failed += !ok;
		trs_log_printf(out, "%s the Eye of Maelstrom: %.0f km across\n", ok ? "ok  " : "FAIL", storm / 1000);
	}

	/* 5. what the words say about the seas: islands out of them, lagoons in them */
	static const struct { const char *body, *lm; int kind; } sea[] = {
		{ "selkie", "mount_skerry", 0 },  /* an island: its top out, its foot under */
		{ "selkie", "the_ring_sea", 1 },  /* a ring of islands round a lagoon */
		{ "mere", "mount_brood", 0 },
		{ "mere", "the_atoll", 1 },
		{ "mere", "the_tableland", 0 },
	};
	for (size_t k = 0; k < sizeof sea / sizeof sea[0]; k++) {
		const trs_json *e = entry(files, nfiles, sea[k].body);
		const trs_body *b = &sys->bodies[trs_system_find(sys, sea[k].body)];
		double level = trs_json_num(trs_json_get(e, "look"), "sea_level", 0);
		const trs_json *lm = NULL, *ls = trs_json_get(e, "landmarks");
		for (size_t q = 0; ls && q < ls->count; q++)
			if (!strcmp(trs_json_str(trs_json_at(ls, q), "id", ""), sea[k].lm))
				lm = trs_json_at(ls, q);
		if (!lm) {
			failed++;
			trs_log_printf(out, "FAIL no %s on %s\n", sea[k].lm, sea[k].body);
			continue;
		}
		int ok;
		if (sea[k].kind == 0) {
			double top = height_at(b, trs_json_num(lm, "lat_deg", 0), trs_json_num(lm, "lon_deg", 0)), foot = ring_mean(b, lm, 1.6);
			ok = top > level + 300 && foot < level;
			trs_log_printf(out, "%s %s on %s is an island: %.0f m at its middle, %.0f m round its foot, the sea at %.0f m\n",
					ok ? "ok  " : "FAIL", trs_json_str(lm, "name", ""), b->name, top, foot, level);
		} else {
			double rim = ring_mean(b, lm, 1.0), floor = ring_mean(b, lm, 0.5);
			ok = rim > level && floor < level;
			trs_log_printf(out, "%s %s on %s: its rim at %.0f m stands out of the sea (%.0f m), its floor at %.0f m is under it\n",
					ok ? "ok  " : "FAIL", trs_json_str(lm, "name", ""), b->name, rim, level, floor);
		}
		failed += !ok;
	}
	/* the Anchor's light runs straight up to Maelstrom */
	{
		int si = trs_system_find(sys, "selkie");
		const trs_body *S = &sys->bodies[si];
		trs_v3 up = v3(0, 0, 1);
		for (int r = 0; r < S->nrelics; r++)
			if (!strcmp(S->relics[r].id, "selkie_anchor"))
				up = S->relics[r].dir;
		trs_v3 rr, vv;
		trs_body_state(sys, si, 12345.0, &rr, &vv);
		trs_v3 to = v3_norm(trs_body_to_fixed(S, 12345.0, v3_scale(rr, -1)));
		double off = acos(fmin(1.0, v3_dot(to, up))) * 180 / TRS_PI;
		int ok = off < 2.0;
		failed += !ok;
		trs_log_printf(out, "%s from the Anchor, Maelstrom stands %.2f deg from straight overhead\n", ok ? "ok  " : "FAIL", off);
	}

	/* 6. a course along each hop of the thread arrives */
	{
		trs_part_catalog cat2;
		if (!trs_gate_load_catalog(out, data_dir, &cat2)) {
			failed++;
		} else {
			trs_craft c;
			trs_craft_init(&c, &cat2);
			int pi = trs_catalog_find(&cat2, "probe_core"), hi = trs_catalog_find(&cat2, "hearth_core"), di = trs_catalog_find(&cat2, "stillwater_drive");
			const trs_part_def *pd = &cat2.defs[pi], *hd = &cat2.defs[hi], *dd = &cat2.defs[di];
			trs_craft_add(&c, hi, -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
			trs_craft_add(&c, pi, 0, "bottom", "top", v3(0, hd->height * 0.5 + pd->height * 0.5, 0), quat_identity(), -1, 0);
			trs_craft_add(&c, di, 0, "top", "bottom", v3(0, -hd->height * 0.5 - dd->height * 0.5, 0), quat_identity(), -1, 0);
			static const char *hop[][2] = { { "lantern", "ember" }, { "ember", "brine" }, { "ember", "marrow" }, { "brine", "tarn" }, { "tarn", "hollow" } };
			trs_world *w = malloc(sizeof(trs_world));
			for (size_t k = 0; k < sizeof hop / sizeof hop[0]; k++) {
				int a = trs_system_find(sys, hop[k][0]), b = trs_system_find(sys, hop[k][1]);
				const trs_body *A = &sys->bodies[a], *B = &sys->bodies[b];
				trs_world_init(w, sys, 0);
				trs_v3 dir = v3_norm(v3_sub(B->galactic, A->galactic));
				int id = trs_world_add_craft(w, &c, a, v3_scale(dir, A->soi * 1.01), v3(0, 0, 0), quat_identity());
				trs_world_step_physics(w);
				trs_vessel *vs = &w->vessels[id];
				double tau = 0, t = 0, t0 = w->t;
				int set = trs_world_set_course(w, id, b, &tau, &t);
				int steps = 0;
				while (vs->course && steps < 2000) {
					trs_world_warp_ship(w, id, 1.0e6);
					steps++;
				}
				trs_v3 here = v3_add(vs->r, sys->bodies[vs->body].galactic);
				double from_b = v3_len(v3_sub(here, B->galactic));
				int ok = set && !vs->course && from_b < B->soi * 1.1 && from_b > B->soi && v3_len(vs->v) < 1.0;
				failed += !ok;
				trs_log_printf(out, "%s course %s to %s: %.1f ship days, %.2f home years; stops %.2f heliopauses from %s\n", ok ? "ok  " : "FAIL",
						A->name, B->name, tau / 86400, (w->t - t0) / TRS_YEAR, from_b / B->soi, B->name);
				trs_world_free(w);
			}
			free(w);
			trs_craft_free(&c);
		}
	}
	for (int f = 0; f < nfiles; f++)
		trs_json_free(files[f]);
	free(sys);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
