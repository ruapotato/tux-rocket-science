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

/* test-wild: the animals near a penguin, and the lasers. An animal left
 * alone is exactly where the scenery draws it; a shy one runs from a
 * penguin at its run speed and calms down once well away; a curious one
 * comes up and stops at a penguin's feet; a hostile one charges and bites,
 * its bite's hp every BITE s and no oftener, until the penguin is lost to
 * it. A turret on guard shoots the hostile ones before they reach the
 * penguin beside it: each beam tick does dps x the tick, the batteries pay
 * kW x the time it fires, and a downed animal stays gone. The computer
 * won't fire through a penguin, a pilot by hand will; the ground stops a
 * beam; guard leaves alone what is out of reach. A saved world steps on
 * exactly as the one it was saved from. */
#include "trs/gate.h"
#include "trs/json.h"
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int trs_gate_load_catalog(trs_log *out, const char *data_dir, trs_part_catalog *cat);

static double frand(unsigned *s) {
	*s = *s * 1664525u + 1013904223u;
	return (*s >> 8) / 16777216.0;
}

static void run(trs_world *w, double s) {
	for (int k = 0; k < (int)lround(s * TRS_PHYSICS_HZ); k++)
		trs_world_step_physics(w);
}

static int kind_of(const trs_body *b, const char *id) {
	for (int k = 0; k < b->nlife; k++)
		if (!strcmp(b->life[k].id, id))
			return k;
	return -1;
}

/* an animal of kind k somewhere on body bi: where it is now (body-fixed) */
static int find_animal(const trs_world *w, int bi, int k, unsigned seed, trs_v3 *pos) {
	const trs_body *b = &w->sys.bodies[bi];
	int level = trs_life_level(b), max = 2000;
	trs_life_instance *li = malloc(sizeof(trs_life_instance) * (size_t)max);
	int found = 0;
	for (int q = 0; q < 20000 && !found; q++) {
		double z = frand(&seed) * 2 - 1, a = frand(&seed) * 2 * TRS_PI, r = sqrt(1 - z * z);
		trs_patch_id p = trs_patch_from_dir(v3(r * cos(a), r * sin(a), z), level);
		trs_v3 c, fwd;
		int n = trs_life_cell(b, p, 1u << k, &c, li, max);
		if (n > 0) {
			trs_life_animal_at(&b->life[k], &li[0], c, w->t, pos, &fwd);
			found = 1;
		}
	}
	free(li);
	return found;
}

/* a point m metres from body-fixed p along the ground, toward angle a (from east), on the ground */
static trs_v3 along(const trs_body *b, trs_v3 p, double a, double m) {
	trs_v3 up, n, e;
	trs_local_frame(p, &up, &n, &e);
	trs_v3 d = v3_norm(v3_add(p, v3_scale(v3_add(v3_scale(e, cos(a)), v3_scale(n, sin(a))), m)));
	return v3_scale(d, b->radius + trs_terrain_height(&b->terrain, b->radius, d));
}

/* a vessel at body-fixed ground point g, `up` m over it, upright, at rest on the ground (LANDED if fixed) */
static int put(trs_world *w, const trs_craft *c, int bi, trs_v3 g, double up, int fixed) {
	const trs_body *b = &w->sys.bodies[bi];
	trs_v3 at = v3_madd(g, v3_norm(g), up);
	trs_v3 r = trs_body_from_fixed(b, w->t, at);
	int id = trs_world_add_craft(w, c, bi, r, v3_cross(trs_body_omega(b), r), quat_from_to(v3(0, 1, 0), v3_norm(r)));
	w->vessels[id].w = trs_body_omega(b);
	w->vessels[id].sas = TRS_SAS_UPRIGHT;
	if (fixed)
		trs_vessel_set_mode(w, id, TRS_MODE_LANDED);
	return id;
}

static trs_v3 fixed_of(const trs_world *w, int id) {
	const trs_vessel *vs = &w->vessels[id];
	if (vs->mode == TRS_MODE_LANDED)
		return vs->fixed_r;
	return trs_body_to_fixed(&w->sys.bodies[vs->body], w->t, vs->r);
}

