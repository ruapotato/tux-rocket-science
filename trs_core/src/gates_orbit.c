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

/* M1 gates: on-rails Kepler drift, and rails <-> physics mode switching. */
#include "trs/gate.h"
#include "trs/kepler.h"
#include "trs/trajectory.h"
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DEG (TRS_PI / 180.0)

static int load_system(trs_log *log, const char *data_dir, trs_system *sys) {
	char path[1024], err[256];
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_system_load(sys, path, err, sizeof err)) {
		trs_log_printf(log, "cannot load system: %s\n", err);
		return 0;
	}
	return 1;
}

static double rel(double a, double b) {
	return fabs(a - b) / fmax(fabs(b), 1e-300);
}

static double angdiff(double a, double b) {
	double d = fmod(a - b, TRS_TAU);
	if (d > TRS_PI) d -= TRS_TAU;
	if (d < -TRS_PI) d += TRS_TAU;
	return fabs(d);
}

typedef struct {
	double energy, a, h, e, inc, lan, argp, phase;
} drift;

static void drift_max(drift *m, const trs_orbit *ref, const trs_orbit *o, int angles_ok) {
	double v;
	if ((v = rel(o->energy, ref->energy)) > m->energy) m->energy = v;
	if ((v = rel(o->a, ref->a)) > m->a) m->a = v;
	if ((v = rel(v3_len(o->h), v3_len(ref->h))) > m->h) m->h = v;
	if ((v = fabs(o->e - ref->e)) > m->e) m->e = v;
	if ((v = angdiff(o->inc, ref->inc)) > m->inc) m->inc = v;
	if (angles_ok & 1)
		if ((v = angdiff(o->lan, ref->lan)) > m->lan) m->lan = v;
	if (angles_ok & 2)
		if ((v = angdiff(o->argp, ref->argp)) > m->argp) m->argp = v;
	/* Phase: the mean anomaly the sampled state implies vs. the reference. */
	if (ref->alpha > 0) {
		double Mref = trs_orbit_mean_anomaly_at(ref, o->t0);
		if ((v = angdiff(o->M0, Mref)) > m->phase) m->phase = v;
	} else {
		double Mref = trs_orbit_mean_anomaly_at(ref, o->t0);
		if ((v = fabs(o->M0 - Mref) / fmax(1.0, fabs(Mref))) > m->phase) m->phase = v;
	}
}

static double drift_worst(const drift *d) {
	double w = d->energy;
	const double *f = &d->energy;
	for (int i = 1; i < 8; i++)
		if (f[i] > w) w = f[i];
	return w;
}

typedef struct {
	const char *name;
	double mu;
	trs_orbit o;
	int angles; /* bit0 lan defined, bit1 argp defined */
} kcase;

