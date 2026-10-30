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

/* test-eva: penguins out of the pod. Walking runs at the speed asked for
 * (up to the penguin's top speed) and stops when let go, standing upright;
 * a jump on Krill rises as high as its take-off speed says; climbing out
 * of a pod on the ground puts the penguin beside it unhurt, within reach
 * of the hatch until it walks off; the jetpack's push matches its thrust
 * and burns mono at its Isp. */
#include "trs/gate.h"
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int trs_gate_load_catalog(trs_log *out, const char *data_dir, trs_part_catalog *cat);

static int load_system(trs_log *out, const char *data_dir, trs_system *sys) {
	char path[1024], err[256];
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_system_load(sys, path, err, sizeof err)) {
		trs_log_printf(out, "cannot load system: %s\n", err);
		return 0;
	}
	return 1;
}

/* a craft standing on body `bi` at the point under +X, its bottom `clear` m up */
static int stand(trs_world *w, const trs_craft *c, int bi, double half_height, double clear) {
	const trs_body *b = &w->sys.bodies[bi];
	double ground = trs_body_terrain_at(b, w->t, v3(b->radius, 0, 0));
	trs_v3 r = v3(b->radius + ground + half_height + clear, 0, 0);
	trs_v3 v = v3_cross(trs_body_omega(b), r);
	int id = trs_world_add_craft(w, c, bi, r, v, quat_from_to(v3(0, 1, 0), v3(1, 0, 0)));
	w->vessels[id].w = trs_body_omega(b);
	return id;
}

static void ground_vel(const trs_world *w, int id, double *east, double *north, double *up_v) {
	const trs_vessel *vs = &w->vessels[id];
	trs_v3 u, n, e;
	trs_local_frame(vs->r, &u, &n, &e);
	trs_v3 rel = v3_sub(vs->v, v3_cross(trs_body_omega(&w->sys.bodies[vs->body]), vs->r));
	*east = v3_dot(rel, e), *north = v3_dot(rel, n), *up_v = v3_dot(rel, u);
}

static void run(trs_world *w, double s) {
	for (int k = 0; k < (int)lround(s * TRS_PHYSICS_HZ); k++)
		trs_world_step_physics(w);
}

/* mean ground speed east / north over the next `secs` (from the body-fixed
 * displacement, so tick-to-tick jitter of the contact averages out) */
static void mean_walk(trs_world *w, int id, double secs, double *east, double *north) {
	const trs_vessel *vs = &w->vessels[id];
	const trs_body *b = &w->sys.bodies[vs->body];
	trs_v3 p0 = trs_body_to_fixed(b, w->t, vs->r);
	run(w, secs);
	trs_v3 p1 = trs_body_to_fixed(b, w->t, vs->r);
	trs_v3 u, n, e;
	trs_local_frame(p0, &u, &n, &e); /* the fixed frame shares the body's axis */
	trs_v3 d = v3_scale(v3_sub(p1, p0), 1.0 / secs);
	*east = v3_dot(d, e), *north = v3_dot(d, n);
}

static double tilt_deg(const trs_vessel *vs) {
	return acos(fmin(1, v3_dot(quat_rotate(vs->q, v3(0, 1, 0)), v3_norm(vs->r)))) * 180 / TRS_PI;
}


