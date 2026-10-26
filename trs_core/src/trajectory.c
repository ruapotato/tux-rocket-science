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

#include "trs/trajectory.h"

#include <math.h>
#include <stdlib.h>

trs_v3 trs_node_vector(const trs_orbit *o, const trs_node *n) {
	trs_v3 r, v;
	trs_orbit_state_at(o, n->t, &r, &v);
	trs_v3 pro = v3_norm(v);
	trs_v3 nrm = v3_norm(v3_cross(r, v));
	trs_v3 rad = v3_cross(pro, nrm); /* radial-out: in plane, away from the body */
	rad = v3_neg(rad);
	if (v3_dot(rad, r) < 0)
		rad = v3_neg(rad);
	return v3_add(v3_add(v3_scale(pro, n->pro), v3_scale(nrm, n->nrm)), v3_scale(rad, n->rad));
}

int trs_trajectory(const trs_system *sys, int body, const trs_orbit *o0, double t, const trs_node *nodes, int nn,
		double horizon, trs_patch *out, int max) {
	trs_orbit o = *o0;
	double t_max = t + horizon;
	int used[64] = { 0 };
	int n = 0, start_node = -1;
	while (n < max) {
		/* next unused node after t */
		int ni = -1;
		for (int k = 0; k < nn && k < 64; k++)
			if (!used[k] && nodes[k].t >= t && (ni < 0 || nodes[k].t < nodes[ni].t))
				ni = k;
		double limit = ni >= 0 ? fmin(nodes[ni].t, t_max) : t_max;
		trs_event ev = trs_next_event(sys, body, &o, t, limit);
		trs_patch *p = &out[n++];
		p->body = body;
		p->orbit = o;
		p->t0 = t;
		p->node = start_node;
		start_node = -1;
		if (ev.kind != TRS_EV_NONE) {
			p->t1 = ev.t;
			p->end = ev.kind == TRS_EV_SOI_EXIT ? TRS_PATCH_END_SOI_EXIT
					: ev.kind == TRS_EV_SOI_ENTER ? TRS_PATCH_END_SOI_ENTER
					: ev.kind == TRS_EV_ATMOSPHERE ? TRS_PATCH_END_ATMOSPHERE
												 : TRS_PATCH_END_SURFACE;
			if (p->end == TRS_PATCH_END_ATMOSPHERE || p->end == TRS_PATCH_END_SURFACE)
				break;
			trs_v3 r, v;
			trs_orbit_state_at(&o, ev.t, &r, &v);
			trs_rebase_state(sys, body, ev.body, ev.t, &r, &v);
			body = ev.body;
			trs_orbit_from_state(&o, sys->bodies[body].mu, r, v, ev.t);
			t = ev.t;
			continue;
		}
		if (ni >= 0 && nodes[ni].t <= t_max) {
			p->t1 = nodes[ni].t;
			p->end = TRS_PATCH_END_NODE;
			used[ni] = 1;
			trs_v3 r, v;
			trs_orbit_state_at(&o, nodes[ni].t, &r, &v);
			v = v3_add(v, trs_node_vector(&o, &nodes[ni]));
			trs_orbit_from_state(&o, sys->bodies[body].mu, r, v, nodes[ni].t);
			t = nodes[ni].t;
			start_node = ni;
			continue;
		}
		p->t1 = t_max;
		p->end = TRS_PATCH_END_HORIZON;
		break;
	}
	return n;
}

/* ---- closest approach ------------------------------------------------------- */

#define CA_SAMPLES 720

static double ca_dist(const trs_orbit *a, const trs_orbit *b, double t) {
	trs_v3 ra, va, rb, vb;
	trs_orbit_state_at(a, t, &ra, &va);
	trs_orbit_state_at(b, t, &rb, &vb);
	return v3_len(v3_sub(ra, rb));
}

/* Sampled over each overlap of same-body patches, then the best sample's
 * bracket is narrowed by golden section to a millisecond: the true minimum
 * of the distance between the two conics there. */
int trs_closest_approach(const trs_patch *a, int na, const trs_patch *b, int nb, double t0, double t1, trs_approach *out) {
	int found = 0;
	out->dist = INFINITY;
	for (int i = 0; i < na; i++)
		for (int j = 0; j < nb; j++) {
			if (a[i].body != b[j].body)
				continue;
			double lo = fmax(t0, fmax(a[i].t0, b[j].t0)), hi = fmin(t1, fmin(a[i].t1, b[j].t1));
			if (hi <= lo)
				continue;
			double step = (hi - lo) / CA_SAMPLES, best_t = lo, best = INFINITY;
			for (int k = 0; k <= CA_SAMPLES; k++) {
				double t = lo + step * k, d = ca_dist(&a[i].orbit, &b[j].orbit, t);
				if (d < best)
					best = d, best_t = t;
			}
			double x0 = fmax(lo, best_t - step), x1 = fmin(hi, best_t + step);
			const double gr = 0.6180339887498949;
			double c = x1 - gr * (x1 - x0), d = x0 + gr * (x1 - x0);
			double fc = ca_dist(&a[i].orbit, &b[j].orbit, c), fd = ca_dist(&a[i].orbit, &b[j].orbit, d);
			for (int it = 0; it < 200 && x1 - x0 > 1e-3; it++) {
				if (fc < fd) {
					x1 = d, d = c, fd = fc;
					c = x1 - gr * (x1 - x0);
					fc = ca_dist(&a[i].orbit, &b[j].orbit, c);
				} else {
					x0 = c, c = d, fc = fd;
					d = x0 + gr * (x1 - x0);
					fd = ca_dist(&a[i].orbit, &b[j].orbit, d);
				}
			}
			double t = 0.5 * (x0 + x1), dist = ca_dist(&a[i].orbit, &b[j].orbit, t);
			if (best < dist)
				t = best_t, dist = best; /* an end of the overlap */
			if (dist < out->dist) {
				trs_v3 va, vb;
				trs_orbit_state_at(&a[i].orbit, t, &out->ra, &va);
				trs_orbit_state_at(&b[j].orbit, t, &out->rb, &vb);
				out->t = t, out->dist = dist, out->body = a[i].body;
				out->rel_speed = v3_len(v3_sub(va, vb));
				found = 1;
			}
		}
	return found;
}
