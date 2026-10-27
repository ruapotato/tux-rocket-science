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

/* Inside the ship. A penguin indoors is no extra body in the dynamics:
 * between the walls it simply falls freely, and the walls are what move.
 * Relative to the craft's centre of mass, in non-rotating axes, a free
 * penguin accelerates by minus the craft's sensed (non-gravitational)
 * acceleration (gravity pulls both alike; so does the Stillwater Drive,
 * which is why nobody feels it). That step is exact; turning the result
 * into the craft's rotating frame gives centrifugal weight and Coriolis
 * with no extra terms. Then the walls push back where it has gone into
 * them, and its feet grip the floor as hard as it is pressed onto it. */
#include "trs/world.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROOMS 256
#define COVER_TOL 0.01   /* m: a wall point this close inside another room is a doorway */
#define TOUCH_GAP 0.01   /* m of air still counted as touching */
#define PADDLE 0.6       /* m/s2 a floating penguin can paddle itself along with */
#define FLOAT_SPEED 1.0  /* m/s it paddles up to */
#define FEET_MU 1.0      /* grip of its feet */
#define SUBSTEPS 4
#define STAND_G 0.05     /* m/s2: below this nothing is a floor */

int trs_craft_rooms(const trs_craft *c, trs_room *out, int max) {
	int n = 0;
	for (int i = 0; i < c->count; i++) {
		const trs_part_def *d = trs_craft_def(c, i);
		for (int k = 0; k < d->nvolumes && n < max; k++) {
			const trs_volume *v = &d->volumes[k];
			trs_quat r = c->parts[i].rot;
			if (v->kind == TRS_VOL_BOX && v->yaw != 0)
				r = quat_normalize(quat_mul(r, quat_axis_angle(v3(0, 1, 0), v->yaw)));
			out[n++] = (trs_room){ i, v, c->parts[i].pos, r };
		}
	}
	return n;
}

static trs_v3 room_local(const trs_room *r, trs_v3 x) {
	return quat_rotate(quat_conj(r->rot), v3_sub(x, r->pos));
}

static double room_depth_local(const trs_volume *v, trs_v3 p) {
	if (v->kind == TRS_VOL_BOX)
		return fmin(fmin(fmin(p.x - v->lo.x, v->hi.x - p.x), fmin(p.y - v->lo.y, v->hi.y - p.y)),
				fmin(p.z - v->lo.z, v->hi.z - p.z));
	double rho = hypot(p.x, p.z);
	if (v->kind == TRS_VOL_CYLINDER && v->rt != v->r1) {
		/* tapering: the slant wall's distance, square to it */
		double sl = (v->rt - v->r1) / (v->y1 - v->y0);
		double R = v->r1 + sl * (p.y - v->y0);
		return fmin(fmin((R - rho) / sqrt(1 + sl * sl), p.y - v->y0), v->y1 - p.y);
	}
	double d = fmin(fmin(v->r1 - rho, p.y - v->y0), v->y1 - p.y);
	return v->kind == TRS_VOL_ANNULUS ? fmin(d, rho - v->r0) : d;
}

/* a cylinder's radius at height y (tapering from r1 at y0 to rt at y1) */
static double wall_r(const trs_volume *v, double y) {
	if (v->kind != TRS_VOL_CYLINDER || v->rt == v->r1)
		return v->r1;
	return v->r1 + (v->rt - v->r1) * (y - v->y0) / (v->y1 - v->y0);
}

static double room_depth(const trs_room *r, trs_v3 x) {
	return room_depth_local(r->v, room_local(r, x));
}

double trs_craft_room_depth(const trs_craft *c, trs_room *rooms, int n, trs_v3 x) {
	trs_room buf[MAX_ROOMS];
	if (!rooms) {
		rooms = buf;
		n = trs_craft_rooms(c, buf, MAX_ROOMS);
	}
	double best = -1e300;
	for (int i = 0; i < n; i++)
		best = fmax(best, room_depth(&rooms[i], x));
	return n ? best : -1e300;
}

/* Faces of a room: the closest point of face f to p (room-local) and its
 * normal into the room. Returns 0 past the last face. */