trs_gate_status trs_gate_kepler(trs_log *log, const char *data_dir) {
	trs_system sys;
	if (!load_system(log, data_dir, &sys))
		return TRS_GATE_FAIL;
	const double tol = 1e-9, chained_phase_tol = 1e-7;
	int floe = trs_system_find(&sys, "floe"), krill = trs_system_find(&sys, "krill");
	double muF = sys.bodies[floe].mu, R = sys.bodies[floe].radius;

	kcase cases[10];
	int n = 0;
	cases[n].name = "Floe around Austra (body rails)";
	cases[n].o = sys.bodies[floe].orbit;
	cases[n++].angles = 0;
	cases[n].name = "Krill around Floe (body rails)";
	cases[n].o = sys.bodies[krill].orbit;
	cases[n++].angles = 0;
	cases[n].name = "LEO 80x100 km";
	trs_orbit_from_elements(&cases[n].o, muF, R + 90e3, 10e3 / (2 * R + 180e3), 0.5 * DEG, 10 * DEG, 20 * DEG, 0.3, 0);
	cases[n++].angles = 3;
	cases[n].name = "eccentric e=0.7 i=30";
	trs_orbit_from_elements(&cases[n].o, muF, 3.0e6, 0.7, 30 * DEG, 40 * DEG, 70 * DEG, 2.0, 0);
	cases[n++].angles = 3;
	cases[n].name = "transfer-like e=0.97";
	trs_orbit_from_elements(&cases[n].o, muF, 2.0e7, 0.97, 2 * DEG, 100 * DEG, 200 * DEG, 0.01, 0);
	cases[n++].angles = 3;
	cases[n].name = "retrograde polar-ish i=170";
	trs_orbit_from_elements(&cases[n].o, muF, 9.0e5, 0.05, 170 * DEG, 300 * DEG, 45 * DEG, 5.0, 0);
	cases[n++].angles = 3;
	cases[n].name = "equatorial e=0.2 (node undefined)";
	trs_orbit_from_elements(&cases[n].o, muF, 1.5e6, 0.2, 0, 0, 123 * DEG, 1.0, 0);
	cases[n++].angles = 2;

	int failed = 0;
	for (int c = 0; c < n; c++) {
		const trs_orbit *ref = &cases[c].o;
		/* (a) direct: sample the analytic orbit 10x per period for 1000 periods */
		drift d1 = { 0 };
		const int samples = 10000;
		for (int k = 1; k <= samples; k++) {
			double t = ref->t0 + ref->period * 1000.0 * k / samples + 0.123 * k;
			trs_v3 r, v;
			trs_orbit_state_at(ref, t, &r, &v);
			trs_orbit o;
			trs_orbit_from_state(&o, ref->mu, r, v, t);
			drift_max(&d1, ref, &o, cases[c].angles);
		}
		/* (b) chained: re-anchor from the propagated state every 0.37 periods
		 * (what every warp start/stop does), for 1000 periods. */
		drift d2 = { 0 };
		trs_orbit cur = *ref;
		double t = ref->t0;
		while (t < ref->t0 + 1000.0 * ref->period) {
			t += 0.37 * ref->period;
			trs_v3 r, v;
			trs_orbit_state_at(&cur, t, &r, &v);
			trs_orbit_from_state(&cur, ref->mu, r, v, t);
			drift_max(&d2, ref, &cur, cases[c].angles);
		}
		/* Re-anchoring 2700 times random-walks a by ~1 ulp per anchor, which
		 * integrates into along-track phase; that is reported separately with
		 * its own bound. Energy and orbit shape still must hold 1e-9. */
		double chained_phase = d2.phase;
		d2.phase = 0;
		double w1 = drift_worst(&d1), w2 = drift_worst(&d2);
		int ok = w1 < tol && w2 < tol && chained_phase < chained_phase_tol;
		failed += !ok;
		trs_log_printf(log, "%s %-34s P=%.0fs rails=%.2e chained=%.2e chained-phase=%.1e rad\n", ok ? "ok  " : "FAIL",
				cases[c].name, ref->period, w1, w2, chained_phase);
	}

	/* Hyperbolic flyby: no period, so sample out to 1000 x the time to cross
	 * Floe's SOI and back in both directions. */
	{
		trs_orbit h;
		trs_v3 r0 = v3(R + 200e3, 0, 0), v0 = v3(0, 4200, 300);
		trs_orbit_from_state(&h, muF, r0, v0, 0);
		drift d = { 0 };
		for (int k = -1000; k <= 1000; k++) {
			double t = k * 1e4;
			trs_v3 r, v;
			trs_orbit_state_at(&h, t, &r, &v);
			trs_orbit o;
			trs_orbit_from_state(&o, muF, r, v, t);
			drift_max(&d, &h, &o, 3);
		}
		double w = drift_worst(&d);
		int ok = w < tol && h.e > 1;
		failed += !ok;
		trs_log_printf(log, "%s %-34s e=%.3f worst=%.2e\n", ok ? "ok  " : "FAIL", "hyperbolic flyby +-1e7 s", h.e, w);
	}
	trs_log_printf(log, "rails: 10000 analytic samples over 1000 periods; chained: re-anchored every 0.37 P\n");
	trs_log_printf(log, "tolerance %.0e relative on E, a, |h|; absolute on e, i, lan, argp, M (chained M: %.0e)\n", tol,
			chained_phase_tol);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}

