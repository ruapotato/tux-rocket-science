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

/* test-interior: penguins indoors feel what anyone aboard would.
 *   1. under thrust a floating penguin falls toward the stern at exactly
 *      thrust / mass, lands on the aft wall when the fall says, and stands
 *      there weighing that;
 *   2. in a ring spinning at w, a penguin let go at rest flies the straight
 *      line it really flies (seen from the ring: curving back, Coriolis),
 *      reaches the deck when that line does, and stands there weighing
 *      w^2 r; it can walk across the deck to the side wall;
 *   3. the spin hold spins a ring ship up to its design rate in the time
 *      its torque gives, without the nose wandering off;
 *   4. the Stillwater Drive (1 g in Austra's heliopause, 100 g between the
 *      stars) moves nobody aboard: nothing felt, nothing drifts;
 *   5. on the pad the floor weighs what the ground does;
 *   6. rooms that meet are one space (stacked habs, a junction's side
 *      hatch); a room against a tank is walled off; the walls drawn have
 *      no wall where two rooms meet;
 *   7. the world save keeps a penguin indoors mid-flight: it steps on
 *      exactly as before. */
#include "trs/gate.h"
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int trs_gate_load_catalog(trs_log *out, const char *data_dir, trs_part_catalog *cat);

static const trs_part_catalog *CAT;

static double height_of(const char *id) {
	return CAT->defs[trs_catalog_find(CAT, id)].height;
}

