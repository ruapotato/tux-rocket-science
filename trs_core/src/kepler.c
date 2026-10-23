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

#include "trs/kepler.h"

#include <float.h>
#include <math.h>

double trs_wrap_tau(double x) {
	x = fmod(x, TRS_TAU);
	return x < 0 ? x + TRS_TAU : x;
}

/* Stumpff functions C(z), S(z), with series near zero to avoid cancellation. */
static void stumpff(double z, double *c, double *s) {
	if (fabs(z) < 1.0) {
		/* C = sum (-z)^k/(2k+2)!, S = sum (-z)^k/(2k+3)! */
		double cc = 0, ss = 0, term_c = 0.5, term_s = 1.0 / 6.0;
		for (int k = 0; k < 14; k++) {
			cc += term_c;
			ss += term_s;
			term_c *= -z / ((2.0 * k + 3) * (2.0 * k + 4));
			term_s *= -z / ((2.0 * k + 4) * (2.0 * k + 5));
		}
		*c = cc;
		*s = ss;
	} else if (z > 0) {
		double sz = sqrt(z);
		*c = (1 - cos(sz)) / z;
		*s = (sz - sin(sz)) / (sz * z);
	} else {
		double sz = sqrt(-z);
		*c = (cosh(sz) - 1) / (-z);
		*s = (sinh(sz) - sz) / (sz * -z);
	}
}

void trs_orbit_from_state(trs_orbit *o, double mu, trs_v3 r, trs_v3 v, double t) {
	o->mu = mu;
	o->r0 = r;
	o->v0 = v;
	o->t0 = t;
	double rl = v3_len(r), v2 = v3_len2(v);
	o->energy = 0.5 * v2 - mu / rl;
	o->alpha = 2.0 / rl - v2 / mu;
	o->a = o->alpha != 0 ? 1.0 / o->alpha : INFINITY;
	o->h = v3_cross(r, v);
	double hl = v3_len(o->h);
	/* e = (v x h)/mu - r/|r| */
	o->evec = v3_sub(v3_scale(v3_cross(v, o->h), 1.0 / mu), v3_scale(r, 1.0 / rl));
	o->e = v3_len(o->evec);
	o->p = hl * hl / mu;
	o->rp = o->p / (1 + o->e);
	o->ra = o->e < 1 ? o->p / (1 - o->e) : INFINITY;
	o->inc = acos(fmax(-1.0, fmin(1.0, o->h.z / hl)));

	trs_v3 node = v3(-o->h.y, o->h.x, 0);
	double nl = v3_len(node);
	const double tiny = 1e-11;
	if (nl > tiny * hl) {
		o->lan = trs_wrap_tau(atan2(node.y, node.x));
	} else {
		o->lan = 0;
		node = v3(1, 0, 0);
		nl = 1;
	}
	/* Argument of periapsis measured in the orbit plane from the node. */
	trs_v3 hn = v3_scale(o->h, 1.0 / hl);
	trs_v3 nn = v3_scale(node, 1.0 / nl);
	trs_v3 qn = v3_cross(hn, nn); /* 90 deg ahead of node in-plane */
	if (o->e > tiny)
		o->argp = trs_wrap_tau(atan2(v3_dot(o->evec, qn), v3_dot(o->evec, nn)));
	else
		o->argp = 0;

	/* True anomaly at epoch, measured from periapsis (or node if circular). */
	trs_v3 pdir = o->e > tiny ? v3_scale(o->evec, 1.0 / o->e) : nn;
	trs_v3 qdir = v3_cross(hn, pdir);
	double nu = atan2(v3_dot(r, qdir), v3_dot(r, pdir));

	if (o->alpha > 0) {
		o->period = TRS_TAU / sqrt(mu * o->alpha * o->alpha * o->alpha);
		o->n = TRS_TAU / o->period;
		double E = 2 * atan2(sqrt(1 - o->e) * sin(nu / 2), sqrt(1 + o->e) * cos(nu / 2));
		o->M0 = trs_wrap_tau(E - o->e * sin(E));
	} else {
		o->period = 0;
		o->n = sqrt(mu * fabs(o->alpha * o->alpha * o->alpha));
		double th = sqrt((o->e - 1) / (o->e + 1)) * tan(nu / 2);
		double H = 2 * atanh(fmax(-1 + DBL_EPSILON, fmin(1 - DBL_EPSILON, th)));
		o->M0 = o->e * sinh(H) - H;
	}
}

