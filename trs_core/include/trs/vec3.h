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

/* Double-precision 3-vectors. All authoritative geometry uses these. */
#ifndef TRS_VEC3_H
#define TRS_VEC3_H

#include <math.h>

typedef struct {
	double x, y, z;
} trs_v3;

static inline trs_v3 v3(double x, double y, double z) {
	trs_v3 r = { x, y, z };
	return r;
}
static inline trs_v3 v3_add(trs_v3 a, trs_v3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline trs_v3 v3_sub(trs_v3 a, trs_v3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline trs_v3 v3_scale(trs_v3 a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline trs_v3 v3_madd(trs_v3 a, trs_v3 b, double s) { return v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
static inline double v3_dot(trs_v3 a, trs_v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline trs_v3 v3_cross(trs_v3 a, trs_v3 b) {
	return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline double v3_len2(trs_v3 a) { return v3_dot(a, a); }
static inline double v3_len(trs_v3 a) { return sqrt(v3_dot(a, a)); }
static inline trs_v3 v3_norm(trs_v3 a) {
	double l = v3_len(a);
	return l > 0 ? v3_scale(a, 1.0 / l) : v3(0, 0, 0);
}
static inline trs_v3 v3_neg(trs_v3 a) { return v3(-a.x, -a.y, -a.z); }

#define TRS_PI 3.14159265358979323846
#define TRS_TAU 6.28318530717958647692

#endif