static int face(const trs_volume *v, int f, trs_v3 p, trs_v3 *at, trs_v3 *n) {
	if (v->kind == TRS_VOL_BOX) {
		if (f >= 6)
			return 0;
		trs_v3 q = v3(fmin(fmax(p.x, v->lo.x), v->hi.x), fmin(fmax(p.y, v->lo.y), v->hi.y), fmin(fmax(p.z, v->lo.z), v->hi.z));
		double lo[3] = { v->lo.x, v->lo.y, v->lo.z }, hi[3] = { v->hi.x, v->hi.y, v->hi.z }, qq[3] = { q.x, q.y, q.z };
		int ax = f / 2, hi_side = f % 2;
		qq[ax] = hi_side ? hi[ax] : lo[ax];
		double nn[3] = { 0, 0, 0 };
		nn[ax] = hi_side ? -1 : 1;
		*at = v3(qq[0], qq[1], qq[2]);
		*n = v3(nn[0], nn[1], nn[2]);
		return 1;
	}
	int nf = v->kind == TRS_VOL_ANNULUS ? 4 : 3;
	if (f >= nf)
		return 0;
	double rho = hypot(p.x, p.z);
	trs_v3 dir = rho > 1e-12 ? v3(p.x / rho, 0, p.z / rho) : v3(1, 0, 0);
	double y = fmin(fmax(p.y, v->y0), v->y1);
	if (f == 0 && v->kind == TRS_VOL_CYLINDER && v->rt != v->r1) {
		/* a tapering wall: the nearest point of its slant, in the plane through the axis */
		double ty = v->y1 - v->y0, tr = v->rt - v->r1, L = sqrt(ty * ty + tr * tr);
		double s = fmin(1, fmax(0, ((rho - v->r1) * tr + (p.y - v->y0) * ty) / (L * L)));
		double rr = v->r1 + tr * s, yy = v->y0 + ty * s;
		*at = v3(dir.x * rr, yy, dir.z * rr);
		*n = v3_add(v3_scale(dir, -ty / L), v3(0, tr / L, 0));
	} else if (f == 0) { /* the outer wall */
		*at = v3(dir.x * v->r1, y, dir.z * v->r1);
		*n = v3_neg(dir);
	} else if (f == 1 || f == 2) { /* the ends */
		double rr = fmin(fmax(rho, v->kind == TRS_VOL_ANNULUS ? v->r0 : 0), f == 2 ? wall_r(v, v->y1) : v->r1);
		*at = v3(dir.x * rr, f == 1 ? v->y0 : v->y1, dir.z * rr);
		*n = v3(0, f == 1 ? 1 : -1, 0);
	} else { /* an annulus's inner wall */
		*at = v3(dir.x * v->r0, y, dir.z * v->r0);
		*n = dir;
	}
	return 1;
}

/* A wall point is open where another room carries on past it. */
static int covered(const trs_room *rooms, int n, int self, trs_v3 x) {
	for (int j = 0; j < n; j++)
		if (j != self && room_depth(&rooms[j], x) > -COVER_TOL)
			return 1;
	return 0;
}

/* ---- the penguin --------------------------------------------------------- */

static const trs_part_def *penguin_def(const trs_part_catalog *cat) {
	for (int i = 0; i < cat->count; i++)
		if (cat->defs[i].is_eva)
			return &cat->defs[i];
	return NULL;
}

double trs_penguin_room_radius(const trs_part_catalog *cat) {
	const trs_part_def *pd = penguin_def(cat);
	return pd && pd->eva_room_radius > 0 ? pd->eva_room_radius : 0.35;
}

static double penguin_radius(const trs_part_def *pd) {
	return pd && pd->eva_room_radius > 0 ? pd->eva_room_radius : 0.35;
}

trs_v3 trs_part_seat(const trs_craft *c, int part) {
	const trs_part_def *d = trs_craft_def(c, part);
	if (d->nvolumes == 0)
		return c->parts[part].pos;
	trs_room rooms[MAX_ROOMS];
	int n = trs_craft_rooms(c, rooms, MAX_ROOMS);
	for (int i = 0; i < n; i++)
		if (rooms[i].part == part) {
			const trs_volume *v = rooms[i].v;
			/* on a ring's deck; in the middle of anything else */
			trs_v3 at = v->kind == TRS_VOL_BOX ? v3_scale(v3_add(v->lo, v->hi), 0.5)
				: v3(v->kind == TRS_VOL_ANNULUS ? v->r1 - penguin_radius(penguin_def(c->cat)) : 0, 0.5 * (v->y0 + v->y1), 0);
			return v3_add(rooms[i].pos, quat_rotate(rooms[i].rot, at));
		}
	return c->parts[part].pos;
}

