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

/* Special relativity for flight between the stars: a ship under a
 * constant proper acceleration (what an accelerometer aboard would read,
 * if anything aboard could feel it), its proper velocity u = gamma v, and
 * the two clocks: coordinate time t (the stars' shared rest frame, so the
 * home world's calendar) and proper time tau (the ship's). */
#ifndef TRS_RELATIVITY_H
#define TRS_RELATIVITY_H

#include "trs/vec3.h"

#define TRS_C 299792458.0          /* m/s */
#define TRS_LIGHT_YEAR 9.4607304725808e15 /* m (Julian year of light) */
#define TRS_YEAR 31557600.0        /* s, Julian */

typedef struct {
	trs_v3 x;   /* position, m */
	trs_v3 u;   /* proper velocity gamma v, m/s */
	double t;   /* coordinate time, s */
	double tau; /* proper time, s */
} trs_rel_state;

double trs_rel_gamma(trs_v3 u);
trs_v3 trs_rel_velocity(trs_v3 u);        /* u / gamma */
trs_v3 trs_rel_proper_velocity(trs_v3 v); /* gamma v (|v| < c) */

/* Moves s on by dtau of proper time under proper acceleration a (its
 * direction in the stars' axes, its size what the ship's frame feels),
 * in steps small enough that the speed changes smoothly. */
void trs_rel_advance_tau(trs_rel_state *s, trs_v3 a, double dtau);
/* ... and by dt of coordinate time instead. */
void trs_rel_advance_t(trs_rel_state *s, trs_v3 a, double dt);

/* Closed form, rest to rest over distance d at proper acceleration a,
 * turning round halfway: proper time, coordinate time, peak speed. */
void trs_rel_flip_burn(double d, double a, double *tau, double *t, double *v_peak);

#endif
