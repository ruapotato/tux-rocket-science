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

#include "trs/part.h"
#include "trs/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static trs_v3 json_v3(const trs_json *a, trs_v3 fallback) {
	if (!a || a->type != TRS_JSON_ARRAY || a->count < 3)
		return fallback;
	return v3(a->items[0]->num, a->items[1]->num, a->items[2]->num);
}

static int parse_part(trs_part_def *d, const trs_json *j, char *err, size_t errlen) {
	memset(d, 0, sizeof(*d));
	snprintf(d->id, sizeof d->id, "%s", trs_json_str(j, "id", ""));
	snprintf(d->name, sizeof d->name, "%s", trs_json_str(j, "name", d->id));
	snprintf(d->category, sizeof d->category, "%s", trs_json_str(j, "category", "misc"));
	if (!d->id[0]) {
		snprintf(err, errlen, "part without id");
		return 0;
	}
	d->mass = trs_json_num(j, "mass", 0);
	if (d->mass <= 0) {
		snprintf(err, errlen, "part %s: mass must be > 0", d->id);
		return 0;
	}
	d->fuel_liquid = trs_json_num(j, "fuel_liquid", 0);
	d->fuel_solid = trs_json_num(j, "fuel_solid", 0);
	d->fuel_mono = trs_json_num(j, "fuel_mono", 0);
	d->crew = (int)trs_json_num(j, "crew", 0);
	d->torque = trs_json_num(j, "torque", 0);
	d->crash_tolerance = trs_json_num(j, "crash_tolerance", 8);

	const trs_json *e = trs_json_get(j, "engine");
	if (e) {
		const char *prop = trs_json_str(e, "propellant", "liquid");
		d->propellant = !strcmp(prop, "solid") ? TRS_PROP_SOLID : TRS_PROP_LIQUID;
		d->thrust_vac = trs_json_num(e, "thrust_vac", 0);
		d->isp_vac = trs_json_num(e, "isp_vac", 0);
		d->isp_sl = trs_json_num(e, "isp_sl", d->isp_vac);
		d->gimbal = trs_json_num(e, "gimbal_deg", 0) * TRS_PI / 180.0;
		d->air_breathing = trs_json_bool(e, "air_breathing", 0);
		d->engine_kw = trs_json_num(e, "kw", 0);
		d->jet_v_max = trs_json_num(e, "v_max", 600);
		d->jet_rho_ref = trs_json_num(e, "rho_ref", 1.225);
		if (d->thrust_vac <= 0 || d->isp_vac <= 0) {
			snprintf(err, errlen, "part %s: engine needs thrust_vac and isp_vac", d->id);
			return 0;
		}
	}
	const trs_json *rc = trs_json_get(j, "rcs");
	if (rc) {
		d->rcs_thrust = trs_json_num(rc, "thrust", 0);
		d->rcs_isp_vac = trs_json_num(rc, "isp_vac", 0);
		d->rcs_isp_sl = trs_json_num(rc, "isp_sl", d->rcs_isp_vac);
		const trs_json *nz = trs_json_get(rc, "nozzles");
		for (size_t k = 0; nz && k < nz->count && k < TRS_MAX_NOZZLES; k++) {
			const trs_json *n = trs_json_at(nz, k);
			d->rcs_pos[d->rcs_n] = json_v3(trs_json_get(n, "pos"), v3(0, 0, 0));
			d->rcs_dir[d->rcs_n++] = v3_norm(json_v3(trs_json_get(n, "dir"), v3(0, 1, 0)));
		}
		if (d->rcs_thrust <= 0 || d->rcs_isp_vac <= 0 || d->rcs_n == 0) {
			snprintf(err, errlen, "part %s: rcs needs thrust, isp_vac and nozzles", d->id);
			return 0;
		}
	}
	const trs_json *dk = trs_json_get(j, "docking");
	if (dk) {
		d->is_dock = 1;
		d->dock_capture = trs_json_num(dk, "capture", 0.4);
		d->dock_max_speed = trs_json_num(dk, "max_speed", 1.0);
		d->dock_max_angle = trs_json_num(dk, "max_angle_deg", 10) * TRS_PI / 180.0;
		d->dock_impulse = trs_json_num(dk, "undock_impulse", 100);
	}
	const trs_json *dc = trs_json_get(j, "decoupler");
	if (dc) {
		d->decoupler = !strcmp(trs_json_str(dc, "kind", "stack"), "radial") ? TRS_DECOUPLER_RADIAL : TRS_DECOUPLER_STACK;
		d->eject_impulse = trs_json_num(dc, "impulse", 0);
	}
	const trs_json *pc = trs_json_get(j, "parachute");
	if (pc) {
		d->is_parachute = 1;
		d->chute_semi_cda = trs_json_num(pc, "semi_cda", 0);
		d->chute_full_cda = trs_json_num(pc, "full_cda", 0);
		d->chute_min_pressure = trs_json_num(pc, "min_pressure", 0);
		d->chute_full_alt = trs_json_num(pc, "full_altitude", 1000);
		d->chute_max_speed = trs_json_num(pc, "max_speed", 1e9);
		d->chute_inflate = trs_json_num(pc, "inflate_time", 2.5);
	}
	const trs_json *fl = trs_json_get(j, "feed_line");
	if (fl)
		d->feed_reach = trs_json_num(fl, "reach", 0.6);
	const trs_json *pw = trs_json_get(j, "power");
	if (pw) {
		d->ec_capacity = trs_json_num(pw, "battery_kj", 0);
		d->ec_draw = trs_json_num(pw, "draw_kw", 0);
		d->torque_kw_per_knm = trs_json_num(pw, "torque_kw_per_knm", 0);
		d->solar_kw = trs_json_num(pw, "solar_kw", 0);
		d->solar_ref_m = trs_json_num(pw, "solar_ref_m", 1.4e10);
		d->gen_kw = trs_json_num(pw, "generator_kw", 0);
		d->antenna_kj_per_point = trs_json_num(pw, "antenna_kj_per_point", 0);
		d->antenna_fraction = trs_json_num(pw, "antenna_fraction", 0);
	}
	d->is_ladder = trs_json_get(j, "ladder") != NULL;
	const trs_json *sc = trs_json_get(j, "scanner");
	if (sc)
		d->scanner_range = trs_json_num(sc, "range_km", 0) * 1000.0;
	d->hp = trs_json_num(j, "hp", 50.0 + 0.5 * d->mass);
	const trs_json *lz = trs_json_get(j, "laser");
	if (lz) {
		d->laser_range = trs_json_num(lz, "range_m", 1000);
		d->laser_dps = trs_json_num(lz, "dps", 30);
		d->laser_kw = trs_json_num(lz, "kw", 0);
		d->laser_slew = trs_json_num(lz, "slew_deg_s", 90) * TRS_PI / 180.0;
		d->laser_arc = trs_json_num(lz, "arc_deg", 100) * TRS_PI / 180.0;
	}
	d->is_alien = trs_json_bool(j, "alien", 0);
	d->alien_pilot = trs_json_bool(j, "alien_pilot", 0);
	const trs_json *lf = trs_json_get(j, "lift");
	if (lf) {
		d->lift_force = trs_json_num(lf, "force_kn", 0) * 1000.0;
		d->lift_kw = trs_json_num(lf, "kw", 0);
	}
	const trs_json *dj = trs_json_get(j, "drive");
	if (dj) {
		d->drive_accel = trs_json_num(dj, "accel_g", 0) * 9.80665;
		d->drive_kw = trs_json_num(dj, "kw", 0);
	}
	const trs_json *mn = trs_json_get(j, "mining");
	if (mn) {
		d->ore_capacity = trs_json_num(mn, "ore_kg", 0);
		d->drill_kg_s = trs_json_num(mn, "drill_kg_s", 0);
		d->drill_kw = trs_json_num(mn, "drill_kw", 0);
		d->drill_reach = trs_json_num(mn, "drill_reach", 1.0);
		d->refine_kg_s = trs_json_num(mn, "refine_kg_s", 0);
		d->refine_yield = trs_json_num(mn, "refine_yield", 0.8);
		d->refine_kw = trs_json_num(mn, "refine_kw", 0);
	}
	const trs_json *ev = trs_json_get(j, "eva");
	if (ev) {
		d->is_eva = 1;
		d->eva_walk = trs_json_num(ev, "walk_speed", 1.5);
		d->eva_jump = trs_json_num(ev, "jump_speed", 2.0);
		d->eva_reach = trs_json_num(ev, "reach", 1.5);
		d->eva_room_radius = trs_json_num(ev, "room_radius", 0.35);
	}
	const trs_json *wh = trs_json_get(j, "wheel");
	if (wh) {
		d->is_wheel = 1;
		d->wheel_length = trs_json_num(wh, "length", 0.5);
		d->leg_spring = trs_json_num(wh, "spring", 0);
		d->leg_damper = trs_json_num(wh, "damper", 0);
		d->leg_max_speed = trs_json_num(wh, "max_load_speed", 8);
		d->wheel_mu_roll = trs_json_num(wh, "mu_roll", 0.03);
		d->wheel_mu_brake = trs_json_num(wh, "mu_brake", 0.7);
		d->wheel_mu_side = trs_json_num(wh, "mu_side", 0.9);
		d->wheel_steer = trs_json_num(wh, "steer_deg", 0) * TRS_PI / 180.0;
	}
	const trs_json *lg = trs_json_get(j, "leg");
	if (lg) {
		d->is_leg = 1;
		d->leg_length = trs_json_num(lg, "length", 1);
		d->leg_spring = trs_json_num(lg, "spring", 0);
		d->leg_damper = trs_json_num(lg, "damper", 0);
		d->leg_max_speed = trs_json_num(lg, "max_load_speed", 10);
		d->leg_hinge = json_v3(trs_json_get(lg, "hinge"), v3(0, 0.1, 0.14));
		d->leg_angle = trs_json_num(lg, "deploy_angle_deg", 20) * TRS_PI / 180.0;
		d->leg_stowed_fraction = trs_json_num(lg, "stowed_fraction", 0.55);
	}
	const trs_json *in = trs_json_get(j, "interior");
	if (in) {
		d->spin_rate = trs_json_num(in, "spin_rpm", 0) * 2 * TRS_PI / 60.0;
		const trs_json *vl = trs_json_get(in, "volumes");
		for (size_t k = 0; vl && k < vl->count; k++) {
			const trs_json *vj = trs_json_at(vl, k);
			const char *ty = trs_json_str(vj, "type", "cylinder");
			trs_volume v;
			memset(&v, 0, sizeof v);
			v.r0 = trs_json_num(vj, "r0", 0);
			v.r1 = trs_json_num(vj, "r1", trs_json_num(vj, "radius", 0));
			v.y0 = trs_json_num(vj, "y0", 0);
			v.y1 = trs_json_num(vj, "y1", 0);
			v.rt = trs_json_num(vj, "radius_top", v.r1);
			const trs_json *wr = trs_json_get(vj, "windows");
			for (size_t q = 0; wr && q < wr->count && v.nwindows < TRS_MAX_WINDOWS; q++) {
				const trs_json *wj = trs_json_at(wr, q);
				trs_window_row wr = { trs_json_num(wj, "at", 0), trs_json_num(wj, "size", 0.5), (int)trs_json_num(wj, "count", 0), 0, { 0 } };
				const trs_json *aj = trs_json_get(wj, "angles_deg");
				for (size_t e = 0; aj && e < aj->count && wr.nangles < 6; e++)
					wr.angles[wr.nangles++] = trs_json_at(aj, e)->num * TRS_PI / 180.0;
				v.windows[v.nwindows++] = wr;
			}
			int n = 1;
			if (!strcmp(ty, "cylinder"))
				v.kind = TRS_VOL_CYLINDER;
			else if (!strcmp(ty, "annulus"))
				v.kind = TRS_VOL_ANNULUS;
			else if (!strcmp(ty, "box")) {
				v.kind = TRS_VOL_BOX;
				v.lo = json_v3(trs_json_get(vj, "min"), v3(0, 0, 0));
				v.hi = json_v3(trs_json_get(vj, "max"), v3(0, 0, 0));
				v.yaw = trs_json_num(vj, "yaw_deg", 0) * TRS_PI / 180.0;
			} else if (!strcmp(ty, "spokes")) {
				/* `count` boxes from r0 out to r1, `width` square, evenly round */
				v.kind = TRS_VOL_BOX;
				n = (int)trs_json_num(vj, "count", 4);
				double hw = 0.5 * trs_json_num(vj, "width", 1.0);
				v.lo = v3(v.r0, v.y0, -hw);
				v.hi = v3(v.r1, v.y1, hw);
			} else {
				snprintf(err, errlen, "part %s: unknown interior volume type %s", d->id, ty);
				return 0;
			}
			if (v.kind != TRS_VOL_BOX && (v.r1 <= v.r0 || v.y1 <= v.y0)) {
				snprintf(err, errlen, "part %s: interior volume %d is empty", d->id, (int)k);
				return 0;
			}
			for (int s = 0; s < n; s++) {
				if (d->nvolumes >= TRS_MAX_VOLUMES) {
					snprintf(err, errlen, "part %s: more than %d interior volumes", d->id, TRS_MAX_VOLUMES);
					return 0;
				}
				trs_volume vs = v;
				if (n > 1)
					vs.yaw = 2 * TRS_PI * s / n;
				d->volumes[d->nvolumes++] = vs;
			}
		}
	}
	const trs_json *sh = trs_json_get(j, "shape");
	d->radius = trs_json_num(sh, "radius", trs_json_num(sh, "radius_bottom", 0.3));
	d->height = trs_json_num(sh, "height", trs_json_num(sh, "length", trs_json_num(sh, "root", 0.5)));
	const trs_json *aero = trs_json_get(j, "aero");
	d->cd_top = trs_json_num(aero, "cd_top", 0.8);
	d->cd_bottom = trs_json_num(aero, "cd_bottom", 0.8);
	d->cd_side = trs_json_num(aero, "cd_side", 1.0);
	d->area_axial = trs_json_num(aero, "area_axial", TRS_PI * d->radius * d->radius);
	d->area_side = trs_json_num(aero, "area_side", 2 * d->radius * d->height);
	d->cop = trs_json_num(aero, "cop", 0);
	d->plate = trs_json_bool(aero, "plate", sh && !strcmp(trs_json_str(sh, "type", ""), "fin"));
	d->lift_slope = trs_json_num(aero, "lift_slope", 0);
	d->fin_deflect = trs_json_num(aero, "control_deflection_deg", 0) * TRS_PI / 180.0;
	d->fin_span = trs_json_num(sh, "span", 0.8);
	d->radius_top = trs_json_num(sh, "radius_top", d->radius);
	d->radius_bottom = trs_json_num(sh, "radius_bottom", d->radius);
	const char *stype = trs_json_str(sh, "type", "");
	if (!strcmp(stype, "box") || !strcmp(stype, "wedge") || !strcmp(stype, "slab")) {
		d->box = 1;
		d->width_bottom = trs_json_num(sh, "width", 2.5);
		d->depth_bottom = trs_json_num(sh, "depth", d->width_bottom);
		d->width_top = trs_json_num(sh, "width_top", d->width_bottom);
		d->depth_top = trs_json_num(sh, "depth_top", d->depth_bottom);
		d->radius_bottom = 0.5 * fmax(d->width_bottom, d->depth_bottom);
		d->radius_top = 0.5 * fmax(d->width_top, d->depth_top);
		d->radius = fmax(d->radius_bottom, d->radius_top);
		double wm = 0.5 * (d->width_bottom + d->width_top), dm = 0.5 * (d->depth_bottom + d->depth_top);
		if (!trs_json_get(aero, "area_axial"))
			d->area_axial = fmax(d->width_bottom * d->depth_bottom, d->width_top * d->depth_top);
		if (!trs_json_get(aero, "area_side"))
			d->area_side = d->height * fmax(wm, dm);
	}
	const trs_json *rz = trs_json_get(j, "resize");
	d->size_min = d->size_max = v3(1, 1, 1);
	if (rz) {
		d->resize = !strcmp(trs_json_str(rz, "kind", d->box ? "box" : "round"), "box") ? 2 : 1;
		d->size_min = json_v3(trs_json_get(rz, "min"), v3(0.5, 0.5, 0.5));
		d->size_max = json_v3(trs_json_get(rz, "max"), v3(2, 2, 2));
	}
	const trs_json *th = trs_json_get(j, "thermal");
	d->max_temp = trs_json_num(th, "max_temp", 1500);
	d->skin_heat = trs_json_num(th, "skin_heat", 8000);
	d->emissivity = trs_json_num(th, "emissivity", 0.8);

	const trs_json *nodes = trs_json_get(j, "nodes");
	for (size_t k = 0; nodes && k < nodes->count && k < TRS_MAX_NODES; k++) {
		const trs_json *n = trs_json_at(nodes, k);
		trs_attach_node *an = &d->nodes[d->nnodes++];
		snprintf(an->name, sizeof an->name, "%s", trs_json_str(n, "name", "node"));
		an->pos = json_v3(trs_json_get(n, "pos"), v3(0, 0, 0));
		an->dir = json_v3(trs_json_get(n, "dir"), v3(0, 1, 0));
		an->radius = trs_json_num(n, "radius", d->radius);
	}
	d->surface_attach = trs_json_bool(j, "surface_attach", 1);
	return 1;
}

