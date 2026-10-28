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

/* Procedural terrain: a deterministic height field over each body's sphere.
 * The same function feeds ground contact (physics) and the LOD meshes the
 * view draws, so what you see is what you land on (to mesh resolution). */
#ifndef TRS_TERRAIN_H
#define TRS_TERRAIN_H

#include "trs/vec3.h"

typedef struct {
	int enabled;
	unsigned seed;
	double amplitude;   /* m */
	double base_freq;   /* features per body radius */
	int octaves;
	double persistence;
	double offset;      /* m added everywhere */
	double ridge;       /* 0..1 blend of ridged noise */
	double crater_freq; /* crater cells per radius (0 = none) */
	double crater_depth; /* m */
	double mountain_amplitude; /* m: ridged ranges, gated by a mask */
	double mountain_freq;      /* ridge features per radius */
	double mountain_coverage;  /* 0..1 fraction of the surface in ranges */
	/* rolling hills over the open ground (the plains between the ranges):
	 * relief in m, the hills' size in m, the fraction of the ground they
	 * cover (the rest stays plain) */
	double hills_amplitude, hills_size, hills_coverage;
	/* named mountain massifs placed by data (dir, radius m, height m) */
	int nranges;
	trs_v3 range_dir[8];
	double range_radius[8], range_height[8];
	/* landmarks shaped into the ground by data (a body's "landmarks"):
	 * a volcano (a cone with a caldera), a great crater (bowl, rim and a
	 * central peak), a canyon (a long rift along a bearing), a mesa (a
	 * flat-topped plateau with cliffs). dir is the centre (body-fixed unit);
	 * radius, height, depth in m; a canyon's length and width in m along its
	 * great circle (t: its way at the centre, g: the circle's pole). */
	int nfeatures;
	struct trs_feature {
		int kind;
		trs_v3 dir, t, g;
		double radius, height, depth, length, width;
	} feature[12];
	/* flattened launch site */
	int has_site;
	trs_v3 site_dir;    /* body-fixed unit vector */
	double site_radius, site_height;
} trs_terrain;

enum { TRS_FEAT_VOLCANO = 1, TRS_FEAT_CRATER, TRS_FEAT_CANYON, TRS_FEAT_MESA };

/* Height above the body's datum radius at body-fixed unit direction dir. */
double trs_terrain_height(const trs_terrain *t, double body_radius, trs_v3 dir);
/* Surface normal (body-fixed) by central differences over `step` metres. */
trs_v3 trs_terrain_normal(const trs_terrain *t, double body_radius, trs_v3 dir, double step);

/* Cube-sphere quadtree patch addressing. */
typedef struct {
	int face, level, i, j;
} trs_patch_id;

/* Direction for patch-local coordinates u,v in [0,1]. */
trs_v3 trs_patch_dir(trs_patch_id p, double u, double v);
/* Leaves of the LOD quadtree for a viewer at body-fixed position `eye`.
 * Writes up to max ids, returns the count. */
int trs_terrain_lod(const trs_terrain *t, double body_radius, trs_v3 eye, int max_level, double split_factor,
		trs_patch_id *out, int max);
/* Vertex grid for a patch: res x res vertices plus a skirt ring. Writes
 * positions relative to the patch centre (body-fixed) and normals; returns
 * the vertex count. center gets the patch centre position. */
int trs_terrain_patch_mesh(const trs_terrain *t, double body_radius, trs_patch_id p, int res, double *pos, double *nrm,
		trs_v3 *center);

#endif