/* Deterministic LCG so the gate is reproducible on every platform. */
static unsigned lcg(unsigned *s) {
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}

trs_gate_status trs_gate_mode_switch(trs_log *log, const char *data_dir) {
	trs_system sys;
	if (!load_system(log, data_dir, &sys))
		return TRS_GATE_FAIL;
	int floe = trs_system_find(&sys, "floe"), krill = trs_system_find(&sys, "krill");
	int failed = 0;

	struct {
		const char *name;
		int body;
		double a, e, inc;
	} cases[] = {
		{ "Floe 80x120 km coast", floe, 0, 0, 0 },
		{ "Floe eccentric 100 x 5000 km", floe, 0, 0, 0 },
		{ "Krill 15 x 60 km", krill, 0, 0, 0 },
	};
	double Rf = sys.bodies[floe].radius, Rk = sys.bodies[krill].radius;
	double pe_ap[3][2] = { { Rf + 80e3, Rf + 120e3 }, { Rf + 100e3, Rf + 5000e3 }, { Rk + 15e3, Rk + 60e3 } };

	for (int c = 0; c < 3; c++) {
		double rp = pe_ap[c][0], ra = pe_ap[c][1];
		trs_body *b = &sys.bodies[cases[c].body];
		double a = 0.5 * (rp + ra), e = (ra - rp) / (ra + rp);
		trs_orbit init;
		trs_orbit_from_elements(&init, b->mu, a, e, 6 * DEG, 30 * DEG, 50 * DEG, 0.4, 1000.0);
		trs_world w;
		trs_world_init(&w, &sys, 1000.0);
		int id = trs_world_add_vessel(&w, cases[c].body, init.r0, init.v0);
		trs_vessel_set_mode(&w, id, TRS_MODE_RAILS);

		unsigned seed = 12345u + (unsigned)c;
		double max_dpe = 0, max_dap = 0;
		int physics_ticks = 0, toggles = 0;
		for (int k = 0; k < 1000; k++) {
			/* coast on rails for a while (up to ~1/3 orbit) */
			trs_event ev;
			double warp = (lcg(&seed) % 100000) * 1e-5 * init.period * 0.33;
			trs_world_warp(&w, warp, id, &ev);
			if (ev.kind != TRS_EV_NONE) {
				trs_log_printf(log, "unexpected event %d during warp\n", ev.kind);
				failed++;
				break;
			}
			/* drop to physics for 1..250 ticks (0.02 .. 5 s) */
			trs_vessel_set_mode(&w, id, TRS_MODE_PHYSICS);
			toggles++;
			int ticks = 1 + (int)(lcg(&seed) % 250);
			for (int s = 0; s < ticks; s++)
				trs_world_step_physics(&w);
			physics_ticks += ticks;
			trs_vessel_set_mode(&w, id, TRS_MODE_RAILS);
			toggles++;
			trs_orbit o;
			trs_vessel_orbit(&w, id, &o);
			max_dpe = fmax(max_dpe, fabs(o.rp - init.rp));
			max_dap = fmax(max_dap, fabs(o.ra - init.ra));
		}
		int ok = max_dpe < 1.0 && max_dap < 1.0 && w.vessels[id].body == cases[c].body;
		failed += !ok;
		trs_log_printf(log, "%s %-30s toggles=%d physics=%.0fs  |dPe|=%.3e m |dAp|=%.3e m\n", ok ? "ok  " : "FAIL",
				cases[c].name, toggles, physics_ticks * TRS_PHYSICS_DT, max_dpe, max_dap);
	}
	trs_log_printf(log, "tolerance 1 m on apoapsis and periapsis\n");
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}

/* Builds a Floe LEO -> Krill transfer by scanning the burn phase until the
 * patched-conic search predicts a Krill SOI entry. Returns the vessel state
 * right after an impulsive prograde burn at time *t_burn. */