trs_v3 trs_part_clamp_size(const trs_part_def *d, trs_v3 s) {
	if (!d->resize)
		return v3(1, 1, 1);
	if (d->resize == 1)
		s.z = s.x;
	s.x = fmin(fmax(s.x, d->size_min.x), d->size_max.x);
	s.y = fmin(fmax(s.y, d->size_min.y), d->size_max.y);
	s.z = d->resize == 1 ? s.x : fmin(fmax(s.z, d->size_min.z), d->size_max.z);
	return s;
}

static trs_v3 mulv(trs_v3 a, trs_v3 s) {
	return v3(a.x * s.x, a.y * s.y, a.z * s.z);
}

void trs_part_def_scaled(const trs_part_def *b, trs_v3 s, trs_part_def *d) {
	*d = *b;
	double vol = s.x * s.y * s.z, lat = 0.5 * (s.x + s.z);
	d->mass *= vol;
	d->fuel_liquid *= vol, d->fuel_solid *= vol, d->fuel_mono *= vol;
	d->ec_capacity *= vol, d->ore_capacity *= vol;
	if (b->crew > 0)
		d->crew = (int)fmax(1, floor(b->crew * vol + 0.5));
	d->height *= s.y;
	d->radius *= lat, d->radius_top *= lat, d->radius_bottom *= lat;
	d->width_bottom *= s.x, d->width_top *= s.x, d->depth_bottom *= s.z, d->depth_top *= s.z;
	if (b->box) {
		d->radius_bottom = 0.5 * fmax(d->width_bottom, d->depth_bottom);
		d->radius_top = 0.5 * fmax(d->width_top, d->depth_top);
		d->radius = fmax(d->radius_bottom, d->radius_top);
	}
	d->area_axial *= s.x * s.z;
	d->area_side *= b->plate ? s.y * s.z : s.y * fmax(s.x, s.z);
	d->fin_span *= s.z;
	for (int k = 0; k < d->nnodes; k++) {
		d->nodes[k].pos = mulv(b->nodes[k].pos, s);
		d->nodes[k].radius *= fmin(s.x, s.z);
	}
	for (int k = 0; k < d->rcs_n; k++)
		d->rcs_pos[k] = mulv(b->rcs_pos[k], s);
	for (int k = 0; k < d->nvolumes; k++) {
		trs_volume *v = &d->volumes[k];
		v->r0 *= lat, v->r1 *= lat, v->rt *= lat, v->y0 *= s.y, v->y1 *= s.y;
		if (v->kind == TRS_VOL_BOX && v->yaw == 0)
			v->lo = mulv(v->lo, s), v->hi = mulv(v->hi, s);
		else if (v->kind == TRS_VOL_BOX)
			v->lo = mulv(v->lo, v3(lat, s.y, lat)), v->hi = mulv(v->hi, v3(lat, s.y, lat));
		for (int w = 0; w < v->nwindows; w++)
			v->windows[w].at *= v->kind == TRS_VOL_ANNULUS ? lat : s.y;
	}
}

