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

/* test-life: plants and animals on the ground. For every body with life,
 * over cells all round it: everything stands on the terrain (or flies its
 * height over it), only where its kind can live (biome, slope, height),
 * never near the launch site or a relic; animals' loops stay on the
 * ground round their spot; a cell is the same every time it is asked
 * for (and each kind alone as among the rest); single plants come at the
 * density the data asks for, counted
 * against how much of the cell suits them; a direction maps back to the
 * patch it came from; and the cells near a point cover everything within
 * range of it. Airless worlds have no animals. */
#include "trs/gate.h"
#include "trs/system.h"
#include "trs/terrain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double frand(unsigned *s) {
	*s = *s * 1664525u + 1013904223u;
	return (*s >> 8) / 16777216.0;
}

static trs_v3 rand_dir(unsigned *s) {
	double z = frand(s) * 2 - 1, a = frand(s) * 2 * TRS_PI, r = sqrt(fmax(0, 1 - z * z));
	return v3(r * cos(a), r * sin(a), z);
}

trs_gate_status trs_gate_life(trs_log *out, const char *data_dir) {
	trs_system sys;
	char path[1024], err[256];
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_system_load(&sys, path, err, sizeof err)) {
		trs_log_printf(out, "cannot load system: %s\n", err);
		return TRS_GATE_FAIL;
	}
	int max = 60000;
	trs_life_instance *li = malloc(sizeof(trs_life_instance) * (size_t)max);
	trs_life_instance *li2 = malloc(sizeof(trs_life_instance) * (size_t)max);
	int failed = 0;
	for (int bi = 0; bi < sys.count; bi++) {
		const trs_body *b = &sys.bodies[bi];
		if (b->nlife == 0)
			continue;
		int level = trs_life_level(b), cells = 0, fauna = 0;
		double cell_m = b->radius * (TRS_PI / 2) / (1 << level);
		double worst_ground = 0, worst_loop = 0;
		int misplaced = 0, near_site = 0, unstable = 0, total = 0;
		double got[TRS_MAX_LIFE] = { 0 }, want[TRS_MAX_LIFE] = { 0 };
		unsigned seed = 12345u + (unsigned)bi;
		for (int q = 0; q < 100; q++) {
			/* cells all round the body, and a few by the site and relics */
			trs_v3 d = rand_dir(&seed);
			if (q < 8 && b->terrain.has_site)
				d = b->terrain.site_dir;
			else if (q < 12 && b->nrelics)
				d = b->relics[q % b->nrelics].dir;
			trs_patch_id p = trs_patch_from_dir(d, level);
			trs_v3 c, c2;
			int n = trs_life_cell(b, p, ~0u, &c, li, max), n2 = trs_life_cell(b, p, ~0u, &c2, li2, max);
			unstable += n != n2 || memcmp(li, li2, sizeof(trs_life_instance) * (size_t)n) != 0;
			/* each kind alone is what it was among the rest */
			int k0 = q % b->nlife, m = 0;
			int n1 = trs_life_cell(b, p, 1u << k0, &c2, li2, max);
			for (int k = 0; k < n; k++)
				if (li[k].kind == k0)
					unstable += m >= n1 || memcmp(&li[k], &li2[m++], sizeof(trs_life_instance)) != 0;
			unstable += m != n1;
			cells++;
			total += n;
			for (int k = 0; k < n; k++) {
				const trs_life_instance *o = &li[k];
				const trs_life *L = &b->life[o->kind];
				trs_v3 at = v3_add(c, o->pos), dir = v3_norm(at), nrm;
				double h = trs_terrain_height(&b->terrain, b->radius, dir);
				worst_ground = fmax(worst_ground, fabs(v3_len(at) - (b->radius + h + L->fly)));
				misplaced += !trs_life_fits(b, L, dir, &nrm);
				if (b->terrain.has_site && acos(fmin(1, v3_dot(dir, b->terrain.site_dir))) * b->radius < b->terrain.site_radius + 300)
					near_site++;
				for (int r = 0; r < b->nrelics; r++)
					near_site += acos(fmin(1, v3_dot(dir, b->relics[r].dir))) * b->radius < 80;
				fauna += L->fauna;
				for (int e = 0; e < o->nloop; e++) {
					trs_v3 la = v3_add(c, o->loop[e]), ld = v3_norm(la);
					double lh = trs_terrain_height(&b->terrain, b->radius, ld);
					double off = acos(fmin(1, v3_dot(ld, dir))) * b->radius;
					worst_loop = fmax(worst_loop, fabs(v3_len(la) - (b->radius + lh + L->fly)));
					if (off < 0.6 * L->loop - 1e-6 || off > L->loop + 1e-6)
						worst_loop = fmax(worst_loop, 1e9);
				}
				if (L->herd <= 1)
					got[o->kind] += 1;
			}
			/* what single plants should come to: density x area x the
			 * share of the cell that suits them (sampled) */
			double km2 = trs_life_cell_area(b, p) / 1e6;
			for (int k = 0; k < b->nlife; k++) {
				if (b->life[k].herd > 1)
					continue;
				int fit = 0, m = 48;
				for (int s = 0; s < m; s++) {
					trs_v3 nrm;
					fit += trs_life_fits(b, &b->life[k], trs_patch_dir(p, frand(&seed), frand(&seed)), &nrm);
				}
				want[k] += b->life[k].density * km2 * fit / m;
			}
		}
		int dens_ok = 1;
		char dens[1024] = "";
		for (int k = 0; k < b->nlife; k++) {
			if (b->life[k].herd > 1)
				continue;
			double tol = 4 * sqrt(want[k]) + 0.02 * want[k] + 5;
			int ok = fabs(got[k] - want[k]) <= tol;
			dens_ok &= ok;
			snprintf(dens + strlen(dens), sizeof dens - strlen(dens), " %s %.0f/%.0f%s", b->life[k].id, got[k], want[k], ok ? "" : "!");
		}
		int airless_ok = b->atm.height > 0 || fauna == 0;
		/* a direction maps back to its patch; cells near a point cover its surroundings */
		int inverse_ok = 1, cover_ok = 1;
		for (int q = 0; q < 200; q++) {
			trs_patch_id p = { (int)(frand(&seed) * 6), level, 0, 0 };
			p.i = (int)(frand(&seed) * (1 << level)), p.j = (int)(frand(&seed) * (1 << level));
			trs_patch_id r = trs_patch_from_dir(trs_patch_dir(p, 0.05 + 0.9 * frand(&seed), 0.05 + 0.9 * frand(&seed)), level);
			inverse_ok &= r.face == p.face && r.i == p.i && r.j == p.j;
		}
		trs_patch_id near[512];
		for (int q = 0; q < 6; q++) {
			trs_v3 eye = rand_dir(&seed);
			double range = 1500;
			int nn = trs_life_cells_near(b, eye, range, near, 512);
			for (int s = 0; s < 64; s++) {
				/* a point within range of the eye */
				double a = frand(&seed) * 2 * TRS_PI, m = range * sqrt(frand(&seed)) / b->radius;
				trs_v3 e = v3_norm(v3_cross(v3(0, 0, 1), eye)), nn2 = v3_cross(eye, e);
				trs_v3 d = v3_norm(v3_add(v3_scale(eye, cos(m)), v3_scale(v3_add(v3_scale(e, cos(a)), v3_scale(nn2, sin(a))), sin(m))));
				trs_patch_id p = trs_patch_from_dir(d, level);
				int found = 0;
				for (int k = 0; k < nn; k++)
					found |= near[k].face == p.face && near[k].i == p.i && near[k].j == p.j;
				cover_ok &= found;
			}
		}
		int ok = worst_ground < 1e-6 && worst_loop < 1e-6 && misplaced == 0 && near_site == 0 && unstable == 0 && dens_ok && airless_ok &&
			 inverse_ok && cover_ok && total > 0;
		failed += !ok;
		trs_log_printf(out, "%s %s: %d kinds on %.0f m cells; %d cells, %d living things (%d animals): on the ground to %.1e m, loops to %.1e m, "
				"%d out of place, %d by the site or a relic, %d cells changed when asked again; singles got/want:%s; patch inverse %s, near cells %s\n",
				ok ? "ok  " : "FAIL", b->name, b->nlife, cell_m, cells, total, fauna, worst_ground, worst_loop, misplaced, near_site, unstable, dens,
				inverse_ok ? "ok" : "BAD", cover_ok ? "cover" : "MISS");
	}
	free(li);
	free(li2);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
