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

#include "trs/relativity.h"

#include <math.h>

double trs_rel_gamma(trs_v3 u) {
	return sqrt(1.0 + v3_len2(u) / (TRS_C * TRS_C));
}

trs_v3 trs_rel_velocity(trs_v3 u) {
	return v3_scale(u, 1.0 / trs_rel_gamma(u));
}

trs_v3 trs_rel_proper_velocity(trs_v3 v) {
	double b2 = v3_len2(v) / (TRS_C * TRS_C);
	return v3_scale(v, 1.0 / sqrt(fmax(1e-300, 1.0 - b2)));
}

/* d/dtau of (x, u, t): u, the boost of the rest-frame acceleration into
 * the stars' frame (the part along the motion grows by gamma), gamma. */
static void deriv(trs_v3 u, trs_v3 a, trs_v3 *du, double *dt) {
	double g = trs_rel_gamma(u), ul = v3_len(u);
	if (ul > 0) {
		trs_v3 n = v3_scale(u, 1.0 / ul);
		*du = v3_madd(a, n, (g - 1.0) * v3_dot(a, n));
	} else
		*du = a;
	*dt = g;
}

static void rk4(trs_rel_state *s, trs_v3 a, double h) {
	trs_v3 du1, du2, du3, du4;
	double dt1, dt2, dt3, dt4;
	trs_v3 u1 = s->u;
	deriv(u1, a, &du1, &dt1);
	trs_v3 u2 = v3_madd(s->u, du1, h * 0.5);
	deriv(u2, a, &du2, &dt2);
	trs_v3 u3 = v3_madd(s->u, du2, h * 0.5);
	deriv(u3, a, &du3, &dt3);
	trs_v3 u4 = v3_madd(s->u, du3, h);
	deriv(u4, a, &du4, &dt4);
	/* dx/dtau = u: integrate the u samples alongside */
	s->x = v3_add(s->x, v3_scale(v3_add(v3_add(u1, u4), v3_scale(v3_add(u2, u3), 2.0)), h / 6.0));
	s->u = v3_add(s->u, v3_scale(v3_add(v3_add(du1, du4), v3_scale(v3_add(du2, du3), 2.0)), h / 6.0));
	s->t += (dt1 + 2.0 * dt2 + 2.0 * dt3 + dt4) * h / 6.0;
	s->tau += h;
}

/* longest proper-time step: a small change of rapidity per step */
static double max_step(trs_v3 a) {
	double al = v3_len(a);
	return al > 0 ? 0.01 * TRS_C / al : INFINITY;
}

void trs_rel_advance_tau(trs_rel_state *s, trs_v3 a, double dtau) {
	if (dtau <= 0)
		return;
	if (v3_len2(a) == 0) { /* coasting: exact */
		double g = trs_rel_gamma(s->u);
		s->x = v3_madd(s->x, s->u, dtau);
		s->t += g * dtau;
		s->tau += dtau;
		return;
	}
	int n = (int)ceil(dtau / max_step(a));
	for (int i = 0; i < n; i++)
		rk4(s, a, dtau / n);
}

void trs_rel_advance_t(trs_rel_state *s, trs_v3 a, double dt) {
	double target = s->t + dt;
	for (int guard = 0; guard < 1000000 && target - s->t > 1e-9 * fmax(1.0, fabs(target)); guard++) {
		double left = target - s->t;
		double h = fmin(max_step(a), left / trs_rel_gamma(s->u));
		trs_rel_state keep = *s;
		trs_rel_advance_tau(s, a, h);
		if (s->t > target) { /* gamma grew within the step: take the part that fits */
			double over = (s->t - keep.t);
			*s = keep;
			trs_rel_advance_tau(s, a, h * left / over);
			if (s->t > target)
				break;
		}
	}
}

void trs_rel_flip_burn(double d, double a, double *tau, double *t, double *v_peak) {
	double c = TRS_C, eta = acosh(1.0 + 0.5 * d * a / (c * c)); /* rapidity at the turn */
	*tau = 2.0 * c / a * eta;
	*t = 2.0 * c / a * sinh(eta);
	*v_peak = c * tanh(eta);
}