static const trs_critter *nearest(const trs_world *w, int k, trs_v3 p) {
	const trs_critter *best = NULL;
	double bd = 1e300;
	for (int i = 0; i < w->wild.n; i++)
		if (w->wild.c[i].kind == k && v3_len(v3_sub(w->wild.c[i].pos, p)) < bd)
			bd = v3_len(v3_sub(w->wild.c[i].pos, p)), best = &w->wild.c[i];
	return best;
}

static trs_world *fresh(const trs_system *sys, double t) {
	trs_world *w = malloc(sizeof(trs_world));
	trs_world_init(w, sys, t);
	return w;
}

static void done(trs_world *w) {
	trs_world_free(w);
	free(w);
}

trs_gate_status trs_gate_wild(trs_log *out, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	char path[1024], err[256];
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_gate_load_catalog(out, data_dir, &cat) || !trs_system_load(&sys, path, err, sizeof err)) {
		trs_log_printf(out, "cannot load: %s\n", err);
		return TRS_GATE_FAIL;
	}
	int failed = 0;
	int kelp = trs_system_find(&sys, "kelp"), floe = trs_system_find(&sys, "floe"), gloam = trs_system_find(&sys, "gloam");
	trs_craft pc, ship, raider;
	trs_craft_init(&pc, &cat);
	trs_craft_add(&pc, trs_catalog_find(&cat, "eva_penguin"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	/* a probe with a battery and a turret on top, its mount looking up */
	trs_craft_init(&ship, &cat);
	trs_craft_add(&ship, trs_catalog_find(&cat, "probe_core"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	trs_craft_add(&ship, trs_catalog_find(&cat, "battery"), 0, "", "", v3(0.6, 0, 0), quat_identity(), -1, 0);
	int turret = trs_craft_add(&ship, trs_catalog_find(&cat, "laser_turret"), 0, "", "", v3(0, 0.25, 0),
			quat_from_to(v3(0, 0, 1), v3(0, 1, 0)), -1, 0);
	trs_craft_init(&raider, &cat);
	{
		snprintf(path, sizeof path, "%s/craft/alien/jaeger_raider.craft.json", data_dir);
		trs_json *j = trs_json_load_file(path, err, sizeof err);
		if (!j || !trs_craft_from_json(&raider, &cat, j, err, sizeof err)) {
			trs_log_printf(out, "cannot load the raider: %s\n", err);
			return TRS_GATE_FAIL;
		}
		trs_json_free(j);
	}
	const trs_part_def *td = trs_craft_def(&ship, turret);
	const double t0 = 5000.0, h = TRS_PHYSICS_DT;

	/* 1. left alone, an animal is where the scenery draws it */
	{
		trs_world *w = fresh(&sys, t0);
		int k = kind_of(&sys.bodies[kelp], "grazers");
		trs_v3 at;
		int ok = k >= 0 && find_animal(w, kelp, k, 7, &at);
		const trs_body *b = &sys.bodies[kelp];
		int pen = put(w, &pc, kelp, along(b, at, 0.3, 200), 0.47, 0);
		trs_world_set_focus(w, pen);
		run(w, 2.0);
		const trs_critter *c = ok ? nearest(w, k, at) : NULL;
		double worst = 0;
		int checks = 0;
		for (int s = 0; c && s < 5; s++) {
			trs_v3 want, fwd;
			const trs_critter *cc = nearest(w, k, c->pos);
			trs_life_animal_at(&b->life[k], &cc->inst, cc->centre, w->t, &want, &fwd);
			worst = fmax(worst, v3_len(v3_sub(cc->pos, want)));
			checks += cc->state == TRS_CRIT_ROAM;
			run(w, 1.0);
		}
		ok = c && worst == 0 && checks == 5;
		failed += !ok;
		trs_log_printf(out, "%s left alone, %d grazers alive near: on its loop exactly as drawn (off by %.3g m over %d looks)\n",
				ok ? "ok  " : "FAIL", w->wild.n, worst, checks);
		done(w);
	}
	/* 2. a shy one runs at its run speed, and settles once well away */
	{
		trs_world *w = fresh(&sys, t0);
		const trs_body *b = &sys.bodies[kelp];
		int k = kind_of(b, "grazers");
		const trs_life *L = &b->life[k];
		trs_v3 at;
		find_animal(w, kelp, k, 7, &at);
		int pen = put(w, &pc, kelp, along(b, at, 0.3, 12), 0.47, 1);
		trs_world_set_focus(w, pen);
		run(w, 1.0);
		const trs_critter *c = nearest(w, k, at);
		int id = c ? c->id : -1;
		double d0 = c ? v3_len(v3_sub(c->pos, fixed_of(w, pen))) : 0;
		run(w, 4.0);
		int i = trs_wild_find(w, id);
		c = i >= 0 ? &w->wild.c[i] : NULL;
		double speed = c ? v3_len(c->vel) : 0, d1 = c ? v3_len(v3_sub(c->pos, fixed_of(w, pen))) : 0;
		int fled = c && c->state == TRS_CRIT_FLEE && d1 > d0 + 6 && fabs(speed - L->run) < 0.03 * L->run;
		run(w, 30.0);
		i = trs_wild_find(w, id);
		c = i >= 0 ? &w->wild.c[i] : NULL;
		double d2 = c ? v3_len(v3_sub(c->pos, fixed_of(w, pen))) : 0;
		int calm = c && c->state == TRS_CRIT_WANDER && d2 > L->wary;
		failed += !(fled && calm);
		trs_log_printf(out, "%s a shy grazer %.1f m from a penguin runs: %.1f m off after 4 s at %.2f m/s (its run %.2f), "
				"then wanders, %.0f m away (it calms past %.0f, and minds a penguin within %.0f)\n", fled && calm ? "ok  " : "FAIL", d0, d1,
				speed, L->run, d2, 2 * L->wary, L->wary);
		done(w);
	}
	/* 3. a curious one comes to a penguin's feet, and no nearer */
	{
		trs_world *w = fresh(&sys, t0);
		const trs_body *b = &sys.bodies[floe];
		int k = kind_of(b, "shelf_penguins");
		trs_v3 at;
		int ok = k >= 0 && find_animal(w, floe, k, 3, &at);
		int pen = put(w, &pc, floe, along(b, at, 1.0, 18), 0.47, 1);
		trs_world_set_focus(w, pen);
		run(w, 1.0);
		const trs_critter *c = nearest(w, k, fixed_of(w, pen));
		int id = c ? c->id : -1;
		run(w, 40.0);
		int i = trs_wild_find(w, id);
		c = i >= 0 ? &w->wild.c[i] : NULL;
		double gap = c ? v3_len(v3_sub(c->pos, fixed_of(w, pen))) - 0.35 : 0, stop = c ? 1.2 + 0.5 * c->size : 0;
		ok = ok && c && c->state == TRS_CRIT_FOLLOW && gap > stop - 0.3 && gap < stop + 0.3 && v3_len(c->vel) < 0.2;
		failed += !ok;
		trs_log_printf(out, "%s a shelf penguin comes over and stops %.2f m from ours (it stops at %.2f m), at %.2f m/s\n",
				ok ? "ok  " : "FAIL", gap, stop, c ? v3_len(c->vel) : 0);
		done(w);
	}
	/* 4. a hostile one charges and bites, until the penguin is lost to it */
	{
		trs_world *w = fresh(&sys, t0);
		const trs_body *b = &sys.bodies[gloam];
		int k = kind_of(b, "crawlers");
		const trs_life *L = &b->life[k];
		trs_v3 at;
		int ok = k >= 0 && find_animal(w, gloam, k, 5, &at);
		int pen = put(w, &pc, gloam, along(b, at, 2.0, 25), 0.47, 1);
		int serial = w->vessels[pen].serial;
		trs_world_set_focus(w, pen);
		double last_bite[TRS_WILD_MAX + 64];
		int ids[TRS_WILD_MAX + 64], nb = 0, bites = 0, odd = 0, early = 0, lost = 0;
		double t_lost = -1, first = -1;
		for (int s = 0; s < 60 * TRS_PHYSICS_HZ && !lost; s++) {
			double was = w->vessels[pen].alive && w->vessels[pen].serial == serial ? w->vessels[pen].craft->parts[0].damage : 0;
			double nb_was[TRS_WILD_MAX];
			for (int i = 0; i < w->wild.n; i++)
				nb_was[i] = w->wild.c[i].next_bite;
			int n_was = w->wild.n;
			trs_world_step_physics(w);
			for (int e = 0; e < w->nevents; e++)
				if (w->events[e].kind == TRS_WEV_PART_DESTROYED && w->events[e].vessel == pen && w->events[e].speed == TRS_DESTROY_BITTEN)
					lost = 1, t_lost = w->t;
			w->nevents = 0;
			for (int i = 0; i < w->wild.n && i < n_was; i++) {
				const trs_critter *c = &w->wild.c[i];
				if (c->next_bite <= nb_was[i])
					continue;
				/* it bit this step: no sooner than BITE after its last */
				int q = 0;
				while (q < nb && ids[q] != c->id)
					q++;
				if (q < nb && w->t - last_bite[q] < 1.2 - 1e-9)
					early++;
				if (q == nb && nb < TRS_WILD_MAX + 64)
					ids[nb++] = c->id;
				last_bite[q] = w->t;
				bites++;
				if (first < 0)
					first = w->t;
			}
			if (!lost && w->vessels[pen].alive && w->vessels[pen].serial == serial) {
				double now = w->vessels[pen].craft->parts[0].damage;
				double steps = (now - was) / L->bite;
				odd += fabs(steps - lround(steps)) > 1e-9;
			}
		}
		ok = ok && lost && bites >= 9 && !early && !odd;
		failed += !ok;
		trs_log_printf(out, "%s dusk crawlers charge a penguin 25 m off: %d of them bite, %d bites of %.0f hp (none sooner than "
				"1.2 s after the last, %d not whole bites), first at %.1f s; lost to them at %.1f s\n", ok ? "ok  " : "FAIL", nb,
				bites, L->bite, odd, first - t0, t_lost - t0);
		done(w);
	}
	/* 4b. a ship they leave be: with no penguin out, nothing on it is bitten */
	{
		trs_world *w = fresh(&sys, t0);
		const trs_body *b = &sys.bodies[gloam];
		int k = kind_of(b, "crawlers");
		trs_v3 at;
		int ok = k >= 0 && find_animal(w, gloam, k, 5, &at);
		int sh = put(w, &ship, gloam, along(b, at, 2.0, 8), 3.0, 1);
		trs_world_set_focus(w, sh);
		int charged = 0, bitten = 0, near = 0;
		for (int s = 0; s < 30 * TRS_PHYSICS_HZ; s++) {
			trs_world_step_physics(w);
			for (int i = 0; i < w->wild.n; i++) {
				charged += w->wild.c[i].kind == k && w->wild.c[i].state == TRS_CRIT_CHARGE;
				near += w->wild.c[i].kind == k && s == 0;
			}
		}
		for (int i = 0; i < w->vessels[sh].craft->count; i++)
			bitten += w->vessels[sh].craft->parts[i].damage > 0;
		ok = ok && near > 0 && !charged && !bitten && w->vessels[sh].alive;
		failed += !ok;
		trs_log_printf(out, "%s a ship 8 m from %d dusk crawlers and no penguin out: in 30 s none charges (%d ticks), "
				"%d of its parts bitten\n", ok ? "ok  " : "FAIL", near, charged, bitten);
		done(w);
	}
	/* 5. a turret on guard sees them off before they reach the penguin */
	{
		trs_world *w = fresh(&sys, t0);
		const trs_body *b = &sys.bodies[gloam];
		int k = kind_of(b, "crawlers");
		const trs_life *L = &b->life[k];
		trs_v3 at;
		find_animal(w, gloam, k, 5, &at);
		/* (the ship between the penguin and the herd, its turret on a mast) */
		trs_v3 base = along(b, at, 2.0, 30);
		int sh = put(w, &ship, gloam, base, 3.0, 1);
		int pen = put(w, &pc, gloam, along(b, at, 2.0, 33), 0.47, 1);
		int ps = w->vessels[pen].serial;
		trs_world_set_focus(w, pen);
		trs_world_set_guard(w, sh, 1);
		double ec0 = trs_craft_ec(w->vessels[sh].craft, NULL);
		int fire_ticks = 0, crit_ticks = 0, downs = 0;
		double hp_lost = 0;
		int seen[4096], nseen = 0;
		for (int s = 0; s < 30 * TRS_PHYSICS_HZ; s++) {
			trs_world_step_physics(w);
			const trs_part *tp = &w->vessels[sh].craft->parts[turret];
			fire_ticks += tp->beam > 0;
			crit_ticks += tp->beam > 0 && tp->beam_hit == TRS_THING_CRITTER;
			for (int i = 0; i < w->wild.n; i++) {
				const trs_critter *c = &w->wild.c[i];
				if (c->kind != k || c->state != TRS_CRIT_DOWN)
					continue;
				int q = 0;
				while (q < nseen && seen[q] != c->id)
					q++;
				if (q == nseen && nseen < 4096) {
					seen[nseen++] = c->id;
					downs++;
					hp_lost += L->hp;
				}
			}
		}
		/* the hp they lost: each downed one all of it, the rest what's off them */
		for (int i = 0; i < w->wild.n; i++)
			if (w->wild.c[i].kind == k && w->wild.c[i].state != TRS_CRIT_DOWN)
				hp_lost += L->hp - w->wild.c[i].hp;
		double dealt = crit_ticks * td->laser_dps * h;
		double ec_used = ec0 - trs_craft_ec(w->vessels[sh].craft, NULL);
		int alive = w->vessels[pen].alive && w->vessels[pen].serial == ps;
		double health = alive ? 100 - w->vessels[pen].craft->parts[0].damage : 0;
		int ok = downs >= 3 && alive && hp_lost <= dealt + 1e-9 && hp_lost >= dealt - downs * td->laser_dps * h - 1e-9 &&
			fabs(ec_used - fire_ticks * td->laser_kw * h) < 1e-6;
		failed += !ok;
		trs_log_printf(out, "%s a turret on guard 3 m from a penguin (between it and the herd) downs %d dusk crawlers in 30 s; the penguin lives (%.0f%% "
				"health); beams on them %d ticks = %.1f hp, and they lost %.1f; batteries paid %.3f kJ for %d ticks at %.0f kW (%.3f)\n",
				ok ? "ok  " : "FAIL", downs, health, crit_ticks, dealt, hp_lost, ec_used, fire_ticks, td->laser_kw,
				fire_ticks * td->laser_kw * h);
		/* the downed stay down: still in the dead list, and not back */
		run(w, 10.0);
		int back = 0;
		for (int i = 0; i < w->wild.n; i++)
			for (int q = 0; q < nseen; q++)
				back += w->wild.c[i].id == seen[q];
		int dead = 0;
		for (int q = 0; q < w->wild.ndead; q++)
			dead += w->wild.dead[q].kind == k;
		ok = back == 0 && dead >= downs;
		failed += !ok;
		trs_log_printf(out, "%s 10 s on: the %d downed are gone (%d back), %d in the herds' losses until %.0f h on\n", ok ? "ok  " : "FAIL",
				downs, back, dead, w->wild.ndead ? (w->wild.dead[0].until - w->t) / 3600 : 0);
		/* a saved world steps on exactly as the one it was saved from */
		trs_json *j = trs_world_to_json(w);
		trs_world *w2 = fresh(&sys, 0);
		int loaded = trs_world_from_json(w2, &cat, j, err, sizeof err);
		trs_json_free(j);
		run(w, 5.0);
		run(w2, 5.0);
		double off = 0;
		int same = loaded && w->wild.n == w2->wild.n;
		for (int i = 0; same && i < w->wild.n; i++) {
			same &= w->wild.c[i].id == w2->wild.c[i].id && w->wild.c[i].state == w2->wild.c[i].state && w->wild.c[i].hp == w2->wild.c[i].hp;
			off = fmax(off, v3_len(v3_sub(w->wild.c[i].pos, w2->wild.c[i].pos)));
		}
		same &= off == 0 && trs_craft_ec(w->vessels[sh].craft, NULL) == trs_craft_ec(w2->vessels[sh].craft, NULL);
		failed += !same;
		trs_log_printf(out, "%s saved and loaded, %d animals step on exactly alike (off by %.3g m)\n", same ? "ok  " : "FAIL", w->wild.n, off);
		done(w2);
		done(w);
	}
	/* 6. the computer won't fire through a penguin; by hand it will */
	{
		trs_world *w = fresh(&sys, t0);
		const trs_body *b = &sys.bodies[kelp];
		/* (on the launch site's flat: no herds there, and nothing in the way) */
		trs_v3 g = along(b, b->terrain.site_dir, 0.5, 60);
		int sh = put(w, &ship, kelp, g, 0.5, 1);
		int rd = put(w, &raider, kelp, along(b, g, 0.0, 100), 2.0, 1);
		trs_world_set_focus(w, sh);
		/* the penguin hangs right in the line, halfway */
		trs_v3 mount, axis, aim_at;
		double rad;
		trs_laser_mount(w, &w->vessels[sh], turret, &mount, &axis);
		trs_thing_where(w, kelp, (trs_thing){ TRS_THING_VESSEL, rd, w->vessels[rd].serial }, &aim_at, &rad);
		trs_v3 mid = v3_scale(v3_add(mount, aim_at), 0.5);
		trs_v3 mf = trs_body_to_fixed(b, w->t, mid);
		int pen = put(w, &pc, kelp, v3_scale(v3_norm(mf), v3_len(mf)), 0.0, 1);
		trs_world_lock(w, sh, (trs_thing){ TRS_THING_VESSEL, rd, w->vessels[rd].serial });
		int fired = 0;
		for (int s = 0; s < 3 * TRS_PHYSICS_HZ; s++) {
			trs_world_step_physics(w);
			fired += w->vessels[sh].craft->parts[turret].beam > 0;
		}
		trs_world_lock(w, sh, (trs_thing){ 0, 0, 0 });
		trs_thing_where(w, kelp, (trs_thing){ TRS_THING_VESSEL, rd, w->vessels[rd].serial }, &aim_at, &rad);
		trs_world_hand_fire(w, sh, 1, aim_at);
		int hits = 0;
		for (int s = 0; s < 25; s++) {
			trs_world_step_physics(w);
			hits += w->vessels[sh].craft->parts[turret].beam_hit == TRS_THING_VESSEL;
		}
		double took = w->vessels[pen].craft->parts[0].damage;
		int ok = fired == 0 && hits == 25 && fabs(took - 25 * td->laser_dps * h) < 1e-9;
		failed += !ok;
		trs_log_printf(out, "%s locked on a Jaeger 100 m off with a penguin in the line: the computer holds fire (%d ticks fired); "
				"by hand the beam meets the penguin, %.1f hp in %d ticks (%.0f a second)\n", ok ? "ok  " : "FAIL", fired, took, hits, td->laser_dps);
		/* the penguin out of the way, the lock does its work on the Jaeger */
		trs_world_hand_fire(w, sh, 0, aim_at);
		trs_world_remove_vessel(w, pen);
		trs_world_lock(w, sh, (trs_thing){ TRS_THING_VESSEL, rd, w->vessels[rd].serial });
		double dmg0 = 0, dmg1 = 0;
		for (int i = 0; i < w->vessels[rd].craft->count; i++)
			dmg0 += w->vessels[rd].craft->parts[i].damage;
		int on = 0;
		for (int s = 0; s < 2 * TRS_PHYSICS_HZ; s++) {
			trs_world_step_physics(w);
			on += w->vessels[sh].craft->parts[turret].beam_hit == TRS_THING_VESSEL;
		}
		for (int i = 0; i < w->vessels[rd].craft->count; i++)
			dmg1 += w->vessels[rd].craft->parts[i].damage;
		ok = on > 90 && fabs(dmg1 - dmg0 - on * td->laser_dps * h) < 1e-9;
		failed += !ok;
		trs_log_printf(out, "%s then locked on, it fires %d of 100 ticks and the Jaeger takes %.1f hp (%.1f)\n", ok ? "ok  " : "FAIL", on,
				dmg1 - dmg0, on * td->laser_dps * h);
		/* the ground stops a beam: fired at a point under a hill's skin */
		trs_world_lock(w, sh, (trs_thing){ 0, 0, 0 });
		trs_v3 under = along(b, g, 2.4, 300);
		under = v3_madd(under, v3_norm(under), -10);
		trs_world_hand_fire(w, sh, 1, trs_body_from_fixed(b, w->t, under));
		run(w, 3.0);
		trs_world_step_physics(w);
		const trs_part *tp = &w->vessels[sh].craft->parts[turret];
		trs_laser_mount(w, &w->vessels[sh], turret, &mount, &axis);
		trs_v3 aim = quat_rotate(w->vessels[sh].mode == TRS_MODE_LANDED
				? quat_normalize(quat_mul(trs_body_quat(b, w->t), w->vessels[sh].fixed_q)) : w->vessels[sh].q, tp->aim);
		trs_v3 end = trs_body_to_fixed(b, w->t, v3_madd(mount, v3_norm(aim), tp->beam));
		double agl = v3_len(end) - b->radius - trs_terrain_height(&b->terrain, b->radius, v3_norm(end));
		double to_point = v3_len(v3_sub(trs_body_from_fixed(b, w->t, under), mount));
		ok = tp->beam_hit == TRS_THING_GROUND && tp->beam < to_point && fabs(agl) < 1.0;
		failed += !ok;
		trs_log_printf(out, "%s fired at a point 10 m under the ground %.0f m off, the beam stops at the ground after %.1f m "
				"(%.2f m off it)\n", ok ? "ok  " : "FAIL", to_point, tp->beam, agl);
		trs_world_hand_fire(w, sh, 0, v3(0, 0, 0));
		done(w);
	}
	/* 7. guard leaves alone what is out of reach */
	{
		trs_world *w = fresh(&sys, t0);
		const trs_body *b = &sys.bodies[kelp];
		trs_v3 g = along(b, b->terrain.site_dir, 0.5, 60);
		int sh = put(w, &ship, kelp, g, 0.5, 1);
		/* (the far one hangs well up, so only its distance keeps it safe) */
		put(w, &raider, kelp, along(b, g, 0.0, td->laser_range + 150), 120.0, 1);
		int near = put(w, &raider, kelp, along(b, g, 3.1, 300), 8.0, 1);
		trs_world_set_focus(w, sh);
		trs_world_set_guard(w, sh, 1);
		int fired = 0;
		for (int s = 0; s < 3 * TRS_PHYSICS_HZ; s++) {
			trs_world_step_physics(w);
			fired += w->vessels[sh].craft->parts[turret].beam > 0;
		}
		trs_world_remove_vessel(w, near);
		int far_fired = 0;
		for (int s = 0; s < 3 * TRS_PHYSICS_HZ; s++) {
			trs_world_step_physics(w);
			far_fired += w->vessels[sh].craft->parts[turret].beam > 0;
		}
		int ok = fired > 0 && far_fired == 0;
		failed += !ok;
		trs_log_printf(out, "%s on guard: fires on a Jaeger 300 m off (%d ticks), not on one %.0f m off, past its %.0f m reach (%d)\n",
				ok ? "ok  " : "FAIL", fired, td->laser_range + 150, td->laser_range, far_fired);
		done(w);
	}
	trs_craft_free(&pc);
	trs_craft_free(&ship);
	trs_craft_free(&raider);
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