/* part `id` stacked under (below < 0) or over part `parent` */
static int stack(trs_craft *c, const char *id, int parent, int below) {
	int def = trs_catalog_find(CAT, id);
	if (parent < 0)
		return trs_craft_add(c, def, -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	const trs_part *p = &c->parts[parent];
	double y = p->pos.y + (below ? -1 : 1) * 0.5 * (trs_craft_def(c, parent)->height + CAT->defs[def].height);
	return trs_craft_add(c, def, parent, below ? "top" : "bottom", below ? "bottom" : "top", v3(p->pos.x, y, p->pos.z),
			quat_identity(), -1, 0);
}

static int find_uid(const trs_vessel *vs, int part) {
	return vs->craft->parts[part].uid;
}

/* a free-flying vessel well clear of Floe, at rest in orbit */
static int in_orbit(trs_world *w, const trs_craft *c, int floe) {
	const trs_body *F = &w->sys.bodies[floe];
	double r = F->radius + 2e6;
	int id = trs_world_add_craft(w, c, floe, v3(r, 0, 0), v3(0, sqrt(F->mu / r), 0), quat_identity());
	w->vessels[id].sas = TRS_SAS_OFF;
	return id;
}

trs_gate_status trs_gate_interior(trs_log *out, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	char path[1024], err[256];
	if (!trs_gate_load_catalog(out, data_dir, &cat))
		return TRS_GATE_FAIL;
	CAT = &cat;
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_system_load(&sys, path, err, sizeof err)) {
		trs_log_printf(out, "cannot load system: %s\n", err);
		return TRS_GATE_FAIL;
	}
	int floe = trs_system_find(&sys, "floe"), austra = trs_system_find(&sys, "austra");
	const double h = TRS_PHYSICS_DT;
	const double rad = trs_penguin_room_radius(&cat);
	int failed = 0;
	trs_world *w = malloc(sizeof(trs_world));

	/* 1. thrust is weight */
	{
		trs_world_init(w, &sys, 0);
		trs_craft c;
		trs_craft_init(&c, &cat);
		int hab = stack(&c, "hab_burrow", -1, 0);
		int tank = stack(&c, "tank_xl", hab, 1);
		int eng = stack(&c, "engine_big", tank, 1);
		c.parts[eng].limit = 0.04;
		c.parts[eng].engine_on = 1;
		int id = in_orbit(w, &c, floe);
		trs_vessel *vs = &w->vessels[id];
		vs->throttle = 1;
		trs_walker_enter(w, id, hab);
		trs_v3 x0 = vs->walker.x;
		double fall = x0.y - (c.parts[hab].pos.y - 0.5 * height_of("hab_burrow") + rad); /* to the aft wall */
		/* the fall as thrust / mass gives it, tick by tick */
		double y = 0, vy = 0, t_land = -1, worst = 0, t = 0;
		int landed_tick = -1;
		for (int k = 0; k < 400; k++) {
			trs_world_step_physics(w);
			double a = v3_len(vs->sensed_accel);
			y += vy * h + 0.5 * a * h * h;
			vy += a * h;
			t += h;
			if (t_land < 0 && y >= fall)
				t_land = t - (y - fall) / vy;
			if (landed_tick < 0 && vs->walker.touching)
				landed_tick = k;
			if (t_land < 0)
				worst = fmax(worst, fabs((x0.y - vs->walker.x.y) - y));
		}
		double a = v3_len(vs->sensed_accel), felt = v3_len(vs->walker.felt);
		double t_seen = (landed_tick + 1) * h;
		int ok = worst < 2e-4 && fabs(t_seen - t_land) <= 1.5 * h && vs->walker.standing && fabs(felt - a) < 1e-9 * a &&
			 vs->walker.felt.y < 0 && fabs(vs->walker.x.y - (x0.y - fall)) < 1e-3;
		failed += !ok;
		trs_log_printf(out, "%s thrust %.3f m/s2: falls %.3f m to the aft wall, path within %.1e m of the free fall, lands at %.3f s (fall says %.3f), stands feeling %.4f g (thrust/mass %.4f g)\n",
				ok ? "ok  " : "FAIL", a, fall, worst, t_seen, t_land, felt / TRS_G0, a / TRS_G0);
		trs_world_free(w);
		trs_craft_free(&c);
	}

	/* 2. a spinning ring: the straight line seen from inside, then weight w^2 r */
	{
		trs_world_init(w, &sys, 0);
		trs_craft c;
		trs_craft_init(&c, &cat);
		int ring = stack(&c, "spin_ring", -1, 0);
		const trs_part_def *rd = trs_craft_def(&c, ring);
		int id = in_orbit(w, &c, floe);
		trs_vessel *vs = &w->vessels[id];
		double om = rd->spin_rate;
		vs->w = v3(0, om, 0);
		trs_walker_enter(w, id, ring);
		trs_v3 x0 = vs->walker.x;
		double r0 = hypot(x0.x, x0.z), R = rd->volumes[0].r1 - rad;
		double s = sqrt(R * R - r0 * r0), t_hit = s / (om * r0);
		trs_quat q0 = vs->q;
		double worst = 0, t = 0, t_seen = -1, ang = 0;
		for (int k = 0; k < 250; k++) {
			trs_world_step_physics(w);
			t += h;
			if (t_seen < 0 && vs->walker.touching)
				t_seen = t, ang = atan2(-vs->walker.x.z, vs->walker.x.x);
			if (t < t_hit - h) {
				/* inertially: straight on at w r0 from where it was let go */
				trs_v3 p = v3_add(quat_rotate(q0, x0), v3_scale(v3_cross(v3(0, om, 0), quat_rotate(q0, x0)), t));
				trs_v3 want = quat_rotate(quat_conj(vs->q), p);
				worst = fmax(worst, v3_len(v3_sub(want, vs->walker.x)));
			}
		}
		double rho = hypot(vs->walker.x.x, vs->walker.x.z), felt = v3_len(vs->walker.felt), wnow = quat_rotate(quat_conj(vs->q), vs->w).y;
		/* where it lands, seen from the ring: ahead by the line's angle, back by the ring's turn */
		double want_ang = atan2(s, r0) - om * t_hit;
		int ok = worst < 1e-6 && fabs(t_seen - t_hit) <= 1.5 * h && vs->walker.standing && fabs(rho - R) < 2e-3 &&
			 fabs(felt - wnow * wnow * rho) < 1e-6 * felt && fabs(ang - want_ang) < 0.01;
		failed += !ok;
		trs_log_printf(out, "%s ring at %.2f rpm: let go %.2f m out, flies within %.1e m of its straight line, reaches the deck at %.3f s (line says %.3f) %.2f deg %s, stands %.3f m out feeling %.4f g (w^2 r %.4f g)\n",
				ok ? "ok  " : "FAIL", om * 60 / (2 * TRS_PI), r0, worst, t_seen, t_hit, fabs(ang) * 180 / TRS_PI, ang < 0 ? "behind" : "ahead",
				rho, felt / TRS_G0, wnow * wnow * rho / TRS_G0);
		/* walking across the deck to the side wall */
		trs_walker_command(w, id, v3(0, 1.2, 0), 0);
		for (int k = 0; k < 250; k++)
			trs_world_step_physics(w);
		double side = rd->volumes[0].y1 - rad;
		ok = fabs(vs->walker.x.y - side) < 2e-3 && vs->walker.standing && fabs(hypot(vs->walker.x.x, vs->walker.x.z) - R) < 2e-3;
		failed += !ok;
		trs_log_printf(out, "%s walks across the deck: at the side wall %.3f m off the middle (wall %.3f), still on its feet\n", ok ? "ok  " : "FAIL",
				vs->walker.x.y, side);
		trs_world_free(w);
		trs_craft_free(&c);
	}

	/* 3. the spin hold */
	{
		trs_world_init(w, &sys, 0);
		trs_craft c;
		trs_craft_init(&c, &cat);
		int hab = stack(&c, "hab_burrow", -1, 0);
		int ring = stack(&c, "spin_ring", hab, 1);
		stack(&c, "hab_burrow", ring, 1);
		int id = in_orbit(w, &c, floe);
		trs_vessel *vs = &w->vessels[id];
		vs->sas = TRS_SAS_STABILITY;
		vs->sas_hold = vs->q;
		double want = trs_craft_def(&c, ring)->spin_rate;
		vs->spin = want;
		trs_vessel_refresh(w, id);
		double Iy = vs->inertia.m[1][1], torque = 0;
		for (int i = 0; i < c.count; i++)
			torque += trs_craft_def(&c, i)->torque;
		double t = 0, t_spun = -1, drift = 0, mono0 = trs_craft_mono(vs->craft, NULL);
		for (int k = 0; k < 50 * 400 && t_spun < 0; k++) {
			trs_world_step_physics(w);
			t += h;
			double wy = quat_rotate(quat_conj(vs->q), vs->w).y;
			drift = fmax(drift, acos(fmin(1, v3_dot(quat_rotate(vs->q, v3(0, 1, 0)), v3(0, 1, 0)))));
			if (fabs(wy - want) < 1e-3 * want)
				t_spun = t;
		}
		double ideal = Iy * want / torque;
		int ok = t_spun > 0 && t_spun >= ideal * 0.999 && t_spun < ideal * 1.1 && drift < 0.5 * TRS_PI / 180 &&
			 trs_craft_mono(vs->craft, NULL) == mono0;
		failed += !ok;
		trs_log_printf(out, "%s spin hold: %.0f kN m spins %.0f t up to %.2f rpm in %.1f s (I w / torque %.1f s), nose held within %.3f deg, no monopropellant spent\n",
				ok ? "ok  " : "FAIL", torque / 1000, vs->mass / 1000, want * 60 / (2 * TRS_PI), t_spun, ideal, drift * 180 / TRS_PI);
		trs_world_free(w);
		trs_craft_free(&c);
	}

	/* 4. the Stillwater Drive is felt by nobody */
	{
		trs_craft c;
		trs_craft_init(&c, &cat);
		int core = stack(&c, "hearth_core", -1, 0);
		int hab = stack(&c, "hab_burrow", core, 0);
		stack(&c, "stillwater_drive", core, 1);
		const trs_body *A = &sys.bodies[austra], *F = &sys.bodies[floe];
		for (int between = 0; between < 2; between++) {
			trs_world_init(w, &sys, 0);
			trs_v3 fr, fv;
			trs_body_state_abs(&sys, floe, 0, &fr, &fv);
			int id = between ? trs_world_add_craft(w, &c, austra, v3(A->soi * 1.01, 0, 0), v3(0, 0, 0), quat_from_to(v3(0, 1, 0), v3(1, 0, 0)))
					 : trs_world_add_craft(w, &c, austra, v3_add(fr, v3(0, 0, F->soi * 3)), fv, quat_identity());
			trs_vessel *vs = &w->vessels[id];
			vs->sas = TRS_SAS_OFF;
			trs_world_step_physics(w); /* (out past the heliopause: between the stars now) */
			trs_walker_enter(w, id, hab);
			trs_world_set_drive(w, id, 1.0);
			trs_v3 x0 = vs->walker.x;
			double push = 0, felt = 0;
			for (int k = 0; k < 100; k++) {
				trs_world_step_physics(w);
				push = fmax(push, vs->drive_accel);
				felt = fmax(felt, v3_len(vs->walker.felt));
			}
			double moved = v3_len(v3_sub(vs->walker.x, x0));
			int ok = push > (between ? 900 : 9) && felt < 1e-9 && moved < 1e-9 && (vs->mode == TRS_MODE_INTERSTELLAR) == between;
			failed += !ok;
			trs_log_printf(out, "%s Stillwater Drive %s: pushing %.2f m/s2 for 2 s, the penguin feels %.1e m/s2 and drifts %.1e m\n",
					ok ? "ok  " : "FAIL", between ? "between the stars" : "inside the heliopause", push, felt, moved);
			trs_world_free(w);
		}
		trs_craft_free(&c);
	}

	/* 5. on the pad */
	{
		trs_world_init(w, &sys, 0);
		trs_craft c;
		trs_craft_init(&c, &cat);
		int hab = stack(&c, "hab_burrow", -1, 0);
		const trs_body *F = &sys.bodies[floe];
		int id = trs_world_launch(w, &c, floe, F->site_lat, F->site_lon);
		trs_vessel *vs = &w->vessels[id];
		trs_walker_enter(w, id, hab);
		for (int k = 0; k < 150; k++)
			trs_world_step_physics(w);
		trs_v3 r = quat_rotate(trs_body_quat(F, w->t), vs->fixed_r), wb = trs_body_omega(F);
		double rl = v3_len(r);
		trs_v3 g_eff = v3_sub(v3_scale(r, -F->mu / (rl * rl * rl)), v3_cross(wb, v3_cross(wb, r)));
		/* and the penguin, a metre off the craft's centre, is carried round too */
		trs_quat qn = quat_normalize(quat_mul(trs_body_quat(F, w->t), vs->fixed_q));
		g_eff = v3_sub(g_eff, v3_cross(wb, v3_cross(wb, quat_rotate(qn, v3_sub(vs->walker.x, vs->cm)))));
		double felt = v3_len(vs->walker.felt);
		trs_v3 down = quat_rotate(qn, v3_scale(vs->walker.felt, 1 / felt));
		double floor_y = c.parts[hab].pos.y - 0.5 * height_of("hab_burrow") + rad;
		int ok = vs->mode == TRS_MODE_LANDED && fabs(felt - v3_len(g_eff)) < 1e-9 * felt && v3_dot(down, v3_norm(g_eff)) > 1 - 1e-12 &&
			 vs->walker.standing && fabs(vs->walker.x.y - floor_y) < 2e-3;
		failed += !ok;
		trs_log_printf(out, "%s on the pad: stands on the floor feeling %.5f m/s2, the ground's pull less its turning %.5f, straight down\n",
				ok ? "ok  " : "FAIL", felt, v3_len(g_eff));
		trs_world_free(w);
		trs_craft_free(&c);
	}

	/* 5b. even the Pebble-1: one penguin, hunched, on its floor */
	{
		trs_world_init(w, &sys, 0);
		trs_craft c;
		trs_craft_init(&c, &cat);
		int pod = stack(&c, "pod_1", -1, 0);
		const trs_body *F = &sys.bodies[floe];
		int id = trs_world_launch(w, &c, floe, F->site_lat, F->site_lon);
		trs_vessel *vs = &w->vessels[id];
		int in = trs_walker_enter(w, id, pod);
		for (int k = 0; k < 150; k++)
			trs_world_step_physics(w);
		const trs_volume *v = &trs_craft_def(vs->craft, pod)->volumes[0];
		double depth = trs_craft_room_depth(vs->craft, NULL, 0, vs->walker.x);
		int ok = in && vs->walker.standing && depth >= rad - 2e-3 && fabs(vs->walker.x.y - (v->y0 + rad)) < 2e-3;
		failed += !ok;
		trs_log_printf(out, "%s in a Pebble-1 on the pad: the penguin (a %.2f m ball, hunched) stands on its floor %.3f m off it, %.3f m clear of the walls\n",
				ok ? "ok  " : "FAIL", rad, vs->walker.x.y - v->y0, depth);
		trs_world_free(w);
		trs_craft_free(&c);
	}

	/* 6. rooms that meet are one space */
	{
		trs_craft c;
		trs_craft_init(&c, &cat);
		int a = stack(&c, "hab_burrow", -1, 0);
		int b = stack(&c, "hab_burrow", a, 0);
		stack(&c, "tank_xl", a, 1);
		int j = stack(&c, "junction", b, 0);
		/* a hab on the junction's east hatch, its bottom against it */
		const trs_part_def *jd = &cat.defs[trs_catalog_find(&cat, "junction")];
		int side = trs_craft_add(&c, trs_catalog_find(&cat, "hab_burrow"), j, "bottom", "east",
				v3_add(c.parts[j].pos, v3(jd->nodes[2].pos.x + 0.5 * height_of("hab_burrow"), 0, 0)),
				quat_from_to(v3(0, 1, 0), v3(1, 0, 0)), -1, 0);
		trs_world_init(w, &sys, 0);
		int id = in_orbit(w, &c, floe);
		trs_vessel *vs = &w->vessels[id];
		trs_walker_enter(w, id, a);
		/* paddling for the junction's middle, through the hab above */
		int up_ok = 0;
		trs_v3 jc = c.parts[j].pos, sc = c.parts[side].pos;
		for (int k = 0; k < 50 * 14; k++) {
			trs_v3 to = v3_sub(jc, vs->walker.x);
			trs_walker_command(w, id, v3_len(to) > 0.05 ? v3_norm(to) : v3(0, 0, 0), 0);
			trs_world_step_physics(w);
			up_ok |= vs->walker.uid == find_uid(vs, b);
		}
		int in_j = vs->walker.uid == find_uid(vs, j);
		double yb = vs->walker.x.y;
		/* then out its side hatch into the side hab */
		for (int k = 0; k < 50 * 8; k++) {
			trs_v3 to = v3_sub(sc, vs->walker.x);
			trs_walker_command(w, id, v3_len(to) > 0.05 ? v3_norm(to) : v3(0, 0, 0), 0);
			trs_world_step_physics(w);
		}
		int in_side = vs->walker.uid == find_uid(vs, side);
		double xs = vs->walker.x.x;
		/* back down to the bottom hab and on: the tank walls it off */
		trs_walker_leave(w, id);
		trs_walker_enter(w, id, a);
		trs_walker_command(w, id, v3(0, -1, 0), 0);
		for (int k = 0; k < 50 * 6; k++)
			trs_world_step_physics(w);
		double floor_y = c.parts[a].pos.y - 0.5 * height_of("hab_burrow") + rad;
		int walled = vs->walker.uid == find_uid(vs, a) && fabs(vs->walker.x.y - floor_y) < 0.011;
		/* no wall drawn where the habs meet; the end walls are drawn */
		int max = 200000;
		double *tri = malloc(sizeof(double) * 9 * (size_t)max);
		int *kind = malloc(sizeof(int) * (size_t)max);
		int n = trs_craft_room_mesh(vs->craft, 0.3, tri, kind, max);
		double joint = 0.5 * (c.parts[a].pos.y + c.parts[b].pos.y), bottom = c.parts[a].pos.y - 0.5 * height_of("hab_burrow");
		int at_joint = 0, at_bottom = 0, glass = 0;
		for (int t = 0; t < n; t++) {
			int flat_j = 1, flat_b = 1;
			for (int k = 0; k < 3; k++) {
				double x = tri[t * 9 + k * 3], yy = tri[t * 9 + k * 3 + 1], z = tri[t * 9 + k * 3 + 2];
				flat_j &= fabs(yy - joint) < 1e-9 && hypot(x, z) < 1.0;
				flat_b &= fabs(yy - bottom) < 1e-9;
			}
			at_joint += flat_j, at_bottom += flat_b, glass += kind[t] == 4;
		}
		free(tri);
		free(kind);
		int ok = up_ok && in_j && in_side && walled && at_joint == 0 && at_bottom > 0 && glass > 0;
		failed += !ok;
		trs_log_printf(out, "%s rooms: floats from hab to hab (%s), on into the junction (%s, %.2f m up) and out its side hatch into the side hab (%s, %.2f m out); "
				"walled off from the tank (%s); walls drawn: %d triangles, %d where the habs meet, %d on the bottom end, %d of glass\n",
				ok ? "ok  " : "FAIL", up_ok ? "yes" : "no", in_j ? "yes" : "no", yb, in_side ? "yes" : "no", xs, walled ? "yes" : "no", n, at_joint,
				at_bottom, glass);
		trs_world_free(w);
		trs_craft_free(&c);
	}

	/* 7. the world save keeps it */
	{
		trs_world_init(w, &sys, 0);
		trs_craft c;
		trs_craft_init(&c, &cat);
		int ring = stack(&c, "spin_ring", -1, 0);
		int id = in_orbit(w, &c, floe);
		w->vessels[id].w = v3(0, trs_craft_def(&c, ring)->spin_rate, 0);
		trs_walker_enter(w, id, ring);
		trs_walker_command(w, id, v3(0, 0.5, 0.8), 0);
		for (int k = 0; k < 20; k++)
			trs_world_step_physics(w);
		trs_json *j = trs_world_to_json(w);
		trs_world *w2 = malloc(sizeof(trs_world));
		trs_world_init(w2, &sys, 0);
		int got = trs_world_from_json(w2, &cat, j, err, sizeof err);
		trs_json_free(j);
		for (int k = 0; k < 100; k++) {
			trs_world_step_physics(w);
			if (got)
				trs_world_step_physics(w2);
		}
		const trs_walker *a = &w->vessels[id].walker, *b = &w2->vessels[id].walker;
		int ok = got && b->active && memcmp(&a->x, &b->x, sizeof a->x) == 0 && memcmp(&a->v, &b->v, sizeof a->v) == 0;
		failed += !ok;
		trs_log_printf(out, "%s saved mid-stride in the ring and loaded: steps on to the same place (%.6f, %.6f, %.6f)\n", ok ? "ok  " : "FAIL",
				b->x.x, b->x.y, b->x.z);
		trs_world_free(w2);
		free(w2);
		trs_world_free(w);
		trs_craft_free(&c);
	}

	free(w);
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
