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

/* test-relativity: the interstellar integrator against exact special
 * relativity. Rest to rest over 4.2 light-years at 100 g, turning round
 * halfway, ends where and when the closed form says, on both clocks;
 * 1 g for a ship's year reaches c tanh(a tau / c); coasting at 0.8 c the
 * ship's clock runs at 0.6 of the stars'; a push square to the motion
 * grows the proper velocity by a dtau; and stepping by coordinate time
 * lands on it. */
#include "trs/gate.h"
#include "trs/relativity.h"

#include <math.h>
#include <stdio.h>

#define G0 9.80665

trs_gate_status trs_gate_relativity(trs_log *out, const char *data_dir) {
	(void)data_dir;
	int failed = 0;

	/* 1. flip and burn */
	{
		double d = 4.2 * TRS_LIGHT_YEAR, a = 100 * G0, tau, t, vp;
		trs_rel_flip_burn(d, a, &tau, &t, &vp);
		trs_rel_state s = { v3(0, 0, 0), v3(0, 0, 0), 0, 0 };
		trs_rel_advance_tau(&s, v3(a, 0, 0), tau / 2);
		double v_turn = v3_len(trs_rel_velocity(s.u));
		trs_rel_advance_tau(&s, v3(-a, 0, 0), tau / 2);
		double ex = fabs(s.x.x - d) / d, et = fabs(s.t - t) / t, ev = v3_len(trs_rel_velocity(s.u));
		int ok = ex < 1e-9 && et < 1e-9 && ev < 0.01 && fabs(v_turn - vp) / vp < 1e-12;
		failed += !ok;
		trs_log_printf(out, "%s 4.2 ly at 100 g, turning halfway: ship %.3f d (want %.3f), home %.4f yr (want %.4f), peak %.9f c (want %.9f); "
				"ends %.3e of the way off, %.2e m/s\n", ok ? "ok  " : "FAIL", s.tau / 86400, tau / 86400, s.t / TRS_YEAR, t / TRS_YEAR,
				v_turn / TRS_C, vp / TRS_C, ex, ev);
	}
	/* 2. a ship's year at 1 g */
	{
		double a = G0;
		trs_rel_state s = { v3(0, 0, 0), v3(0, 0, 0), 0, 0 };
		trs_rel_advance_tau(&s, v3(0, a, 0), TRS_YEAR);
		double eta = a * TRS_YEAR / TRS_C;
		double v = v3_len(trs_rel_velocity(s.u)), want_v = TRS_C * tanh(eta);
		double want_t = TRS_C / a * sinh(eta), want_x = TRS_C * TRS_C / a * (cosh(eta) - 1);
		int ok = fabs(v - want_v) / want_v < 1e-10 && fabs(s.t - want_t) / want_t < 1e-10 && fabs(s.x.y - want_x) / want_x < 1e-10;
		failed += !ok;
		trs_log_printf(out, "%s a ship's year at 1 g: %.6f c (want %.6f), home %.6f yr (%.6f), %.6f ly gone (%.6f)\n", ok ? "ok  " : "FAIL",
				v / TRS_C, want_v / TRS_C, s.t / TRS_YEAR, want_t / TRS_YEAR, s.x.y / TRS_LIGHT_YEAR, want_x / TRS_LIGHT_YEAR);
	}
	/* 3. coasting at 0.8 c, and stepping by coordinate time */
	{
		trs_rel_state s = { v3(0, 0, 0), trs_rel_proper_velocity(v3(0.8 * TRS_C, 0, 0)), 0, 0 };
		trs_rel_advance_t(&s, v3(0, 0, 0), TRS_YEAR);
		double r = s.tau / s.t;
		trs_rel_state b = { v3(0, 0, 0), v3(0, 0, 0), 0, 0 };
		trs_rel_advance_t(&b, v3(50 * G0, 0, 0), TRS_YEAR);
		int ok = fabs(r - 0.6) < 1e-12 && fabs(s.x.x - 0.8 * TRS_C * TRS_YEAR) / (TRS_C * TRS_YEAR) < 1e-12 && fabs(b.t - TRS_YEAR) < 1e-3;
		failed += !ok;
		trs_log_printf(out, "%s coasting at 0.8 c for a home year: ship clock %.12f of home (want 0.6), %.6f ly; 50 g for a home year stops at %.6f s off (ship %.2f d)\n",
				ok ? "ok  " : "FAIL", r, s.x.x / TRS_LIGHT_YEAR, b.t - TRS_YEAR, b.tau / 86400);
	}
	/* 4. a push square to fast motion */
	{
		trs_rel_state s = { v3(0, 0, 0), trs_rel_proper_velocity(v3(0.9 * TRS_C, 0, 0)), 0, 0 };
		double ux = s.u.x;
		trs_rel_advance_tau(&s, v3(0, 10, 0), 1.0);
		int ok = fabs(s.u.y - 10.0) < 1e-9 && fabs(s.u.x - ux) / ux < 1e-12;
		failed += !ok;
		trs_log_printf(out, "%s 10 m/s2 square to 0.9 c for a ship second: proper velocity across +%.9f m/s (want 10), along unchanged\n",
				ok ? "ok  " : "FAIL", s.u.y);
	}
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
