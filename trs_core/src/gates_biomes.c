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

/* test-biomes: every world with land is split into named regions. The
 * launch site lies on its own shelf, each named mountain range is its own
 * biome at its centre, the poles are polar, and every broad biome covers a
 * real share of its world (sampled evenly over the sphere). */
#include "trs/gate.h"
#include "trs/system.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SAMPLES 20000

static trs_v3 latlon(double lat_deg, double lon_deg) {
	double la = lat_deg * TRS_PI / 180.0, lo = lon_deg * TRS_PI / 180.0;
	return v3(cos(la) * cos(lo), cos(la) * sin(lo), sin(la));
}

static int is(const trs_body *b, trs_v3 d, const char *id) {
	int k = trs_body_biome(b, d);
	return k >= 0 && strcmp(b->biomes[k].id, id) == 0;
}

trs_gate_status trs_gate_biomes(trs_log *out, const char *data_dir) {
	trs_system sys;
	char path[1024], err[256];
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_system_load(&sys, path, err, sizeof err)) {
		trs_log_printf(out, "cannot load system: %s\n", err);
		return TRS_GATE_FAIL;
	}
	int ok = 1;
	for (int bi = 0; bi < sys.count; bi++) {
		const trs_body *b = &sys.bodies[bi];
		if (!b->terrain.enabled) {
			if (b->nbiomes)
				trs_log_printf(out, "%s: biomes but no surface\n", b->id), ok = 0;
			continue;
		}
		if (b->nbiomes < 3) {
			trs_log_printf(out, "%s: only %d biomes\n", b->id, b->nbiomes);
			ok = 0;
			continue;
		}
		int count[TRS_MAX_BIOMES] = { 0 };
		double golden = TRS_PI * (3.0 - sqrt(5.0));
		for (int i = 0; i < SAMPLES; i++) {
			double z = 1.0 - (i + 0.5) * 2.0 / SAMPLES, rr = sqrt(1.0 - z * z);
			trs_v3 d = v3(rr * cos(golden * i), rr * sin(golden * i), z);
			count[trs_body_biome(b, d)]++;
		}
		trs_log_printf(out, "%s:", b->id);
		for (int k = 0; k < b->nbiomes; k++) {
			const trs_biome *e = &b->biomes[k];
			double share = 100.0 * count[k] / SAMPLES;
			trs_log_printf(out, " %s %.1f%%", e->name, share);
			if (e->has_near) {
				if (!is(b, e->near_dir, e->id))
					trs_log_printf(out, " (NOT at its own centre)"), ok = 0;
			} else if (share < 1.0)
				trs_log_printf(out, " (too small)"), ok = 0;
		}
		trs_log_printf(out, "\n");
		if (b->has_site) {
			trs_v3 site = latlon(b->site_lat * 180.0 / TRS_PI, b->site_lon * 180.0 / TRS_PI);
			int k = trs_body_biome(b, site);
			trs_log_printf(out, "  launch site in %s\n", b->biomes[k].name);
			ok = ok && b->biomes[k].has_near;
		}
		for (int s = -1; s <= 1; s += 2) {
			int k = trs_body_biome(b, latlon(89.5 * s, 10));
			if (b->biomes[k].abs_lat_min <= 0) {
				trs_log_printf(out, "  %s pole is %s, not polar\n", s > 0 ? "north" : "south", b->biomes[k].name);
				ok = 0;
			}
		}
	}
	return ok ? TRS_GATE_PASS : TRS_GATE_FAIL;
}