static double kepler_E(double M, double e) {
	M = trs_wrap_tau(M);
	double E = e < 0.8 ? M : TRS_PI;
	for (int i = 0; i < 60; i++) {
		double f = E - e * sin(E) - M;
		double d = f / (1 - e * cos(E));
		E -= d;
		if (fabs(d) < 1e-15)
			break;
	}
	return E;
}

void trs_orbit_from_elements(trs_orbit *o, double mu, double a, double e, double inc, double lan, double argp,
		double mean_anomaly, double t0) {
	double E = kepler_E(mean_anomaly, e);
	double cE = cos(E), sE = sin(E);
	double b = a * sqrt(1 - e * e);
	double rl = a * (1 - e * cE);
	/* Perifocal position/velocity */
	double px = a * (cE - e), py = b * sE;
	double vfac = sqrt(mu * a) / rl;
	double vx = -vfac * sE, vy = vfac * sqrt(1 - e * e) * cE;
	double cO = cos(lan), sO = sin(lan), cw = cos(argp), sw = sin(argp), ci = cos(inc), si = sin(inc);
	/* Rotation perifocal -> reference: R3(lan) R1(inc) R3(argp) */
	double r11 = cO * cw - sO * sw * ci, r12 = -cO * sw - sO * cw * ci;
	double r21 = sO * cw + cO * sw * ci, r22 = -sO * sw + cO * cw * ci;
	double r31 = sw * si, r32 = cw * si;
	trs_v3 r = v3(r11 * px + r12 * py, r21 * px + r22 * py, r31 * px + r32 * py);
	trs_v3 v = v3(r11 * vx + r12 * vy, r21 * vx + r22 * vy, r31 * vx + r32 * vy);
	trs_orbit_from_state(o, mu, r, v, t0);
}

int trs_orbit_is_closed(const trs_orbit *o) {
	return o->alpha > 0;
}

/* Universal-variable propagation of the epoch state by dt (Laguerre-Conway). */
static void propagate(const trs_orbit *o, double dt, trs_v3 *r_out, trs_v3 *v_out) {
	if (dt == 0) {
		*r_out = o->r0;
		*v_out = o->v0;
		return;
	}
	const double mu = o->mu, smu = sqrt(mu), alpha = o->alpha;
	if (alpha > 0) {
		/* Drift-free: fold dt into one period around the epoch. */
		dt = fmod(dt, o->period);
		if (dt > 0.5 * o->period)
			dt -= o->period;
		else if (dt < -0.5 * o->period)
			dt += o->period;
	}
	const double r0 = v3_len(o->r0);
	const double sigma0 = v3_dot(o->r0, o->v0) / smu;

	double x;
	if (alpha > 1e-15) {
		x = smu * dt * alpha;
	} else if (alpha < -1e-15) {
		double a = 1 / alpha;
		double sgn = dt > 0 ? 1 : -1;
		double num = -2 * mu * alpha * dt;
		double den = v3_dot(o->r0, o->v0) + sgn * sqrt(-mu * a) * (1 - r0 * alpha);
		x = sgn * sqrt(-a) * log(fabs(num / den));
		if (!isfinite(x))
			x = smu * dt / r0;
	} else {
		x = smu * dt / r0;
	}

	double c = 0, s = 0, z = 0;
	const double nL = 5;
	for (int it = 0; it < 100; it++) {
		z = alpha * x * x;
		stumpff(z, &c, &s);
		double x2 = x * x;
		double F = sigma0 * x2 * c + (1 - alpha * r0) * x2 * x * s + r0 * x - smu * dt;
		double dF = sigma0 * x * (1 - z * s) + (1 - alpha * r0) * x2 * c + r0;
		double ddF = sigma0 * (1 - z * c) + (1 - alpha * r0) * x * (1 - z * s);
		double disc = fabs((nL - 1) * (nL - 1) * dF * dF - nL * (nL - 1) * F * ddF);
		double den = dF + (dF >= 0 ? 1 : -1) * sqrt(disc);
		double dx = nL * F / den;
		x -= dx;
		if (fabs(dx) <= 1e-15 * fmax(1.0, fabs(x)))
			break;
	}
	z = alpha * x * x;
	stumpff(z, &c, &s);
	double x2 = x * x;
	double f = 1 - x2 * c / r0;
	double g = dt - x2 * x * s / smu;
	trs_v3 r = v3_add(v3_scale(o->r0, f), v3_scale(o->v0, g));
	double rl = v3_len(r);
	double fd = smu / (rl * r0) * x * (z * s - 1);
	double gd = 1 - x2 * c / rl;
	*r_out = r;
	*v_out = v3_add(v3_scale(o->r0, fd), v3_scale(o->v0, gd));
}

