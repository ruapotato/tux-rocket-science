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

/* Unit quaternions and 3x3 matrices for rigid-body attitude, in doubles. */
#ifndef TRS_QUAT_H
#define TRS_QUAT_H

#include "trs/vec3.h"

typedef struct {
	double x, y, z, w;
} trs_quat;

typedef struct {
	double m[3][3];
} trs_m3;

static inline trs_quat quat_identity(void) {
	trs_quat q = { 0, 0, 0, 1 };
	return q;
}

static inline trs_quat quat_mul(trs_quat a, trs_quat b) {
	trs_quat q = {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
	return q;
}

static inline trs_quat quat_conj(trs_quat q) {
	trs_quat r = { -q.x, -q.y, -q.z, q.w };
	return r;
}

static inline trs_quat quat_normalize(trs_quat q) {
	double l = sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	trs_quat r = { q.x / l, q.y / l, q.z / l, q.w / l };
	return r;
}

static inline trs_v3 quat_rotate(trs_quat q, trs_v3 v) {
	/* v' = v + 2w (u x v) + 2 u x (u x v) */
	trs_v3 u = v3(q.x, q.y, q.z);
	trs_v3 t = v3_scale(v3_cross(u, v), 2.0);
	return v3_add(v3_add(v, v3_scale(t, q.w)), v3_cross(u, t));
}

static inline trs_quat quat_axis_angle(trs_v3 axis, double angle) {
	trs_v3 a = v3_norm(axis);
	double s = sin(0.5 * angle);
	trs_quat q = { a.x * s, a.y * s, a.z * s, cos(0.5 * angle) };
	return q;
}

/* Rotation that takes unit vector a onto unit vector b. */
static inline trs_quat quat_from_to(trs_v3 a, trs_v3 b) {
	a = v3_norm(a);
	b = v3_norm(b);
	double d = v3_dot(a, b);
	if (d < -0.999999999) {
		trs_v3 axis = v3_cross(v3(1, 0, 0), a);
		if (v3_len2(axis) < 1e-12)
			axis = v3_cross(v3(0, 1, 0), a);
		return quat_axis_angle(axis, TRS_PI);
	}
	trs_v3 c = v3_cross(a, b);
	trs_quat q = { c.x, c.y, c.z, 1 + d };
	return quat_normalize(q);
}

/* Integrates attitude by a world-frame angular velocity over dt. */
static inline trs_quat quat_integrate(trs_quat q, trs_v3 w, double dt) {
	double ang = v3_len(w) * dt;
	if (ang < 1e-300)
		return q;
	return quat_normalize(quat_mul(quat_axis_angle(w, ang), q));
}

static inline trs_m3 m3_zero(void) {
	trs_m3 r = { { { 0 } } };
	return r;
}

static inline trs_v3 m3_mulv(const trs_m3 *a, trs_v3 v) {
	return v3(a->m[0][0] * v.x + a->m[0][1] * v.y + a->m[0][2] * v.z,
			a->m[1][0] * v.x + a->m[1][1] * v.y + a->m[1][2] * v.z,
			a->m[2][0] * v.x + a->m[2][1] * v.y + a->m[2][2] * v.z);
}

static inline trs_m3 m3_inverse(const trs_m3 *a) {
	const double(*m)[3] = a->m;
	double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	trs_m3 r;
	double id = det != 0 ? 1.0 / det : 0;
	r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
	r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
	r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
	r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
	r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
	r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
	r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
	r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
	r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
	return r;
}

/* World-frame inertia R I_body R^T from a body-frame tensor. */
static inline trs_m3 m3_rotate_tensor(trs_quat q, const trs_m3 *ib) {
	trs_v3 c[3] = { quat_rotate(q, v3(1, 0, 0)), quat_rotate(q, v3(0, 1, 0)), quat_rotate(q, v3(0, 0, 1)) };
	double R[3][3] = { { c[0].x, c[1].x, c[2].x }, { c[0].y, c[1].y, c[2].y }, { c[0].z, c[1].z, c[2].z } };
	trs_m3 t = m3_zero(), r = m3_zero();
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			for (int k = 0; k < 3; k++)
				t.m[i][j] += R[i][k] * ib->m[k][j];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			for (int k = 0; k < 3; k++)
				r.m[i][j] += t.m[i][k] * R[j][k];
	return r;
}

#endif