void trs_hull_half(const trs_part_def *d, double y, double *hx, double *hz) {
	double f = d->height > 0 ? fmin(1, fmax(0, (y + d->height * 0.5) / d->height)) : 0.5;
	if (d->box) {
		*hx = 0.5 * (d->width_bottom + (d->width_top - d->width_bottom) * f);
		*hz = 0.5 * (d->depth_bottom + (d->depth_top - d->depth_bottom) * f);
	} else {
		*hx = *hz = d->radius_bottom + (d->radius_top - d->radius_bottom) * f;
	}
}

int trs_hull_solid(const trs_part_def *d) {
	return !(d->is_leg || d->is_parachute || d->plate || d->radius <= 0.05);
}

int trs_hull_inside(const trs_part_def *d, trs_v3 p, trs_v3 *normals, double *depths, int *nfaces) {
	double hh = d->height * 0.5, hx, hz;
	trs_hull_half(d, p.y, &hx, &hz);
	double dy = hh - fabs(p.y);
	int nf = 0;
	if (d->box) {
		double dx = hx - fabs(p.x), dz = hz - fabs(p.z);
		if (dy <= 0 || dx <= 0 || dz <= 0)
			return 0;
		normals[nf] = v3(0, p.y >= 0 ? 1 : -1, 0), depths[nf++] = dy;
		if (dx < dz)
			normals[nf] = v3(p.x >= 0 ? 1 : -1, 0, 0), depths[nf++] = dx;
		else
			normals[nf] = v3(0, 0, p.z >= 0 ? 1 : -1), depths[nf++] = dz;
	} else {
		double rho = sqrt(p.x * p.x + p.z * p.z), dr = hx - rho;
		if (dy <= 0 || dr <= 0)
			return 0;
		normals[nf] = v3(0, p.y >= 0 ? 1 : -1, 0), depths[nf++] = dy;
		if (rho > 1e-9)
			normals[nf] = v3(p.x / rho, 0, p.z / rho), depths[nf++] = dr;
	}
	*nfaces = nf;
	return 1;
}