trs_gate_status trs_gate_eva(trs_log *out, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	if (!trs_gate_load_catalog(out, data_dir, &cat) || !load_system(out, data_dir, &sys))
		return TRS_GATE_FAIL;
	int floe = trs_system_find(&sys, "floe"), krill = trs_system_find(&sys, "krill");
	int pen = trs_catalog_find(&cat, "eva_penguin");
	const trs_part_def *pd = &cat.defs[pen];
	int failed = 0;
	trs_craft pc;
	trs_craft_init(&pc, &cat);
	trs_craft_add(&pc, pen, -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);

	/* 0. SAS radial-out points away from the body (penguins stand on it) */
	{
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		const trs_body *F = &sys.bodies[floe];
		trs_v3 r = v3(F->radius + 100000, 0, 0);
		int id = trs_world_add_craft(w, &pc, floe, r, v3(30, sqrt(F->mu / v3_len(r)), 0), quat_identity());
		double up_out = v3_dot(trs_sas_direction(w, id, TRS_SAS_RADIAL_OUT), v3_norm(r));
		double up_in = v3_dot(trs_sas_direction(w, id, TRS_SAS_RADIAL_IN), v3_norm(r));
		int ok = up_out > 0.99 && up_in < -0.99;
		failed += !ok;
		trs_log_printf(out, "%s SAS radial out . up = %+.4f, radial in . up = %+.4f\n", ok ? "ok  " : "FAIL", up_out, up_in);
		trs_world_free(w);
		free(w);
	}
	/* 1. walking on Floe: east at 1 m/s, then flat out, then let go */
	{
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = stand(w, &pc, floe, pd->height * 0.5, 0.05);
		trs_vessel *vs = &w->vessels[id];
		vs->sas = TRS_SAS_UPRIGHT;
		run(w, 2);
		vs->walk = v3(1.0, 0, 0);
		double worst_tilt = 0;
		for (int k = 0; k < 3 * TRS_PHYSICS_HZ; k++) {
			trs_world_step_physics(w);
			worst_tilt = fmax(worst_tilt, tilt_deg(vs));
		}
		double e1, n1;
		mean_walk(w, id, 2, &e1, &n1);
		vs->walk = v3(0, 30, 0);
		for (int k = 0; k < 3 * TRS_PHYSICS_HZ; k++) {
			trs_world_step_physics(w);
			worst_tilt = fmax(worst_tilt, tilt_deg(vs));
		}
		double e2, n2;
		mean_walk(w, id, 2, &e2, &n2);
		vs->walk = v3(0, 0, 0);
		run(w, 1.5);
		double e3, n3, u3;
		ground_vel(w, id, &e3, &n3, &u3);
		int ok1 = fabs(e1 - 1.0) < 0.03 && fabs(n1) < 0.03;
		int ok2 = fabs(n2 - pd->eva_walk) < 0.05 && fabs(e2) < 0.05;
		int ok3 = sqrt(e3 * e3 + n3 * n3) < 0.05 && worst_tilt < 20 && vs->alive && vs->contacts > 0;
		failed += !(ok1 && ok2 && ok3);
		trs_log_printf(out, "%s walking east at 1 m/s on Floe: averages %.3f m/s east, %.3f north\n", ok1 ? "ok  " : "FAIL", e1, n1);
		trs_log_printf(out, "%s asking 30 m/s north: %.3f m/s (top speed %.1f)\n", ok2 ? "ok  " : "FAIL", n2, pd->eva_walk);
		trs_log_printf(out, "%s let go: %.3f m/s after 1.5 s; leaning at most %.1f deg into the starts, still standing\n", ok3 ? "ok  " : "FAIL",
				sqrt(e3 * e3 + n3 * n3), worst_tilt);
		trs_world_free(w);
		free(w);
	}
	/* 2. a jump on Krill rises v^2 / 2g (contact springs aside) */
	{
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = stand(w, &pc, krill, pd->height * 0.5, 0.02);
		trs_vessel *vs = &w->vessels[id];
		vs->sas = TRS_SAS_UPRIGHT;
		run(w, 3);
		double h0 = v3_len(vs->r), top = h0;
		vs->jump = 1;
		double air = 0;
		int left = 0;
		for (int k = 0; k < 20 * TRS_PHYSICS_HZ; k++) {
			trs_world_step_physics(w);
			top = fmax(top, v3_len(vs->r));
			if (!vs->contacts)
				left = 1, air += TRS_PHYSICS_DT;
			else if (left)
				break;
		}
		const trs_body *K = &sys.bodies[krill];
		double g = K->mu / (h0 * h0), want = pd->eva_jump * pd->eva_jump / (2 * g);
		int ok = fabs((top - h0) / want - 1) < 0.08 && vs->alive;
		failed += !ok;
		trs_log_printf(out, "%s jump on Krill at %.1f m/s (g %.2f): rose %.2f m (v^2/2g %.2f), %.1f s in the air\n", ok ? "ok  " : "FAIL",
				pd->eva_jump, g, top - h0, want, air);
		trs_world_free(w);
		free(w);
	}
	/* 3b. out of a pod high on a tall stack standing on the ground: with no
	 *     ladder the penguin drops from the hatch; with one aboard it climbs
	 *     down it and stands at its foot (the ladder round the far side
	 *     from the hatch: not straight down from the hatch) */
	{
		double impact[2] = { 0, 0 }, start[2] = { 0, 0 }, from_ladder = -1;
		int lost[2] = { 0, 0 }, stood[2] = { 0, 0 };
		for (int lad = 0; lad < 2; lad++) {
			trs_craft tall;
			trs_craft_init(&tall, &cat);
			int ti = trs_catalog_find(&cat, "tank_l"), pi = trs_catalog_find(&cat, "pod_1");
			double th = cat.defs[ti].height, ph = cat.defs[pi].height;
			trs_craft_add(&tall, ti, -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
			trs_craft_add(&tall, ti, 0, "bottom", "top", v3(0, th, 0), quat_identity(), -1, 0);
			int pod = trs_craft_add(&tall, pi, 1, "bottom", "top", v3(0, th * 1.5 + ph * 0.5, 0), quat_identity(), -1, 0);
			int li = -1;
			if (lad)
				li = trs_craft_add(&tall, trs_catalog_find(&cat, "ladder"), 0, "", "", v3(-cat.defs[ti].radius - 0.05, 0, 0),
						quat_axis_angle(v3(0, 1, 0), -TRS_PI / 2), -1, 0);
			trs_world *w = malloc(sizeof(trs_world));
			trs_world_init(w, &sys, 0);
			int tid = stand(w, &tall, floe, th * 0.5, 0.05);
			/* (placed by its centre of mass: up by that over the bottom tank's middle) */
			trs_vessel *tv = &w->vessels[tid];
			tv->r = v3_add(tv->r, v3_scale(v3_norm(tv->r), tv->cm.y));
			tv->sas = TRS_SAS_STABILITY;
			run(w, 3);
			const trs_body *F = &sys.bodies[floe];
			int eid = trs_world_eva_exit(w, tid, pod, pen);
			trs_vessel *ev = &w->vessels[eid];
			tv = &w->vessels[tid];
			start[lad] = v3_len(ev->r) - F->radius - trs_body_terrain_at(F, w->t, ev->r) - pd->height * 0.5;
			if (lad) {
				/* across the ground from the ladder's foot */
				trs_v3 lw = v3_add(tv->r, quat_rotate(tv->q, v3_sub(tv->craft->parts[li].pos, tv->cm)));
				trs_v3 gap = v3_sub(ev->r, lw), upv = v3_norm(ev->r);
				from_ladder = v3_len(v3_sub(gap, v3_scale(upv, v3_dot(gap, upv))));
			}
			for (int k = 0; k < 4 * TRS_PHYSICS_HZ; k++) {
				double vin = -v3_dot(v3_sub(ev->v, v3_cross(trs_body_omega(F), ev->r)), v3_norm(ev->r));
				trs_world_step_physics(w);
				ev = &w->vessels[eid];
				for (int e = 0; e < w->nevents; e++)
					lost[lad] += w->events[e].kind == TRS_WEV_PART_DESTROYED && !strcmp(w->events[e].part, "eva_penguin");
				w->nevents = 0;
				double feet = v3_len(ev->r) - F->radius - trs_body_terrain_at(F, w->t, ev->r) - pd->height * 0.5;
				if (feet < 0.1 && impact[lad] == 0)
					impact[lad] = fmax(vin, 1e-6);
				if (!ev->alive)
					break;
			}
			stood[lad] = ev->alive && ev->contacts > 0 && tilt_deg(ev) < 10;
			trs_world_free(w);
			free(w);
			trs_craft_free(&tall);
		}
		int ok = start[0] > 5 && impact[0] > 9 && start[1] < 0.1 && impact[1] < 1 && !lost[1] && stood[1] && from_ladder >= 0 && from_ladder < 1.0;
		failed += !ok;
		trs_log_printf(out, "%s out of a pod %.1f m up a standing stack: no ladder, it drops and meets the ground at %.1f m/s (%s); with a ladder it climbs down "
				"(feet %.2f m up as it steps off, %.2f m from the ladder's foot, %.2f m/s at the ground), standing %s\n", ok ? "ok  " : "FAIL", start[0], impact[0],
				lost[0] ? "lost" : "survives", start[1], from_ladder, impact[1], stood[1] ? "upright" : "NOT upright");
	}
	/* 3. out of a pod on the ground, within reach of its hatch, then away */
	{
		trs_craft pod;
		trs_craft_init(&pod, &cat);
		int pi = trs_catalog_find(&cat, "pod_1");
		trs_craft_add(&pod, pi, -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int pid = stand(w, &pod, floe, cat.defs[pi].height * 0.5, 0.05);
		w->vessels[pid].sas = TRS_SAS_STABILITY;
		run(w, 3);
		int bad = trs_world_eva_exit(w, pid, 0, trs_catalog_find(&cat, "tank_s"));
		int eid = trs_world_eva_exit(w, pid, 0, pen);
		int lost = 0;
		for (int k = 0; k < 4 * TRS_PHYSICS_HZ; k++) {
			trs_world_step_physics(w);
			for (int e = 0; e < w->nevents; e++)
				lost += w->events[e].kind == TRS_WEV_PART_DESTROYED;
		}
		trs_vessel *ev = &w->vessels[eid];
		double gap;
		int hatch = trs_eva_hatch(w, eid, pid, &gap);
		double tl = tilt_deg(ev), sep = v3_len(v3_sub(ev->r, w->vessels[pid].r));
		int ok = bad < 0 && eid >= 0 && trs_vessel_is_eva(ev) && !lost && ev->contacts > 0 && tl < 10 && hatch == 0 && gap < pd->eva_reach;
		failed += !ok;
		trs_log_printf(out, "%s out of a pod on the ground: standing %.2f m from its centre (tilt %.1f deg), %d parts lost, hatch %.2f m away (reach %.1f); a tank can't go EVA (%d)\n",
				ok ? "ok  " : "FAIL", sep, tl, lost, gap, pd->eva_reach, bad);
		ev->walk = v3(0, 1.5, 0);
		run(w, 3);
		ev->walk = v3(0, 0, 0);
		run(w, 1);
		int far = trs_eva_hatch(w, eid, pid, &gap);
		ok = far < 0 && gap > pd->eva_reach;
		failed += !ok;
		trs_log_printf(out, "%s walked off north: hatch %.2f m away, out of reach\n", ok ? "ok  " : "FAIL", gap);
		trs_world_free(w);
		free(w);
		trs_craft_free(&pod);
	}
	/* 3b. a pod high on a tall tank: out of reach from the ground, until a
	 *     ladder runs down the tank to where the penguin stands */
	for (int with_ladder = 0; with_ladder < 2; with_ladder++) {
		trs_craft t;
		trs_craft_init(&t, &cat);
		int pi = trs_catalog_find(&cat, "pod_1"), ti = trs_catalog_find(&cat, "tank_l"), li = trs_catalog_find(&cat, "ladder");
		const trs_part_def *pd1 = &cat.defs[pi], *td = &cat.defs[ti], *ld = &cat.defs[li];
		trs_craft_add(&t, ti, -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
		trs_craft_add(&t, pi, 0, "bottom", "top", v3(0, td->height * 0.5 + pd1->height * 0.5, 0), quat_identity(), -1, 0);
		if (with_ladder) /* on the tank's side, its foot level with the tank's */
			trs_craft_add(&t, li, 0, "", "", v3(0, -td->height * 0.5 + ld->height * 0.5, td->radius), quat_identity(), -1, 0);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int tid = stand(w, &t, floe, td->height * 0.5, 0.05);
		w->vessels[tid].sas = TRS_SAS_STABILITY;
		run(w, 2);
		/* a penguin on the ground beside the tank, on the ladder's side */
		const trs_body *F = &sys.bodies[floe];
		trs_v3 up = v3_norm(w->vessels[tid].r), side = quat_rotate(w->vessels[tid].q, v3(0, 0, 1));
		side = v3_norm(v3_sub(side, v3_scale(up, v3_dot(side, up))));
		trs_v3 at = v3_add(w->vessels[tid].r, v3_scale(side, td->radius + pd->radius + 0.3));
		double ground = trs_body_terrain_at(F, w->t, at);
		at = v3_scale(v3_norm(at), F->radius + ground + pd->height * 0.5 + 0.02);
		int pid = trs_world_add_craft(w, &pc, floe, at, v3_cross(trs_body_omega(F), at), quat_from_to(v3(0, 1, 0), v3_norm(at)));
		w->vessels[pid].sas = TRS_SAS_UPRIGHT;
		run(w, 1);
		double gap;
		int hatch = trs_eva_hatch(w, pid, tid, &gap);
		int ok = with_ladder ? hatch == 1 : hatch < 0;
		failed += !ok;
		trs_log_printf(out, "%s pod 3.75 m up a tank, penguin on the ground beside it%s: %s (%.2f m to the nearest of pod%s)\n", ok ? "ok  " : "FAIL",
				with_ladder ? ", ladder down the tank" : "", hatch >= 0 ? "can climb in" : "out of reach", gap, with_ladder ? " and ladder" : "");
		trs_world_free(w);
		free(w);
		trs_craft_free(&t);
	}
	/* 3c. a flag planted on Floe stays where it stands: anchored, it can't
	 *     be put into physics (nothing knocks it over), it survives the
	 *     world save, and a penguin in the air can't plant one */
	{
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = stand(w, &pc, floe, pd->height * 0.5, 0.05);
		w->vessels[id].sas = TRS_SAS_UPRIGHT;
		run(w, 2);
		int fdef = trs_catalog_find(&cat, "flag");
		int fid = trs_world_plant_flag(w, id, fdef);
		trs_vessel *fv = &w->vessels[fid];
		const trs_body *F = &sys.bodies[floe];
		trs_v3 fixed0 = fv->fixed_r;
		trs_vessel_set_mode(w, fid, TRS_MODE_PHYSICS);
		run(w, 5);
		trs_v3 fr, fvv;
		trs_vessel_state(w, fid, &fr, &fvv);
		double above = v3_len(fr) - F->radius - trs_body_terrain_at(F, w->t, fr) - cat.defs[fdef].height * 0.5;
		double dist = v3_len(v3_sub(fr, w->vessels[id].r));
		trs_json *wj = trs_world_to_json(w);
		trs_world *w2 = malloc(sizeof(trs_world));
		trs_world_init(w2, &sys, 0);
		char err[256];
		int back = trs_world_from_json(w2, &cat, wj, err, sizeof err) && w2->vessels[fid].anchored && w2->vessels[fid].mode == TRS_MODE_LANDED;
		trs_json_free(wj);
		trs_world_free(w2);
		free(w2);
		w->vessels[id].jump = 1;
		run(w, 0.2);
		int midair = trs_world_plant_flag(w, id, fdef);
		int ok = fid >= 0 && fv->mode == TRS_MODE_LANDED && fv->anchored && v3_len(v3_sub(fv->fixed_r, fixed0)) == 0 && fabs(above) < 0.01 &&
				dist > 0.5 && dist < 1.2 && back && midair < 0;
		failed += !ok;
		trs_log_printf(out, "%s flag planted %.2f m from the penguin, its foot %.3f m off the ground; still LANDED and unmoved after a request for physics; kept through the save %s; none while jumping (%d)\n",
				ok ? "ok  " : "FAIL", dist, above, back ? "yes" : "no", midair);
		trs_world_free(w);
		free(w);
	}
	/* 4. the jetpack in deep space: dv = F t / m, mono = F t / (Isp g0) */
	{
		trs_system vsys;
		char err[256];
		trs_system_load_json(&vsys, "{\"name\":\"void\",\"bodies\":[{\"id\":\"void\",\"mu\":1e-6,\"radius\":1}]}", err, sizeof err);
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &vsys, 0);
		int id = trs_world_add_craft(w, &pc, 0, v3(1e7, 0, 0), v3(0, 0, 0), quat_identity());
		trs_vessel *vs = &w->vessels[id];
		vs->sas = TRS_SAS_OFF;
		vs->rcs = 1;
		vs->translate = v3(0, 0, 1);
		double m0 = vs->mass, mono0 = trs_craft_mono(vs->craft, NULL);
		run(w, 2);
		double dv = v3_len(vs->v), used = mono0 - trs_craft_mono(vs->craft, NULL);
		double want_mono = pd->rcs_thrust * 2 / (pd->rcs_isp_vac * TRS_G0);
		double want_dv = pd->rcs_isp_vac * TRS_G0 * log(m0 / (m0 - want_mono));
		int ok = fabs(dv / want_dv - 1) < 1e-3 && fabs(used / want_mono - 1) < 1e-3;
		failed += !ok;
		trs_log_printf(out, "%s jetpack 2 s forward in deep space: %.4f m/s (rocket equation %.4f), %.4f kg mono (%.4f)\n",
				ok ? "ok  " : "FAIL", dv, want_dv, used, want_mono);
		trs_world_free(w);
		free(w);
	}
	trs_craft_free(&pc);
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