int trs_walker_enter(trs_world *w, int id, int part) {
	if (id < 0 || id >= w->nvessels || !w->vessels[id].alive || !w->vessels[id].craft)
		return 0;
	trs_vessel *vs = &w->vessels[id];
	const trs_craft *c = vs->craft;
	if (part < 0 || part >= c->count || trs_craft_def(c, part)->nvolumes == 0)
		return 0;
	trs_room rooms[MAX_ROOMS];
	int n = trs_craft_rooms(c, rooms, MAX_ROOMS);
	const trs_room *r = NULL;
	for (int i = 0; i < n && !r; i++)
		if (rooms[i].part == part)
			r = &rooms[i];
	const trs_volume *v = r->v;
	/* the middle of the room (of an annulus: its cross-section at +X) */
	trs_v3 mid = v->kind == TRS_VOL_BOX ? v3_scale(v3_add(v->lo, v->hi), 0.5)
		: v3(v->kind == TRS_VOL_ANNULUS ? 0.5 * (v->r0 + v->r1) : 0, 0.5 * (v->y0 + v->y1), 0);
	if (v->kind == TRS_VOL_CYLINDER && v->rt != v->r1) {
		/* a tapering cabin: on the axis where there is most room */
		double best = -1e300;
		for (int k = 0; k <= 40; k++) {
			trs_v3 q = v3(0, v->y0 + (v->y1 - v->y0) * k / 40.0, 0);
			double dp = room_depth_local(v, q);
			if (dp > best)
				best = dp, mid = q;
		}
	}
	trs_walker *wk = &vs->walker;
	memset(wk, 0, sizeof *wk);
	wk->active = 1;
	wk->uid = c->parts[part].uid;
	wk->x = v3_add(r->pos, quat_rotate(r->rot, mid));
	wk->felt = trs_vessel_felt(w, id, wk->x);
	return 1;
}

void trs_walker_leave(trs_world *w, int id) {
	if (id >= 0 && id < w->nvessels)
		memset(&w->vessels[id].walker, 0, sizeof(trs_walker));
}

void trs_walker_command(trs_world *w, int id, trs_v3 walk, int jump) {
	if (id < 0 || id >= w->nvessels || !w->vessels[id].alive)
		return;
	w->vessels[id].walker.walk = walk;
	w->vessels[id].walker.jump |= jump;
}

/* Attitude, spin and sensed acceleration (SOI frame) of a vessel in any
 * mode: on the ground the surface holds it up and carries it round. */
static void frame_of(const trs_world *w, const trs_vessel *vs, double t, trs_quat *q, trs_v3 *om, trs_v3 *sensed) {
	const trs_body *b = &w->sys.bodies[vs->body];
	if (vs->mode == TRS_MODE_LANDED) {
		trs_v3 wb = trs_body_omega(b);
		trs_v3 r = quat_rotate(trs_body_quat(b, t), vs->fixed_r);
		double rl = v3_len(r);
		*q = quat_normalize(quat_mul(trs_body_quat(b, t), vs->fixed_q));
		*om = wb;
		/* its acceleration (round the axis) less gravity's */
		*sensed = v3_add(v3_cross(wb, v3_cross(wb, r)), v3_scale(r, b->mu / (rl * rl * rl)));
	} else {
		*q = vs->q;
		*om = vs->mode == TRS_MODE_RAILS ? v3(0, 0, 0) : vs->w;
		*sensed = vs->mode == TRS_MODE_PHYSICS ? vs->sensed_accel : v3(0, 0, 0);
	}
}

trs_v3 trs_vessel_felt(const trs_world *w, int id, trs_v3 x) {
	const trs_vessel *vs = &w->vessels[id];
	trs_quat q;
	trs_v3 om, s;
	frame_of(w, vs, w->t, &q, &om, &s);
	trs_quat qi = quat_conj(q);
	trs_v3 a = quat_rotate(qi, s), wl = quat_rotate(qi, om), d = v3_sub(x, vs->cm);
	return v3_sub(v3_neg(a), v3_cross(wl, v3_cross(wl, d)));
}