static int make_krill_transfer(const trs_system *sys, double t_start, double *t_burn, trs_v3 *r_out, trs_v3 *v_out,
		double *t_enc) {
	int floe = trs_system_find(sys, "floe"), krill = trs_system_find(sys, "krill");
	const trs_body *F = &sys->bodies[floe], *K = &sys->bodies[krill];
	double r1 = F->radius + 100e3, r2 = K->orbit.a;
	double vc = sqrt(F->mu / r1);
	double at = 0.5 * (r1 + r2);
	double vt = sqrt(F->mu * (2 / r1 - 1 / at));
	trs_orbit leo;
	trs_orbit_from_state(&leo, F->mu, v3(r1, 0, 0), v3(0, vc, 0), t_start);
	for (int k = 0; k < 2000; k++) {
		double tb = t_start + k * leo.period / 400.0;
		trs_v3 r, v;
		trs_orbit_state_at(&leo, tb, &r, &v);
		/* slightly more than Hohmann so the apoapsis reaches into Krill's SOI */
		v = v3_scale(v3_norm(v), vt + 15);
		trs_orbit xfer;
		trs_orbit_from_state(&xfer, F->mu, r, v, tb);
		trs_event ev = trs_next_event(sys, floe, &xfer, tb, tb + xfer.period);
		if (ev.kind == TRS_EV_SOI_ENTER && ev.body == krill) {
			*t_burn = tb;
			*r_out = r;
			*v_out = v;
			*t_enc = ev.t;
			return 1;
		}
	}
	return 0;
}