int trs_catalog_load(trs_part_catalog *cat, const char *path, char *err, size_t errlen) {
	memset(cat, 0, sizeof(*cat));
	trs_json *root = trs_json_load_file(path, err, errlen);
	if (!root)
		return 0;
	const trs_json *parts = trs_json_get(root, "parts");
	if (!parts || parts->type != TRS_JSON_ARRAY) {
		snprintf(err, errlen, "%s: no parts array", path);
		trs_json_free(root);
		return 0;
	}
	cat->defs = calloc(parts->count ? parts->count : 1, sizeof(trs_part_def));
	for (size_t i = 0; i < parts->count; i++) {
		if (!parse_part(&cat->defs[cat->count], trs_json_at(parts, i), err, errlen)) {
			trs_json_free(root);
			trs_catalog_free(cat);
			return 0;
		}
		if (trs_catalog_find(cat, cat->defs[cat->count].id) >= 0) {
			snprintf(err, errlen, "duplicate part id %s", cat->defs[cat->count].id);
			trs_json_free(root);
			trs_catalog_free(cat);
			return 0;
		}
		cat->count++;
	}
	trs_json_free(root);
	return 1;
}

void trs_catalog_free(trs_part_catalog *cat) {
	free(cat->defs);
	cat->defs = NULL;
	cat->count = 0;
}