static trs_quat nlerp(trs_quat a, trs_quat b, double f) {
	double dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	double s = dot < 0 ? -1 : 1;
	trs_quat q = { a.x + (s * b.x - a.x) * f, a.y + (s * b.y - a.y) * f, a.z + (s * b.z - a.z) * f, a.w + (s * b.w - a.w) * f };
	return quat_normalize(q);
}

/* Walls push the penguin (centre x, radius rad) back into the rooms and
 * take the speed it had into them. Each surface it touches is a contact:
 * its normal and the speed it took (for friction). Returns how many. */
#define MAX_TOUCH 4
typedef struct {
	trs_v3 n;
	double took;
} touch_t;

/* the face of the rooms x is in that it is deepest into (s: its distance
 * from that face, < limit), skipping faces it already touches */
static int deepest(const trs_room *rooms, int n, trs_v3 x, double limit, const touch_t *tc, int nt, trs_v3 *wn, double *ws) {
	double best_depth = -1e300;
	int nearest = -1, found = 0;
	for (int i = 0; i < n; i++) {
		double dp = room_depth(&rooms[i], x);
		if (dp > best_depth)
			best_depth = dp, nearest = i;
	}
	double worst = limit;
	for (int i = 0; i < n; i++) {
		if (i != nearest && room_depth(&rooms[i], x) < -1e-9)
			continue; /* (or, lost outside them all, the nearest) */
		trs_v3 p = room_local(&rooms[i], x), at, nl;
		for (int f = 0; face(rooms[i].v, f, p, &at, &nl); f++) {
			trs_v3 atc = v3_add(rooms[i].pos, quat_rotate(rooms[i].rot, at));
			trs_v3 nc = quat_rotate(rooms[i].rot, nl);
			double s = v3_dot(v3_sub(x, atc), nc);
			if (s >= worst || covered(rooms, n, i, atc))
				continue;
			int seen = 0;
			for (int k = 0; k < nt; k++)
				seen |= v3_dot(tc[k].n, nc) > 0.999;
			if (seen)
				continue;
			worst = s, *wn = nc, found = 1;
		}
	}
	*ws = worst;
	return found;
}

static int collide(const trs_room *rooms, int n, double rad, trs_v3 *x, trs_v3 *v, touch_t *tc) {
	int nt = 0;
	trs_v3 wn;
	double s;
	for (int it = 0; it < 8 && deepest(rooms, n, *x, rad, NULL, 0, &wn, &s); it++) {
		*x = v3_madd(*x, wn, rad - s);
		int k = 0;
		while (k < nt && v3_dot(tc[k].n, wn) <= 0.999)
			k++;
		if (k == nt && nt < MAX_TOUCH)
			tc[nt++] = (touch_t){ wn, 0 };
		double vn = v3_dot(*v, wn);
		if (vn < 0) {
			*v = v3_madd(*v, wn, -vn);
			if (k < nt)
				tc[k].took += -vn;
		}
	}
	/* surfaces it rests against without pressing in */
	while (nt < MAX_TOUCH && deepest(rooms, n, *x, rad + TOUCH_GAP, tc, nt, &wn, &s))
		tc[nt++] = (touch_t){ wn, 0 };
	return nt;
}

