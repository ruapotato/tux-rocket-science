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
		}
		int good = have >= want;
		trs_log_printf(out, "  %s %d: %s %.0f m (at least %.0f)%s\n", b->id, k, what, have, want, good ? "" : "  FAIL");
		ok = ok && good;
	}
	return ok;
}

trs_gate_status trs_gate_landmarks(trs_log *out, const char *data_dir) {
	const char *files[] = { "austra_system.json" };   /* (it loads its neighbours: Lantern too) */
	int ok = 1, n = 0;
	for (int fi = 0; fi < 1; fi++) {
		trs_system sys;
		char path[1024], err[256];
		snprintf(path, sizeof path, "%s/bodies/%s", data_dir, files[fi]);
		if (!trs_system_load(&sys, path, err, sizeof err)) {
			trs_log_printf(out, "cannot load %s: %s\n", files[fi], err);
			return TRS_GATE_FAIL;
		}
		for (int bi = 0; bi < sys.count; bi++) {
			if (!sys.bodies[bi].terrain.nfeatures)
				continue;
			n += sys.bodies[bi].terrain.nfeatures;
			ok = check_body(out, &sys.bodies[bi]) && ok;
		}
	}
	trs_log_printf(out, "%d landmarks\n", n);
	return ok && n >= 18 ? TRS_GATE_PASS : TRS_GATE_FAIL;
}