trs_gate_status trs_gate_patched_conics(trs_log *log, const char *data_dir) {
	trs_system sys;
	if (!load_system(log, data_dir, &sys))
		return TRS_GATE_FAIL;
	int austra = trs_system_find(&sys, "austra"), floe = trs_system_find(&sys, "floe"),
		krill = trs_system_find(&sys, "krill");
	int failed = 0;
	const trs_body *K = &sys.bodies[krill], *F = &sys.bodies[floe];

	double tb, tenc;
	trs_v3 r, v;
	if (!make_krill_transfer(&sys, 0, &tb, &r, &v, &tenc)) {
		trs_log_printf(log, "FAIL could not find a Krill transfer window\n");
		return TRS_GATE_FAIL;
	}
	trs_log_printf(log, "transfer burn at t=%.1f s, predicted Krill SOI entry at t=%.1f s\n", tb, tenc);

	/* 1. Coarse warp (100000x at 60 fps) and fine warp (10x) agree on the
	 *    entry and the exit, and the crossings sit exactly on the SOI. */
	double t_in[2] = { 0 }, t_out[2] = { 0 }, r_in[2] = { 0 }, r_out[2] = { 0 };
	double steps[2] = { 100000.0 / 60.0, 10.0 / 60.0 * 50 };
	for (int pass = 0; pass < 2; pass++) {
		trs_world w;
		trs_world_init(&w, &sys, tb);
		int id = trs_world_add_vessel(&w, floe, r, v);
		trs_vessel_set_mode(&w, id, TRS_MODE_RAILS);
		int body = floe;
		for (int it = 0; it < 2000000 && w.t < tenc + 3e5; it++) {
			trs_world_warp(&w, steps[pass], id, NULL);
			trs_vessel *vs = &w.vessels[id];
			if (vs->body != body) {
				/* orbit epoch is the exact event time */
				double rr = v3_len(vs->orbit.r0);
				if (vs->body == krill && !t_in[pass]) {
					t_in[pass] = vs->orbit.t0;
					r_in[pass] = rr;
				} else if (vs->body == floe && t_in[pass] && !t_out[pass]) {
					t_out[pass] = vs->orbit.t0;
					trs_v3 rk, vk;
					trs_body_state(&sys, krill, vs->orbit.t0, &rk, &vk);
					r_out[pass] = v3_len(v3_sub(vs->orbit.r0, rk));
				}
				body = vs->body;
			}
			if (t_out[pass])
				break;
		}
	}
	double soi = K->soi;
	int ok1 = t_in[0] > 0 && t_out[0] > 0 && fabs(t_in[0] - t_in[1]) < 1e-3 && fabs(t_out[0] - t_out[1]) < 1e-3 &&
			fabs(r_in[0] / soi - 1) < 1e-6 && fabs(r_out[0] / soi - 1) < 1e-6 && fabs(t_in[0] - tenc) < 1e-3;
	failed += !ok1;
	trs_log_printf(log, "%s Krill flyby: in t=%.3f (fine %.3f) r/SOI=%.9f, out t=%.3f (fine %.3f) r/SOI=%.9f\n",
			ok1 ? "ok  " : "FAIL", t_in[0], t_in[1], r_in[0] / soi, t_out[0], t_out[1], r_out[0] / soi);

	/* 2. Continuity: the Floe-relative state is identical across the hop. */
	{
		trs_orbit xfer;
		trs_orbit_from_state(&xfer, F->mu, r, v, tb);
		trs_v3 rf, vf;
		trs_orbit_state_at(&xfer, tenc, &rf, &vf);
		trs_v3 rk = rf, vk = vf;
		trs_rebase_state(&sys, floe, krill, tenc, &rk, &vk);
		trs_rebase_state(&sys, krill, floe, tenc, &rk, &vk);
		double dr = v3_len(v3_sub(rk, rf)), dv = v3_len(v3_sub(vk, vf));
		int ok = dr < 1e-6 && dv < 1e-9;
		failed += !ok;
		trs_log_printf(log, "%s rebase round trip: |dr|=%.2e m |dv|=%.2e m/s\n", ok ? "ok  " : "FAIL", dr, dv);
	}

	/* 3. Physics mode crosses at the same tick the rails search predicts. */
	{
		trs_world w;
		trs_world_init(&w, &sys, tb);
		int id = trs_world_add_vessel(&w, floe, r, v);
		trs_vessel_set_mode(&w, id, TRS_MODE_RAILS);
		trs_world_warp(&w, (tenc - tb) - 30.0, id, NULL);
		trs_vessel_set_mode(&w, id, TRS_MODE_PHYSICS);
		double t_cross = 0;
		for (int s = 0; s < 60 * TRS_PHYSICS_HZ; s++) {
			trs_world_step_physics(&w);
			if (w.vessels[id].body == krill) {
				t_cross = w.t;
				break;
			}
		}
		int ok = t_cross > 0 && fabs(t_cross - tenc) <= TRS_PHYSICS_DT + 1e-6;
		failed += !ok;
		trs_log_printf(log, "%s physics-mode SOI entry at t=%.3f vs rails %.3f (|dt| %.4f s, tick %.2f s)\n",
				ok ? "ok  " : "FAIL", t_cross, tenc, fabs(t_cross - tenc), TRS_PHYSICS_DT);
	}

	/* 4. Escape: a hyperbolic departure from Floe hands over to Austra. */
	{
		trs_world w;
		trs_world_init(&w, &sys, 0);
		double r1 = F->radius + 100e3;
		double vesc = sqrt(2 * F->mu / r1);
		/* depart anti-prograde wrt Krill's position so we miss it */
		trs_v3 rk, vk;
		trs_body_state(&sys, krill, 0, &rk, &vk);
		trs_v3 r0 = v3_scale(v3_norm(rk), r1);
		trs_v3 v0 = v3_scale(v3_norm(v3_cross(v3(0, 0, -1), r0)), vesc * 1.3);
		int id = trs_world_add_vessel(&w, floe, r0, v0);
		trs_vessel_set_mode(&w, id, TRS_MODE_RAILS);
		trs_v3 abs_before_r, abs_before_v;
		for (int it = 0; it < 100000 && w.vessels[id].body == floe; it++)
			trs_world_warp(&w, 2000.0, id, NULL);
		trs_vessel *vs = &w.vessels[id];
		trs_orbit_state_at(&vs->orbit, vs->orbit.t0, &abs_before_r, &abs_before_v);
		trs_v3 rf, vf;
		trs_body_state(&sys, floe, vs->orbit.t0, &rf, &vf);
		double rr = v3_len(v3_sub(abs_before_r, rf));
		int ok = vs->body == austra && fabs(rr / F->soi - 1) < 1e-6 && vs->soi_changes == 1;
		failed += !ok;
		trs_log_printf(log, "%s escape to Austra at t=%.1f s, r/SOI(Floe)=%.9f, heliocentric e=%.4f\n", ok ? "ok  " : "FAIL",
				vs->orbit.t0, rr / F->soi, vs->orbit.e);
	}
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}