void trs_walker_step(trs_world *w, trs_vessel *vs, trs_quat q0, trs_v3 w0, trs_v3 cm0, double h) {
	trs_walker *wk = &vs->walker;
	if (!wk->active || !vs->craft)
		return;
	trs_craft *c = vs->craft;
	int home = -1;
	for (int i = 0; i < c->count; i++)
		if (c->parts[i].uid == wk->uid)
			home = i;
	trs_room rooms[MAX_ROOMS];
	int n = trs_craft_rooms(c, rooms, MAX_ROOMS);
	if (home < 0 || n == 0) { /* its room has gone (staged away, or broken) */
		trs_walker_leave(w, (int)(vs - w->vessels));
		return;
	}
	const trs_part_def *pd = penguin_def(c->cat);
	double rad = penguin_radius(pd);
	double top = pd ? pd->eva_walk : 1.5, jump = pd ? pd->eva_jump : 2.0;
	trs_quat q1;
	trs_v3 w1, sensed;
	frame_of(w, vs, w->t, &q1, &w1, &sensed);
	if (vs->mode == TRS_MODE_LANDED) /* carried round at the body's own rate */
		q0 = quat_normalize(quat_mul(trs_body_quat(&w->sys.bodies[vs->body], w->t - h), vs->fixed_q)), w0 = w1;
	trs_v3 cm1 = vs->cm;
	(void)cm1;
	trs_v3 walk = wk->walk;
	double wl = v3_len(walk);
	if (wl > top)
		walk = v3_scale(walk, top / wl), wl = top;
	/* relative to the structure point that was the centre of mass at the
	 * start (the rigid motion of the craft is about it over this step) */
	trs_v3 p = quat_rotate(q0, v3_sub(wk->x, cm0));
	trs_v3 u = v3_add(quat_rotate(q0, wk->v), v3_cross(w0, p));
	double hs = h / SUBSTEPS;
	int touch = 0, standing = 0;
	trs_v3 normal = v3(0, 0, 0);
	trs_v3 alpha = v3_scale(v3_sub(w1, w0), 1.0 / h);
	for (int k = 1; k <= SUBSTEPS; k++) {
		double f = (double)k / SUBSTEPS;
		p = v3_add(p, v3_madd(v3_scale(u, hs), sensed, -0.5 * hs * hs));
		u = v3_madd(u, sensed, -hs);
		trs_quat qf = nlerp(q0, q1, f);
		trs_v3 wf = v3_add(w0, v3_scale(v3_sub(w1, w0), f));
		trs_quat qi = quat_conj(qf);
		trs_v3 x = v3_add(cm0, quat_rotate(qi, p));
		trs_v3 v = quat_rotate(qi, v3_sub(u, v3_cross(wf, p)));
		/* what it feels here (at rest), for its feet and the readout */
		touch_t tc[MAX_TOUCH];
		int nt = collide(rooms, n, rad, &x, &v, tc);
		trs_v3 wlc = quat_rotate(qi, wf), al = quat_rotate(qi, alpha), d = v3_sub(x, cm0);
		trs_v3 felt = v3_sub(v3_sub(v3_neg(quat_rotate(qi, sensed)), v3_cross(wlc, v3_cross(wlc, d))), v3_cross(al, d));
		touch = nt > 0;
		/* the surface its feet are on: the one it is pressed onto (a floor,
		 * against what it feels), else the one it hit hardest */
		double fl = v3_len(felt), took = 0;
		int fk = -1;
		standing = 0;
		for (int k = 0; k < nt; k++) {
			int floor = fl > STAND_G && v3_dot(tc[k].n, felt) < -0.5 * fl;
			if (floor && (!standing || tc[k].took > tc[fk].took))
				fk = k, standing = 1;
			else if (!standing && (fk < 0 || tc[k].took > tc[fk].took))
				fk = k;
		}
		trs_v3 nrm = fk >= 0 ? tc[fk].n : v3(0, 0, 0);
		if (fk >= 0)
			took = tc[fk].took, normal = nrm;
		if (touch) {
			/* feet (or flippers on a wall) grip as hard as they are pressed */
			trs_v3 vt = v3_madd(v, nrm, -v3_dot(v, nrm));
			trs_v3 want = v3_madd(walk, nrm, -v3_dot(walk, nrm));
			trs_v3 dv = v3_sub(want, vt);
			double grip = FEET_MU * took + (wl > 1e-6 ? PADDLE * hs : 0), dl = v3_len(dv);
			if (dl > grip)
				dv = v3_scale(dv, grip / dl);
			v = v3_add(v, dv);
			if (wk->jump) {
				v = v3_madd(v, nrm, jump);
				wk->jump = 0;
			}
		} else if (wl > 1e-6) {
			/* floating: paddles toward where it wants to go */
			trs_v3 want = v3_scale(walk, fmin(1.0, FLOAT_SPEED / wl));
			trs_v3 dv = v3_sub(want, v);
			double dl = v3_len(dv);
			if (dl > PADDLE * hs)
				dv = v3_scale(dv, PADDLE * hs / dl);
			v = v3_add(v, dv);
		}
		wk->felt = felt;
		p = quat_rotate(qf, v3_sub(x, cm0));
		u = v3_add(quat_rotate(qf, v), v3_cross(wf, p));
		wk->x = x;
		wk->v = v;
	}
	wk->jump = 0;
	wk->touching = touch;
	wk->standing = standing;
	wk->normal = normal;
	/* the room it is now in */
	double bd = -1e300;
	for (int i = 0; i < n; i++) {
		double dp = room_depth(&rooms[i], wk->x);
		if (dp > bd)
			bd = dp, wk->uid = c->parts[rooms[i].part].uid;
	}
}

