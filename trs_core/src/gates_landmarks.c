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
/* test-landmarks: every named landmark is really in the ground -- a
 * volcano's rim stands high over the plain around it, a crater's floor
 * sits deep below its rim, a canyon cuts down across its line, a mesa's
 * top stands over its foot -- on every world of both systems, each by at
 * least half of what its data says. */
#include "trs/gate.h"
#include "trs/system.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* the direction `a` metres from d along the tangent t */
static trs_v3 along(trs_v3 d, trs_v3 t, double a, double R) {
	double ang = a / R;
	return v3_norm(v3_add(v3_scale(d, cos(ang)), v3_scale(t, sin(ang))));
}

static int check_body(trs_log *out, const trs_body *b) {
	int ok = 1;
	const trs_terrain *tr = &b->terrain;
	double R = b->radius;
	for (int k = 0; k < tr->nfeatures; k++) {
		const struct trs_feature *f = &tr->feature[k];
		double have = 0, want = 0;
		const char *what = "?";
		if (f->kind == TRS_FEAT_VOLCANO) {
			/* the highest of the rim, around, over the plain past its foot */
			double rim = -1e9, plain = 0;
			for (int i = 0; i < 16; i++) {
				trs_v3 t = v3_norm(v3_add(v3_scale(f->t, cos(i * TRS_PI / 8)), v3_scale(f->g, sin(i * TRS_PI / 8))));
				rim = fmax(rim, trs_terrain_height(tr, R, along(f->dir, t, f->radius * 0.14, R)));
				plain += trs_terrain_height(tr, R, along(f->dir, t, f->radius * 1.6, R)) / 16;
			}
			have = rim - plain, want = f->height * 0.5, what = "volcano rim over the plain";
		} else if (f->kind == TRS_FEAT_CRATER) {
			double rim = 0, floor = 0;
			for (int i = 0; i < 16; i++) {
				trs_v3 t = v3_norm(v3_add(v3_scale(f->t, cos(i * TRS_PI / 8)), v3_scale(f->g, sin(i * TRS_PI / 8))));
				rim += trs_terrain_height(tr, R, along(f->dir, t, f->radius * 1.0, R)) / 16;
				floor += trs_terrain_height(tr, R, along(f->dir, t, f->radius * 0.5, R)) / 16;
			}
			have = rim - floor, want = f->depth * 0.5, what = "crater rim over its floor";
		} else if (f->kind == TRS_FEAT_CANYON) {
			/* across it at its middle (it wanders: the lowest and the highest) */
			double lo = 1e9, hi = -1e9;
			for (int i = -60; i <= 60; i++) {
				double h = trs_terrain_height(tr, R, along(f->dir, f->g, f->width * 3.0 * i / 60.0, R));
				lo = fmin(lo, h), hi = fmax(hi, h);
			}
			have = hi - lo, want = f->depth * 0.5, what = "canyon's walls over its floor";
		} else if (f->kind == TRS_FEAT_MESA) {
			double top = 0, foot = 0;
			for (int i = 0; i < 16; i++) {
				trs_v3 t = v3_norm(v3_add(v3_scale(f->t, cos(i * TRS_PI / 8)), v3_scale(f->g, sin(i * TRS_PI / 8))));
				top += trs_terrain_height(tr, R, along(f->dir, t, f->radius * 0.3, R)) / 16;
				foot += trs_terrain_height(tr, R, along(f->dir, t, f->radius * 1.5, R)) / 16;
			}
			have = top - foot, want = f->height * 0.5, what = "mesa's top over its foot";
		} else if (f->kind == TRS_FEAT_GEYSER) {
			/* the mound round the vent over the vent's own floor */
			double ring = 0;
			for (int i = 0; i < 16; i++) {
				trs_v3 t = v3_norm(v3_add(v3_scale(f->t, cos(i * TRS_PI / 8)), v3_scale(f->g, sin(i * TRS_PI / 8))));
				ring += trs_terrain_height(tr, R, along(f->dir, t, f->radius * 0.3, R)) / 16;
			}
			have = ring - trs_terrain_height(tr, R, f->dir), want = f->depth * 0.5, what = "geyser's mound over its vent";
		}
		int good = have >= want;
		trs_log_printf(out, "  %s %d: %s %.0f m (at least %.0f)%s\n", b->id, k, what, have, want, good ? "" : "  FAIL");
		ok = ok && good;
	}
	return ok;
}

/* The close-up ground (crags, gullies, terraces) is rough, and still a
 * lander finds level ground: at spots spread over the body, the nearest
 * within 250 m tilted under 14 degrees (what the autopilot's land looks
 * for); and somewhere among them a face steeper than 45 degrees. */
static int check_close_ground(trs_log *out, const trs_body *b) {
	const trs_terrain *t = &b->terrain;
	if (t->crags_amplitude <= 0 && t->gullies_depth <= 0 && t->terraces_step <= 0)
		return 1;
	int n = 0, found = 0;
	double far = 0, steep = 0;
	for (int k = 0; k < 80; k++) {
		/* (spread evenly: a Fibonacci sphere) */
		double z = 1.0 - (k + 0.5) / 40.0, r = sqrt(fmax(0.0, 1.0 - z * z)), ph = k * 2.39996323;
		trs_v3 d = v3(r * cos(ph), r * sin(ph), z), at;
		double sl;
		double dist = trs_terrain_level_spot(t, b->radius, d, 250.0, 0.03, &at, &sl);
		n++;
		if (dist >= 0) {
			found++;
			far = fmax(far, dist);
		}
		/* the steepest ground round the spot, over the 4 m a lander spans */
		for (int q = 0; q < 24; q++) {
			trs_v3 e = v3_norm(v3_add(d, v3((q % 5 - 2) * 30.0 / b->radius, (q / 5 - 2) * 30.0 / b->radius, 0)));
			steep = fmax(steep, 1.0 - v3_dot(trs_terrain_normal(t, b->radius, e, 4.0), e));
		}
	}
	int good = found >= n * 95 / 100 && steep > 1.0 - cos(45.0 * TRS_PI / 180.0);
	trs_log_printf(out, "  %s: level ground within 250 m at %d of %d spots (farthest %.0f m); steepest face seen %.0f deg%s\n", b->id, found, n,
		far, acos(1.0 - steep) * 180.0 / TRS_PI, good ? "" : "  FAIL");
	return good;
}

trs_gate_status trs_gate_landmarks(trs_log *out, const char *data_dir) {
	const char *files[] = { "austra_system.json" };   /* (it loads its neighbours: every other star too) */
	int ok = 1, n = 0;
	for (int fi = 0; fi < 1; fi++) {
		trs_system sys;
		char path[1024], err[256];
		snprintf(path, sizeof path, "%s/bodies/%s", data_dir, files[fi]);
		if (!trs_system_load(&sys, path, err, sizeof err)) {
			trs_log_printf(out, "cannot load %s: %s\n", files[fi], err);
			return TRS_GATE_FAIL;
		}
		for (int bi = 0; bi < sys.count; bi++)
			if (sys.bodies[bi].terrain.enabled)
				ok = check_close_ground(out, &sys.bodies[bi]) && ok;
		for (int bi = 0; bi < sys.count; bi++) {
			if (!sys.bodies[bi].terrain.nfeatures)
				continue;
			n += sys.bodies[bi].terrain.nfeatures;
			ok = check_body(out, &sys.bodies[bi]) && ok;
		}
	}
	trs_log_printf(out, "%d landmarks\n", n);
	return ok && n >= 40 ? TRS_GATE_PASS : TRS_GATE_FAIL;
}
