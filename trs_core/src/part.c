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
		if (d->thrust_vac <= 0 || d->isp_vac <= 0) {
			snprintf(err, errlen, "part %s: engine needs thrust_vac and isp_vac", d->id);
			return 0;
		}
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

trs_v3 trs_leg_foot(const trs_part_def *d, double k) {
	double a = d->leg_angle * k;
	double len = d->leg_length * (d->leg_stowed_fraction + (1 - d->leg_stowed_fraction) * k);
	return v3_add(d->leg_hinge, v3(0, -len * cos(a), len * sin(a)));
}