int trs_catalog_find(const trs_part_catalog *cat, const char *id) {
	for (int i = 0; i < cat->count; i++)
		if (!strcmp(cat->defs[i].id, id))
			return i;
	return -1;
}

const trs_attach_node *trs_part_node(const trs_part_def *d, const char *name) {
	for (int i = 0; i < d->nnodes; i++)
		if (!strcmp(d->nodes[i].name, name))
			return &d->nodes[i];
	return NULL;
}

double trs_engine_isp(const trs_part_def *d, double pressure) {
	double isp = d->isp_vac + (d->isp_sl - d->isp_vac) * (pressure / TRS_P_REF);
	return isp > 0.01 * d->isp_vac ? isp : 0.01 * d->isp_vac;
}

double trs_engine_mdot(const trs_part_def *d) {
	return d->thrust_vac / (d->isp_vac * TRS_G0);
}

double trs_engine_air_factor(const trs_part_def *d, double density, double airspeed) {
	if (!d->air_breathing)
		return 1;
	double s = airspeed / fmax(d->jet_v_max, 1e-9);
	return density / fmax(d->jet_rho_ref, 1e-12) * fmax(0, 1 - s * s);
}

trs_v3 trs_leg_foot(const trs_part_def *d, double k) {
	double a = d->leg_angle * k;
	double len = d->leg_length * (d->leg_stowed_fraction + (1 - d->leg_stowed_fraction) * k);
	return v3_add(d->leg_hinge, v3(0, -len * cos(a), len * sin(a)));
}