/* ---- the walls, for the view ------------------------------------------------ */

typedef struct {
	double *tri;
	int *kind;
	int n, max;
} mesh_out;

/* a quad a b c d, wound so its normal (b - a) x (c - a) faces n, into the room */
static void emit(mesh_out *m, trs_v3 a, trs_v3 b, trs_v3 c, trs_v3 d, int kind, trs_v3 n) {
	if (v3_dot(v3_cross(v3_sub(b, a), v3_sub(c, a)), n) < 0) {
		trs_v3 t = b;
		b = d, d = t;
	}
	trs_v3 q[6] = { a, b, c, a, c, d };
	for (int t = 0; t < 2 && m->n < m->max; t++) {
		for (int k = 0; k < 3; k++) {
			m->tri[m->n * 9 + k * 3 + 0] = q[t * 3 + k].x;
			m->tri[m->n * 9 + k * 3 + 1] = q[t * 3 + k].y;
			m->tri[m->n * 9 + k * 3 + 2] = q[t * 3 + k].z;
		}
		m->kind[m->n++] = kind;
	}
}

/* Is a wall cell (centre at angle ang, height or radius `along`, `cell`
 * across) glass? A pane smaller than a cell still opens the one it is in. */
static int in_window(const trs_volume *v, int curved, double ang, double along, double cell) {
	for (int k = 0; k < v->nwindows; k++) {
		const trs_window_row *wr = &v->windows[k];
		double half = 0.5 * fmax(wr->size, cell * 1.01);
		if ((wr->count <= 0 && wr->nangles == 0) || fabs(along - wr->at) > half)
			continue;
		double rad = curved ? v->r1 : wr->at;
		if (wr->nangles) {
			for (int a = 0; a < wr->nangles; a++) {
				/* the panes' angles run from +Z toward +X; the walls' from +X toward +Z */
				double off = fmod(ang - (0.5 * TRS_PI - wr->angles[a]) + 5 * TRS_PI, 2 * TRS_PI) - TRS_PI;
				if (fabs(off) * rad < half)
					return 1;
			}
			continue;
		}
		double step = 2 * TRS_PI / wr->count;
		double off = fmod(ang + 0.5 * step + 4 * TRS_PI, step) - 0.5 * step;
		if (fabs(off) * rad < half)
			return 1;
	}
	return 0;
}