void trs_orbit_state_at(const trs_orbit *o, double t, trs_v3 *r, trs_v3 *v) {
	propagate(o, t - o->t0, r, v);
}

double trs_orbit_mean_anomaly_at(const trs_orbit *o, double t) {
	double M = o->M0 + o->n * (t - o->t0);
	return o->alpha > 0 ? trs_wrap_tau(M) : M;
}

double trs_orbit_true_anomaly_at(const trs_orbit *o, double t) {
	double M = trs_orbit_mean_anomaly_at(o, t);
	double e = o->e;
	if (o->alpha > 0) {
		double E = kepler_E(M, e);
		return trs_wrap_tau(2 * atan2(sqrt(1 + e) * sin(E / 2), sqrt(1 - e) * cos(E / 2)));
	}
	double H = asinh(M / e);
	for (int i = 0; i < 80; i++) {
		double d = (e * sinh(H) - H - M) / (e * cosh(H) - 1);
		H -= d;
		if (fabs(d) < 1e-15 * fmax(1.0, fabs(H)))
			break;
	}
	return 2 * atan(sqrt((e + 1) / (e - 1)) * tanh(H / 2));
}

/* Mean anomaly corresponding to true anomaly nu. */
static double mean_from_true(const trs_orbit *o, double nu) {
	double e = o->e;
	if (o->alpha > 0) {
		double E = 2 * atan2(sqrt(1 - e) * sin(nu / 2), sqrt(1 + e) * cos(nu / 2));
		return trs_wrap_tau(E - e * sin(E));
	}
	double H = 2 * atanh(sqrt((e - 1) / (e + 1)) * tan(nu / 2));
	return e * sinh(H) - H;
}

/* Next absolute time after t at which the mean anomaly equals M_target. */
static double next_time_at_mean(const trs_orbit *o, double t, double M_target) {
	double Mt = trs_orbit_mean_anomaly_at(o, t);
	if (o->alpha > 0) {
		double dM = trs_wrap_tau(M_target - Mt);
		if (dM == 0)
			dM = TRS_TAU;
		return t + dM / o->n;
	}
	if (M_target <= Mt)
		return INFINITY;
	return t + (M_target - Mt) / o->n;
}

double trs_orbit_next_periapsis(const trs_orbit *o, double t) {
	return next_time_at_mean(o, t, 0);
}

double trs_orbit_next_apoapsis(const trs_orbit *o, double t) {
	if (o->alpha <= 0)
		return INFINITY;
	return next_time_at_mean(o, t, TRS_PI);
}

double trs_orbit_next_outbound_crossing(const trs_orbit *o, double t, double radius) {
	if (o->alpha > 0 && o->ra <= radius)
		return INFINITY;
	if (o->rp >= radius) /* entirely outside: already beyond */
		return t;
	double cnu = (o->p / radius - 1) / o->e;
	cnu = fmax(-1.0, fmin(1.0, cnu));
	double nu = acos(cnu); /* outbound branch, 0 < nu < pi */
	return next_time_at_mean(o, t, mean_from_true(o, nu));
}

double trs_orbit_next_inbound_crossing(const trs_orbit *o, double t, double radius) {
	if (o->rp >= radius)
		return INFINITY;
	trs_v3 r, v;
	trs_orbit_state_at(o, t, &r, &v);
	if (v3_len(r) <= radius)
		return t;
	double cnu = (o->p / radius - 1) / o->e;
	cnu = fmax(-1.0, fmin(1.0, cnu));
	double nu = -acos(cnu); /* inbound branch */
	double M = mean_from_true(o, nu);
	return next_time_at_mean(o, t, M);
}

double trs_orbit_max_speed(const trs_orbit *o) {
	return sqrt(o->mu * (2.0 / o->rp - o->alpha));
}