/* test-approach: closest approach of two paths. Two circular, coplanar
 * orbits a phase angle apart pass closest when the inner one has gained
 * that angle (t = phase / (n1 - n2)), at r2 - r1, with the difference of
 * their speeds; two equal circles in different planes, timed to reach the
 * line of nodes together, meet there. */
static trs_patch ca_patch(int body, double t1) {
	trs_patch p;
	memset(&p, 0, sizeof p);
	p.body = body, p.t1 = t1, p.end = TRS_PATCH_END_HORIZON, p.node = -1;
	return p;
}

trs_gate_status trs_gate_approach(trs_log *log, const char *data_dir) {
	trs_system sys;
	if (!load_system(log, data_dir, &sys))
		return TRS_GATE_FAIL;
	int floe = trs_system_find(&sys, "floe");
	double mu = sys.bodies[floe].mu, R = sys.bodies[floe].radius;
	int failed = 0;
	{
		double r1 = R + 100e3, r2 = R + 120e3, phase = 40 * DEG;
		trs_patch a = ca_patch(floe, 1e6), b = a;
		trs_orbit_from_elements(&a.orbit, mu, r1, 0, 0, 0, 0, 0, 0);
		trs_orbit_from_elements(&b.orbit, mu, r2, 0, 0, 0, 0, phase, 0);
		double n1 = sqrt(mu / (r1 * r1 * r1)), n2 = sqrt(mu / (r2 * r2 * r2));
		double want_t = phase / (n1 - n2), want_v = sqrt(mu / r1) - sqrt(mu / r2);
		trs_approach ap;
		int got = trs_closest_approach(&a, 1, &b, 1, 0, want_t * 1.5, &ap);
		int ok = got && fabs(ap.t - want_t) < 0.05 && fabs(ap.dist - (r2 - r1)) < 1e-3 && fabs(ap.rel_speed - want_v) < 1e-6;
		failed += !ok;
		trs_log_printf(log, "%s coplanar circles 20 km apart, 40 deg ahead: closest at t %.3f s (want %.3f), %.4f m (want %.4f), %.4f m/s (want %.4f)\n",
				ok ? "ok  " : "FAIL", ap.t, want_t, ap.dist, r2 - r1, ap.rel_speed, want_v);
	}
	{
		double r = R + 150e3;
		trs_patch a = ca_patch(floe, 1e6), b = a;
		/* both start a quarter turn before the ascending node (along +X) */
		trs_orbit_from_elements(&a.orbit, mu, r, 0, 0, 0, 0, -90 * DEG, 0);
		trs_orbit_from_elements(&b.orbit, mu, r, 0, 30 * DEG, 0, 0, -90 * DEG, 0);
		double n = sqrt(mu / (r * r * r)), want_t = 0.5 * TRS_PI / n;
		trs_approach ap;
		int got = trs_closest_approach(&a, 1, &b, 1, 0, want_t * 1.8, &ap);
		int ok = got && fabs(ap.t - want_t) < 0.05 && ap.dist < 1.0;
		failed += !ok;
		trs_log_printf(log, "%s equal circles 30 deg apart in inclination: meet at the node at t %.3f s (want %.3f), %.4f m apart, %.1f m/s\n",
				ok ? "ok  " : "FAIL", ap.t, want_t, ap.dist, ap.rel_speed);
	}
	{
		trs_patch a = ca_patch(floe, 100), b = a;
		b.body = floe + 1;
		trs_approach ap;
		int got = trs_closest_approach(&a, 1, &b, 1, 0, 100, &ap);
		failed += got;
		trs_log_printf(log, "%s paths around different bodies: no approach reported\n", got ? "FAIL" : "ok  ");
	}
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
