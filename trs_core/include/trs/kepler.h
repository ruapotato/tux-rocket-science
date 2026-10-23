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

/* Two-body (Kepler) orbits. An orbit is anchored to an epoch state vector and
 * always evaluated analytically from that epoch, so on-rails propagation
 * never accumulates drift no matter how many times it is sampled.
 *
 * Frame convention (sim): right-handed, +Z is the reference-plane normal. */
#ifndef TRS_KEPLER_H
#define TRS_KEPLER_H

#include "trs/vec3.h"

typedef struct {
	double mu;    /* gravitational parameter of the central body, m^3/s^2 */
	trs_v3 r0, v0; /* epoch state relative to the central body */
	double t0;    /* epoch, seconds of world time */

	/* Derived, read-only. */
	double energy; /* specific orbital energy v^2/2 - mu/r */
	double alpha;  /* 1/a; > 0 elliptic, < 0 hyperbolic */
	double a;      /* semi-major axis (negative when hyperbolic) */
	double e;      /* eccentricity */
	double p;      /* semi-latus rectum h^2/mu */
	double inc, lan, argp; /* radians */
	double rp, ra; /* periapsis / apoapsis radius; ra = INFINITY when open */
	double period; /* seconds; 0 when open */
	double n;      /* mean motion, rad/s */
	double M0;     /* mean anomaly at epoch */
	trs_v3 h, evec;
} trs_orbit;

void trs_orbit_from_state(trs_orbit *o, double mu, trs_v3 r, trs_v3 v, double t);
/* Angles in radians; mean anomaly at epoch t0. Elliptic only (e < 1). */
void trs_orbit_from_elements(trs_orbit *o, double mu, double a, double e, double inc, double lan, double argp,
		double mean_anomaly, double t0);

void trs_orbit_state_at(const trs_orbit *o, double t, trs_v3 *r, trs_v3 *v);

int trs_orbit_is_closed(const trs_orbit *o);
double trs_orbit_mean_anomaly_at(const trs_orbit *o, double t);
double trs_orbit_true_anomaly_at(const trs_orbit *o, double t);
/* Time (absolute) of the next periapsis / apoapsis passage strictly after t.
 * Apoapsis returns INFINITY for open orbits. */
double trs_orbit_next_periapsis(const trs_orbit *o, double t);
double trs_orbit_next_apoapsis(const trs_orbit *o, double t);
/* Next time after t at which |r| crosses `radius` going outward.
 * Returns INFINITY when it never does (closed orbit inside radius). */
double trs_orbit_next_outbound_crossing(const trs_orbit *o, double t, double radius);

/* Next time after t at which |r| crosses `radius` going inward. Returns t
 * when already inside, INFINITY when the orbit never gets that low. */
double trs_orbit_next_inbound_crossing(const trs_orbit *o, double t, double radius);
/* Speed at periapsis: the maximum speed anywhere on the conic. */
double trs_orbit_max_speed(const trs_orbit *o);

/* Wraps an angle to [0, 2pi). */
double trs_wrap_tau(double x);

#endif
