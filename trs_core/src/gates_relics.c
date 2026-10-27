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

/* test-relics: the signal sources left on the worlds. Each stands on the
 * ground on gentle land a craft can set down beside; from any point its
 * distance is the straight line, its surface distance the great circle,
 * its heading the textbook initial bearing; it is in sight exactly when
 * the point is above its horizon; and the Signal Scanner hears 3,000 km. */
#include "trs/gate.h"
#include "trs/part.h"
#include "trs/system.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol) * fmax(1.0, fabs(b)))

int trs_gate_load_catalog(trs_log *out, const char *data_dir, trs_part_catalog *cat);

static trs_v3 latlon(double la, double lo) {
	return v3(cos(la) * cos(lo), cos(la) * sin(lo), sin(la));
}

trs_gate_status trs_gate_relics(trs_log *out, const char *data_dir) {
	trs_system sys;
	char path[1024], err[256];
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_system_load(&sys, path, err, sizeof err)) {
		trs_log_printf(out, "cannot load system: %s\n", err);
		return TRS_GATE_FAIL;
	}
	int ok = 1, n = 0;
	for (int bi = 0; bi < sys.count; bi++) {
		const trs_body *b = &sys.bodies[bi];
		for (int k = 0; k < b->nrelics; k++, n++) {
			const trs_relic *re = &b->relics[k];
			double R = b->radius;
			trs_v3 s = trs_relic_position(b, k);
			double hr = v3_len(s) - R, th = trs_terrain_height(&b->terrain, R, re->dir);
			/* slope: the ground 50 m round it */
			double la = asin(re->dir.z), lo = atan2(re->dir.y, re->dir.x), d = 50.0 / R, slope = 0;
			for (int q = 0; q < 8; q++) {
				double a = q * TRS_TAU / 8;
				double h2 = trs_terrain_height(&b->terrain, R, latlon(la + d * cos(a), lo + d * sin(a) / cos(la)));
				slope = fmax(slope, fabs(h2 - th) / 50.0);
			}
			int good = NEAR(hr, th, 1e-9) && slope < 0.1;
			trs_log_printf(out, "%s %s: %s on %s at %.2f, %.2f deg, ground %.1f m, slope %.3f\n", good ? "ok  " : "FAIL",
				re->id, re->name, b->name, la * 180 / TRS_PI, lo * 180 / TRS_PI, hr, slope);
			ok = ok && good;

			/* straight above, 100 km up */
			trs_relic_view v = trs_relic_from(b, k, v3_scale(re->dir, R + hr + 1e5));
			good = NEAR(v.distance, 1e5, 1e-9) && v.surface_distance < 1e-3 && v.in_sight;
			trs_log_printf(out, "  %s overhead at 100 km: %.6f km, %.6f m along the ground, %s\n", good ? "ok  " : "FAIL",
				v.distance / 1000, v.surface_distance, v.in_sight ? "in sight" : "hidden");
			ok = ok && good;

			/* the horizon: 200 km up, the relic's tangent plane is at
			 * cos(theta) = (R + hr) / (R + 200 km), along a meridian */
			double hv = 2e5, edge = acos((R + hr) / (R + hv));
			for (int side = -1; side <= 1; side += 2) {
				double th2 = edge * (1 + side * 0.01);
				trs_v3 p = v3_scale(latlon(la + th2, lo), R + hv);
				v = trs_relic_from(b, k, p);
				double want_dist = v3_len(v3_sub(p, s));
				good = v.in_sight == (side < 0) && NEAR(v.surface_distance, th2 * R, 1e-9) && NEAR(v.distance, want_dist, 1e-12) &&
				       NEAR(v.bearing, TRS_PI, 1e-9);
				trs_log_printf(out, "  %s %.0f km north, %s the horizon (%.1f km): %s, heading %.4f deg\n", good ? "ok  " : "FAIL",
					v.surface_distance / 1000, side < 0 ? "inside" : "past", edge * R / 1000, v.in_sight ? "in sight" : "hidden",
					v.bearing * 180 / TRS_PI);
				ok = ok && good;
			}

			/* headings against the textbook formula from a few places */
			double worst = 0;
			for (int q = 0; q < 16; q++) {
				double pla = la + 0.3 * sin(q * 1.7), plo = lo + 0.5 * cos(q * 2.3);
				trs_v3 p = v3_scale(latlon(pla, plo), R + 1e4);
				v = trs_relic_from(b, k, p);
				double dl = lo - plo;
				double want = atan2(sin(dl) * cos(la), cos(pla) * sin(la) - sin(pla) * cos(la) * cos(dl));
				if (want < 0)
					want += TRS_TAU;
				double want_sd = acos(sin(pla) * sin(la) + cos(pla) * cos(la) * cos(dl)) * R;
				worst = fmax(worst, fmax(fabs(remainder(v.bearing - want, TRS_TAU)), fabs(v.surface_distance - want_sd) / R));
			}
			good = worst < 1e-9;
			trs_log_printf(out, "  %s headings and ground distances from 16 places match the spherical formulas (worst %.2g)\n",
				good ? "ok  " : "FAIL", worst);
			ok = ok && good;
		}
	}
	if (n < 4) {
		trs_log_printf(out, "FAIL only %d relics\n", n);
		ok = 0;
	}

	trs_part_catalog cat;
	if (!trs_gate_load_catalog(out, data_dir, &cat))
		return TRS_GATE_FAIL;
	int si = trs_catalog_find(&cat, "signal_scanner");
	const trs_part_def *sc = si >= 0 ? &cat.defs[si] : NULL;
	int good = sc && NEAR(sc->scanner_range, 3e6, 1e-12);
	trs_log_printf(out, "%s Signal Scanner hears %.0f km\n", good ? "ok  " : "FAIL", sc ? sc->scanner_range / 1000 : 0);
	ok = ok && good;
	trs_catalog_free(&cat);
	return ok ? TRS_GATE_PASS : TRS_GATE_FAIL;
}