int trs_craft_room_mesh(const trs_craft *c, double cell, double *tri, int *kind, int max) {
	trs_room rooms[MAX_ROOMS];
	int n = trs_craft_rooms(c, rooms, MAX_ROOMS);
	mesh_out m = { tri, kind, 0, max };
	if (cell <= 0.01)
		cell = 0.01;
	for (int i = 0; i < n; i++) {
		const trs_room *r = &rooms[i];
		const trs_volume *v = r->v;
#define P(x, y, z) v3_add(r->pos, quat_rotate(r->rot, v3((x), (y), (z))))
		if (v->kind == TRS_VOL_BOX) {
			double lo[3] = { v->lo.x, v->lo.y, v->lo.z }, hi[3] = { v->hi.x, v->hi.y, v->hi.z };
			for (int f = 0; f < 6; f++) {
				int ax = f / 2, ua = (ax + 1) % 3, va = (ax + 2) % 3;
				int nu = (int)ceil((hi[ua] - lo[ua]) / cell), nv = (int)ceil((hi[va] - lo[va]) / cell);
				for (int a = 0; a < nu; a++)
					for (int b = 0; b < nv; b++) {
						double c4[4][3];
						for (int k = 0; k < 4; k++) {
							double ku = (a + (k == 1 || k == 2)) / (double)nu, kv = (b + (k >= 2)) / (double)nv;
							c4[k][ax] = f % 2 ? hi[ax] : lo[ax];
							c4[k][ua] = lo[ua] + (hi[ua] - lo[ua]) * ku;
							c4[k][va] = lo[va] + (hi[va] - lo[va]) * kv;
						}
						double mid[3];
						for (int k = 0; k < 3; k++)
							mid[k] = 0.25 * (c4[0][k] + c4[1][k] + c4[2][k] + c4[3][k]);
						if (covered(rooms, n, i, P(mid[0], mid[1], mid[2])))
							continue;
						double nn[3] = { 0, 0, 0 };
						nn[ax] = f % 2 ? -1 : 1;
						emit(&m, P(c4[0][0], c4[0][1], c4[0][2]), P(c4[1][0], c4[1][1], c4[1][2]), P(c4[2][0], c4[2][1], c4[2][2]),
								P(c4[3][0], c4[3][1], c4[3][2]), 1, quat_rotate(r->rot, v3(nn[0], nn[1], nn[2])));
					}
			}
			continue;
		}
		int ann = v->kind == TRS_VOL_ANNULUS;
		int nth = (int)fmax(12, ceil(2 * TRS_PI * v->r1 / cell));
		int ny = (int)fmax(1, ceil((v->y1 - v->y0) / cell));
		/* curved walls: the outer (a ring's deck), and an annulus's inner */
		for (int wall = 0; wall < 1 + ann; wall++) {
			int kd = ann ? (wall ? 3 : 2) : 0;
			for (int a = 0; a < nth; a++) {
				double t0 = 2 * TRS_PI * a / nth, t1 = 2 * TRS_PI * (a + 1) / nth, tm = 0.5 * (t0 + t1);
				for (int b = 0; b < ny; b++) {
					double y0 = v->y0 + (v->y1 - v->y0) * b / ny, y1 = v->y0 + (v->y1 - v->y0) * (b + 1) / ny;
					double ym = 0.5 * (y0 + y1);
					double R0 = wall ? v->r0 : wall_r(v, y0), R1 = wall ? v->r0 : wall_r(v, y1), Rm = 0.5 * (R0 + R1);
					if (covered(rooms, n, i, P(Rm * cos(tm), ym, Rm * sin(tm))))
						continue;
					int k = !wall && !ann && in_window(v, 1, tm, ym, fmax(y1 - y0, Rm * (t1 - t0))) ? 4 : kd;
					emit(&m, P(R0 * cos(t0), y0, R0 * sin(t0)), P(R0 * cos(t1), y0, R0 * sin(t1)), P(R1 * cos(t1), y1, R1 * sin(t1)),
							P(R1 * cos(t0), y1, R1 * sin(t0)), k, quat_rotate(r->rot, v3_scale(v3(cos(tm), 0, sin(tm)), wall ? 1 : -1)));
				}
			}
		}
		/* the flat ends */
		double rin = ann ? v->r0 : 0;
		int nr = (int)fmax(1, ceil((v->r1 - rin) / cell));
		for (int end = 0; end < 2; end++) {
			double y = end ? v->y1 : v->y0, rout = end ? wall_r(v, v->y1) : v->r1;
			for (int a = 0; a < nth; a++) {
				double t0 = 2 * TRS_PI * a / nth, t1 = 2 * TRS_PI * (a + 1) / nth, tm = 0.5 * (t0 + t1);
				for (int b = 0; b < nr; b++) {
					double r0 = rin + (rout - rin) * b / nr, r1 = rin + (rout - rin) * (b + 1) / nr, rm = 0.5 * (r0 + r1);
					if (covered(rooms, n, i, P(rm * cos(tm), y, rm * sin(tm))))
						continue;
					int k = ann && in_window(v, 0, tm, rm, fmax(r1 - r0, rm * (t1 - t0))) ? 4 : 1;
					emit(&m, P(r0 * cos(t0), y, r0 * sin(t0)), P(r1 * cos(t0), y, r1 * sin(t0)), P(r1 * cos(t1), y, r1 * sin(t1)),
							P(r0 * cos(t1), y, r0 * sin(t1)), k, quat_rotate(r->rot, v3(0, end ? -1 : 1, 0)));
				}
			}
		}
#undef P
	}
	return m.n;
}
