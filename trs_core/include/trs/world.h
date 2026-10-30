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

/* The world: time, bodies, and vessels, plus the two integration modes.
 *
 *   PHYSICS  fixed 50 Hz RK4 in doubles, used under thrust or in atmosphere.
 *   RAILS    analytic Kepler from an epoch state; drift-free, warpable.
 *
 * Vessel states are always relative to their current SOI body, in a
 * non-rotating frame centred on that body (patched conics). */
#ifndef TRS_WORLD_H
#define TRS_WORLD_H

#include "trs/craft.h"
#include "trs/kepler.h"
#include "trs/quat.h"
#include "trs/system.h"

#define TRS_PHYSICS_HZ 50
#define TRS_PHYSICS_DT (1.0 / TRS_PHYSICS_HZ)

/* SOI hysteresis: exit slightly outside the sphere, enter slightly inside,
 * so a vessel sitting on the boundary can never ping-pong. */
#define TRS_SOI_EXIT_FACTOR (1.0 + 1e-7)
#define TRS_SOI_ENTER_FACTOR (1.0 - 1e-7)

typedef enum {
	TRS_MODE_PHYSICS = 0,
	TRS_MODE_RAILS = 1,
	TRS_MODE_LANDED = 2, /* fixed to the rotating surface (clamps, landed warp) */
	TRS_MODE_INTERSTELLAR = 3, /* past a star's heliopause: special relativity, no gravity (r, v relative to that star) */
} trs_mode;

typedef enum {
	TRS_EV_NONE = 0,
	TRS_EV_SOI_EXIT,    /* leaves current body's SOI into its parent */
	TRS_EV_SOI_ENTER,   /* enters a child body's SOI */
	TRS_EV_ATMOSPHERE,  /* reaches the top of the atmosphere (inbound) */
	TRS_EV_SURFACE,     /* reaches the surface of an airless body */
} trs_event_kind;

typedef struct {
	trs_event_kind kind;
	double t;
	int body; /* body the vessel is in after the event (or that it hits) */
} trs_event;

/* Force model hook for physics mode (thrust, drag, ...). Returns the
 * non-gravitational acceleration at the given state. M2 plugs vessels in. */
typedef trs_v3 (*trs_accel_fn)(void *user, int body, double t, trs_v3 r, trs_v3 v);

typedef enum {
	TRS_SAS_OFF = 0,
	TRS_SAS_STABILITY, /* hold the attitude captured when input is released */
	TRS_SAS_TARGET,    /* point the nose (+Y) along sas_target (SOI frame) */
	TRS_SAS_PROGRADE,  /* orbital velocity */
	TRS_SAS_RETROGRADE,
	TRS_SAS_NORMAL,
	TRS_SAS_ANTINORMAL,
	TRS_SAS_RADIAL_OUT,
	TRS_SAS_RADIAL_IN,
	TRS_SAS_SURF_PROGRADE, /* surface-relative velocity */
	TRS_SAS_SURF_RETROGRADE,
	TRS_SAS_UPRIGHT, /* local vertical, away from the body (penguins standing) */
	TRS_SAS_COUNT
} trs_sas_mode;


/* A penguin walking (or floating) around inside its ship. It moves in the
 * craft's own frame, so it feels exactly what anyone aboard would: the
 * ship's non-gravitational acceleration (thrust, drag, the ground holding
 * it up) as weight toward the stern, its spin as weight out toward the
 * rim (and Coriolis on the way), and nothing at all from the Stillwater
 * Drive, which pulls every atom alike. (interior.c) */
typedef struct {
	int active;
	int uid;      /* the part whose room it is in */
	trs_v3 x, v;  /* centre, craft-local, and velocity in the craft's frame */
	trs_v3 walk;  /* wanted velocity, craft-local, m/s */
	int jump;     /* push off the surface it touches, on the next step */
	int touching; /* against a wall last step */
	int standing; /* ... one it is pressed onto by what it feels */
	trs_v3 normal; /* that surface's normal, into the room, craft-local */
	trs_v3 felt;  /* apparent gravity where it is, craft-local, m/s2 */
} trs_walker;

/* A room of a craft: part `part`'s volume, placed (craft-local). */
typedef struct {
	int part;
	const trs_volume *v;
	trs_v3 pos;
	trs_quat rot; /* part rotation, and the box's yaw on top */
} trs_room;

/* Something that can be aimed at, shot or bitten: a vessel (by id and
 * serial), an animal of the wild (by its own id), or the ground. */
typedef enum { TRS_THING_NONE = 0, TRS_THING_VESSEL, TRS_THING_CRITTER, TRS_THING_GROUND } trs_thing_kind;
typedef struct {
	int kind;
	int id;
	int serial;
} trs_thing;

/* An animal near the focus, alive to what goes on round it (wild.c).
 * Until something disturbs it, it walks its loop exactly where the
 * scenery would draw it (trs_life_animal_at); after that it goes its own
 * way. Which one it is: the cell, its kind, and its place in that kind's
 * list for the cell. Positions body-fixed. */
typedef enum {
	TRS_CRIT_ROAM = 0, /* on its loop */
	TRS_CRIT_WANDER,   /* free, ambling */
	TRS_CRIT_FLEE,
	TRS_CRIT_FOLLOW,   /* curious, at a penguin's heels */
	TRS_CRIT_CHARGE,   /* hostile, going for something (and biting it) */
	TRS_CRIT_DOWN,     /* zapped: lies there a moment, then gone */
} trs_crit_state;
typedef struct {
	int id;
	trs_patch_id cell;
	int kind, idx;
	int state;
	double t_state;
	trs_v3 pos, vel; /* body-fixed, m and m/s */
	trs_v3 fwd;      /* the way it faces, body-fixed */
	double hp, size;
	trs_thing other; /* what it runs from, follows or goes for */
	double next_bite;
	trs_v3 centre;   /* its cell's centre: the loop is relative to it */
	trs_life_instance inst;
} trs_critter;
#define TRS_WILD_MAX 160
#define TRS_WILD_DEAD 256
#define TRS_WILD_CELLS 24
typedef struct {
	int body;
	trs_patch_id cell;
	int kind, idx;
	double until; /* the herd has made up the loss by then */
} trs_wild_dead;
typedef struct {
	int body;         /* where the live ones are (-1: none) */
	double acc;       /* time since the last step */
	double scan_t;    /* when it last looked for who is near */
	int n, next_id;
	trs_critter c[TRS_WILD_MAX];
	int ndead;
	trs_wild_dead dead[TRS_WILD_DEAD];
	/* what lives in the cells near (generated once a visit) */
	int ncells;
	struct {
		trs_patch_id p;
		int kind, n;
		trs_v3 centre;
		trs_life_instance *inst;
	} cells[TRS_WILD_CELLS * TRS_MAX_LIFE];
} trs_wild;

typedef struct {
	int alive;
	int body;
	trs_mode mode;
	trs_v3 r, v;      /* centre of mass; authoritative in PHYSICS mode */
	trs_orbit orbit;  /* authoritative in RAILS mode */
	trs_accel_fn accel; /* optional extra force (tests); NULL normally */
	void *accel_user;
	int soi_changes;

	/* Rigid assembly. NULL = point mass (used by orbit-only tests). */
	trs_craft *craft;
	trs_quat q;       /* vessel-local -> SOI frame */
	trs_v3 w;         /* angular velocity, SOI frame, rad/s */
	double throttle;  /* 0..1, liquid engines */
	trs_v3 input;     /* pilot: pitch (about local X), yaw (local Z), roll (local Y), -1..1 */
	trs_sas_mode sas;
	trs_quat sas_hold;
	double sas_settle; /* stick just let go: the spin rate (rad/s) being braked, 0 when holding */
	trs_v3 sas_target;
	int next_stage;   /* stage fired by the next trs_world_stage() */
	int rcs;          /* RCS thrusters armed */
	trs_v3 translate; /* RCS translation command, vessel-local, each in [-1, 1] */
	trs_v3 walk;      /* EVA: wanted ground speed, m/s: x east, y north (z unused) */
	int jump;         /* EVA: jump on the next tick that has a foot on the ground */
	int brakes;       /* wheel brakes on */
	double air_speed; /* speed through the air at the start of the tick (jets), m/s */
	/* electricity this tick (telemetry): generated and used, kW; in
	 * sunlight; powered (charge left or the sun covering the draw) */
	double ec_gen, ec_use;
	int sunlit, powered;
	double wheel_load; /* reaction wheels' effort last tick, 0..1 */
	double engine_power; /* share of what powered engines need that there is, 0..1 (this tick) */
	double engine_kw;    /* what they draw this tick, kW (telemetry) */
	/* mining last step (telemetry): ore dug and ore refined, kg/s, and
	 * their draw, kW; drill_ground: running drills that reach the ground
	 * (found while the craft rests, kept while it sits LANDED) */
	double ore_rate, refine_rate, isru_kw;
	int drill_ground;
	/* between the stars: proper velocity (gamma v; v is kept as u / gamma),
	 * how far the home calendar has run ahead of the ship's clock (s), the
	 * Stillwater Drive's throttle, and its push last tick (m/s2, unfelt) */
	trs_v3 u;
	double tau_lag;
	double drive;
	double drive_accel;
	/* a course to a star: course_star + 1 (0 = none); ship time left
	 * speeding up, then slowing down; the line it flies along */
	int course;
	double course_acc, course_dec;
	trs_v3 course_dir;
	double drill_gap; /* m from the nearest running drill to the ground (-1 = none to say) */
	trs_v3 rcs_rot;   /* RCS share of the rotation command per local axis (runtime) */
	trs_v3 rcs_force; /* RCS force last tick, vessel-local, N (telemetry) */
	double rcs_mdot;  /* monopropellant flow last tick, kg/s (telemetry) */
	double rcs_dv_expended; /* integral of |RCS thrust accel| dt */

	trs_walker walker;
	double spin;      /* spin hold: SAS turns the craft about its +Y at this rate (rad/s), 0 = off */

	/* lasers (combat.c): what they do by themselves (0 hold fire, 1 fire on
	 * what threatens the ship, 2 on anything alive but penguins), what
	 * they are locked on (they track it and fire while it's in reach), and
	 * fire by hand at a point (body-fixed: it turns with the ground) */
	int guard;
	trs_thing lock;
	int hand;
	trs_v3 hand_at;
	trs_thing guard_on;  /* what guard picked */
	double guard_pick_t; /* ... and when */
	/* a Jaeger at the controls (alien.c): its nest (body * TRS_MAX_NESTS +
	 * nest + 1; 0 = none), what it is doing, what it is after, its turn
	 * round its beat; and its lift drive's push this tick (m/s2, SOI) */
	int pilot;
	int pilot_state;
	trs_thing pilot_target;
	double pilot_t, pilot_phase;
	trs_v3 lift;
	trs_v3 lift_accel_now; /* what the drive gave last tick (telemetry) */
	int is_alien;     /* cache: any part aboard is the Jaegers' */
	int nlasers;      /* cache: lasers aboard */

	/* Telemetry (true numbers for the HUD) */
	trs_v3 sensed_accel; /* non-gravitational acceleration last tick, SOI frame */
	double dv_expended;  /* integral of |thrust accel| dt */
	double pressure, density, altitude;

	/* Surface */
	trs_v3 fixed_r;   /* body-fixed position in LANDED mode */
	trs_quat fixed_q; /* body-fixed attitude in LANDED mode */
	int serial;       /* unique for the world's lifetime: tells a reused slot from the vessel that died there */
	int clamped;      /* held by launch clamps until the first stage */
	int anchored;     /* fixed to the ground for good (a planted flag): stays LANDED */
	int lifted;       /* left the pad (its pieces inherit it): liftoff_t is set */
	double liftoff_t; /* when the clamps let go, s (mission clock zero) */
	int contacts;     /* contact points touching the ground last tick */
	int landed;       /* resting on the surface (contacts, slow) */
	double max_impact; /* worst contact speed seen, m/s (telemetry) */
	trs_v3 ext_force;  /* contact from other vessels this tick, SOI frame, N */
	trs_v3 ext_torque; /* ... and its torque about the CoM, vessel-local, N m */
	int fresh;         /* just created or split: overlaps found now are by design */
	int nghost;        /* vessels it passes through until they first come apart */
	int ghost[16];
	double ghost_until[16]; /* ... and not before this time (radial separations) */
	int npending; /* parts (by uid) to destroy after this tick */
	int pending_uid[64];
	double pending_speed[64];

	/* Caches rebuilt when the assembly changes */
	int dirty;
	double mass;
	trs_v3 cm;        /* vessel-local */
	trs_m3 inertia;   /* about cm, vessel-local */
	trs_fuel_net fuel;
	double *face_top, *face_bottom; /* exposed axial area per part */
	double *wake_top, *wake_bottom; /* side-heating factor with the flow from that end (1 = in the open) */
	double *shade_top, *shade_bottom; /* a surface-mounted part in the shadow of a wider stacked part ahead,
	                                   * the craft flying top (nose) or bottom first: its heating x this */
	int ncontact;
	trs_v3 *contact_pt;  /* vessel-local contact points */
	int *contact_part;
	unsigned char *contact_leg;
	double bound_radius; /* max distance of any contact point from the CoM */
} trs_vessel;

typedef enum {
	TRS_WEV_PART_DESTROYED = 1,
	TRS_WEV_VESSEL_CREATED,
	TRS_WEV_SOI_CHANGE,
	TRS_WEV_LANDED,
	TRS_WEV_LIFTOFF,
	TRS_WEV_FLAMEOUT,
	TRS_WEV_DOCKED,   /* vessel absorbed `other` (now gone) */
	TRS_WEV_UNDOCKED, /* vessel let go of `other` (new) */
	TRS_WEV_INTERSTELLAR, /* vessel passed star `other`'s heliopause, out into the space between the stars */
	TRS_WEV_DOWNED,   /* a Jaeger raider lost its pilot's hull over body `other`: wreckage now */
} trs_world_event_kind;

typedef struct {
	trs_world_event_kind kind;
	double t;
	int vessel;  /* vessel involved */
	int other;   /* new vessel id / body index */
	int uid;     /* part uid */
	char part[32];
	double speed;
} trs_world_event;

#define TRS_MAX_EVENTS 2048

#define TRS_MAX_VESSELS 256
#define TRS_MAX_PENDING 64
/* pending_speed / event speed codes for causes other than impact */
#define TRS_DESTROY_BLAST (-1.0)
#define TRS_DESTROY_OVERHEAT (-2.0)
#define TRS_DESTROY_CRUSH (-3.0) /* deep in a gas giant */
#define TRS_DESTROY_LASER (-4.0) /* shot to pieces */
#define TRS_DESTROY_BITTEN (-5.0) /* an animal's doing */

typedef struct {
	trs_system sys;
	double t, t_comp; /* world time with Kahan compensation */
	trs_vessel vessels[TRS_MAX_VESSELS];
	int nvessels;
	trs_world_event events[TRS_MAX_EVENTS];
	int nevents;
	int next_uid; /* part uids are unique across the whole world */
	int next_serial; /* vessel serials likewise (vessel slots are reused) */
	/* the vessel the player is with: the wild wakes round it, and the
	 * Jaegers' nests put up their ships when it comes near (id, serial) */
	int focus, focus_serial;
	trs_wild wild;
	/* each nest's losses: how many down, and when the last went */
	int nest_lost[TRS_MAX_BODIES][TRS_MAX_NESTS];
	double nest_lost_t[TRS_MAX_BODIES][TRS_MAX_NESTS];
	double nest_check_t;
	/* the ships the nests put up, by craft id (set by whoever loads the
	 * world; not saved, not freed with the vessels) */
	int nnest_crafts;
	char nest_craft_id[8][32];
	trs_craft *nest_craft[8];
} trs_world;

void trs_world_init(trs_world *w, const trs_system *sys, double t0);
void trs_world_free(trs_world *w);
/* The whole world (time, every vessel's state, every part's runtime state)
 * as JSON, and back: a restored world steps on exactly as the saved one.
 * from_json keeps w's star system and replaces every vessel; on error w is
 * untouched. (world_save.c) */
trs_json *trs_world_to_json(const trs_world *w);
int trs_world_from_json(trs_world *w, const trs_part_catalog *cat, const trs_json *j, char *err, size_t errlen);
int trs_world_add_vessel(trs_world *w, int body, trs_v3 r, trs_v3 v);
void trs_world_remove_vessel(trs_world *w, int id);
/* Adds a vessel built from a craft (copied). Position/velocity are for the
 * centre of mass; q orients the craft's local frame. Starts in PHYSICS. */
int trs_world_add_craft(trs_world *w, const trs_craft *c, int body, trs_v3 r, trs_v3 v, trs_quat q);
/* Places a craft on a body's surface at lat/lon (radians), nose up, held by
 * launch clamps until its first stage fires. Returns the vessel id. */
int trs_world_launch(trs_world *w, const trs_craft *c, int body, double lat, double lon);
/* A LANDED vessel (not anchored) raised or lowered so its lowest foot is on
 * the ground under it, as after its legs go out or in: none inside it. */
void trs_vessel_reseat(trs_world *w, int id);
void trs_world_push_event(trs_world *w, trs_world_event ev);
/* Body-frame attitude quaternion (rotation of the body since epoch). */
trs_quat trs_body_quat(const trs_body *b, double t);
/* Direction for a SAS mode at the vessel's current state (SOI frame). */
trs_v3 trs_sas_direction(const trs_world *w, int id, trs_sas_mode m);
/* Fires the vessel's next stage. New vessels created by decouplers are
 * written to new_ids (up to max); returns how many. */
int trs_world_stage(trs_world *w, int id, int *new_ids, int max);
/* Arms every stowed parachute (they open once the air allows). Returns how many. */
int trs_vessel_deploy_chutes(trs_world *w, int id);
/* Docking (docking.c). Latches every pair of free docking ports, on two
 * vessels flying under physics, whose faces have come within capture range
 * lined up face to face and slowly enough: the second vessel joins the
 * first (momentum kept). Called every physics step. */
void trs_world_dock_check(trs_world *w);
/* Joins vessel b to vessel a at ports pa (on a) and pb (on b). */
void trs_world_dock(trs_world *w, int a, int pa, int b, int pb);
/* Lets go at docking port `port` of vessel id (either side of a docked
 * pair). Returns the new vessel, or -1 if that port holds nothing. */
int trs_world_undock(trs_world *w, int id, int port);
/* Action group g (1..9) of vessel id: engines in it toggle (a solid only
 * lights), parachutes arm, legs toggle, wheels toggle the brakes, a docked
 * port lets go. Returns how many things happened. */
int trs_world_action_group(trs_world *w, int id, int g);
/* One part, told one thing (by the ship's computer): an engine's own
 * throttle (value 0..1, < 0 = follow the ship's; > 0 lights it), lighting
 * or stopping it, one decoupler fired (its pieces' ids into new_ids), a
 * chute armed or cut, legs out (value 1) or in (0). Returns -1 when the part
 * can't do that (or there is none with that uid), else the pieces made. */
typedef enum {
	TRS_PART_THROTTLE,
	TRS_PART_IGNITE,
	TRS_PART_SHUTDOWN,
	TRS_PART_DECOUPLE,
	TRS_PART_DEPLOY,
	TRS_PART_CUT,
} trs_part_action;
int trs_world_part_action(trs_world *w, int id, int uid, trs_part_action act, double value, int *new_ids, int max);
/* A craft on wheels on the runway east of the launch site at (lat, lon):
 * nose east, wheels down, brakes on. Returns the vessel id. */
int trs_world_launch_runway(trs_world *w, const trs_craft *c, int body, double lat, double lon);
/* EVA: a penguin (catalog part `penguin_def`) climbs out of crewed part
 * `part` of vessel `id` and stands beside its hull as a vessel of its own.
 * Returns the new vessel id or -1. */
int trs_world_eva_exit(trs_world *w, int id, int part, int penguin_def);
/* The crewed part of vessel `target` nearest penguin vessel `eva`, if that
 * part or a ladder on the craft is within the penguin's reach (it climbs
 * the ladder); -1 otherwise. *dist gets the gap to the nearest of them. */
int trs_eva_hatch(const trs_world *w, int eva, int target, double *dist);
/* A penguin on EVA (standing) plants a flag (catalog part `flag_def`) a
 * little ahead of it: a one-part vessel anchored to the ground for good.
 * Returns its id, or -1 (not standing on anything, or not a penguin). */
int trs_world_plant_flag(trs_world *w, int eva, int flag_def);
/* Is vessel id a penguin on EVA? */
int trs_vessel_is_eva(const trs_vessel *vs);
/* Solar power (kW) reaching vessel vs's panels now; *sunlit: out of shadow. */
double trs_vessel_solar(const trs_world *w, const trs_vessel *vs, int *sunlit);
/* One step of dt s of mining on a vessel: running drills dig while it
 * rests on the ground, running refineries turn ore into liquid
 * propellant; both stop where the batteries can't pay. */
void trs_vessel_isru(trs_world *w, trs_vessel *vs, double dt);
/* The Stillwater Drive's throttle (0..1); returns the drives aboard. */
int trs_world_set_drive(trs_world *w, int id, double throttle);
/* The drive's full push (m/s2) and power (kW) for the whole vessel. */
double trs_vessel_drive_max(const trs_vessel *vs, double *kw);
/* the Stillwater Drive is running on this ship: its path through warp is
 * pushed (world.c), not a conic */
int trs_vessel_drive_warps(const trs_vessel *vs);
/* Warps the world on by `dtau` of vessel id's own clock (it must be
 * between the stars): everything else moves by the home time that takes.
 * Stops early when its course arrives. Returns the home time that passed. */
double trs_world_warp_ship(trs_world *w, int id, double dtau);
/* Sets a course to star `star`: the drive flat out toward a point just
 * short of its heliopause, turning round exactly when what is left is
 * what stopping takes, and off on arrival. Returns 0 if the vessel isn't
 * between the stars or has no drive; star < 0 clears the course. Fills
 * the ship time and home time the trip will take. */
int trs_world_set_course(trs_world *w, int id, int star, double *tau, double *t);
/* internal: heliopause crossings, and dt of home time between the stars */
void trs_heliopause_check(trs_world *w, trs_vessel *vs);
void trs_interstellar_advance(trs_world *w, trs_vessel *vs, double dt);
/* Switches every drill and refinery aboard on or off; returns how many. */
int trs_world_set_isru(trs_world *w, int id, int on);
/* Interiors (interior.c). The rooms of a craft, craft-local; returns how
 * many (up to max). */
int trs_craft_rooms(const trs_craft *c, trs_room *out, int max);
/* How far point x (craft-local) is inside the craft's rooms: the depth
 * into the room it is deepest in (< 0: outside every room). */
double trs_craft_room_depth(const trs_craft *c, trs_room *rooms, int n, trs_v3 x);
/* The inside walls of every room, cut where another room carries on and
 * where windows are, as triangles about `cell` m across: 9 doubles each
 * (three craft-local corners) into tri, and a kind each into kind (0
 * curved wall, 1 flat wall, 2 a ring's deck, 3 a ring's ceiling, 4 glass).
 * Returns how many (up to max). */
int trs_craft_room_mesh(const trs_craft *c, double cell, double *tri, int *kind, int max);
/* Where part `part`'s crew are (craft-local): on a ring's deck, in the
 * middle of any other room, at the part itself if it has none. */
trs_v3 trs_part_seat(const trs_craft *c, int part);
/* A penguin of vessel id goes inside: into the first room of part `part`
 * (which must have one), at rest in the craft's frame. 0 if it can't. */
int trs_walker_enter(trs_world *w, int id, int part);
/* The ball a penguin takes up indoors (its "eva" room_radius), m. */
double trs_penguin_room_radius(const trs_part_catalog *cat);
void trs_walker_leave(trs_world *w, int id);
/* What the penguin is trying to do: walk (or paddle, floating) at `walk`
 * m/s (craft-local), and push off its surface. */
void trs_walker_command(trs_world *w, int id, trs_v3 walk, int jump);
/* Apparent gravity (m/s2, craft-local) felt at rest at craft-local x now. */
trs_v3 trs_vessel_felt(const trs_world *w, int id, trs_v3 x);
/* internal: one step of the penguin, given the craft's attitude, spin and
 * centre of mass at the start of the step (the vessel has already moved) */
void trs_walker_step(trs_world *w, trs_vessel *vs, trs_quat q0, trs_v3 w0, trs_v3 cm0, double h);

/* Local horizon frame at r (SOI frame): up, north, east. */
void trs_local_frame(trs_v3 r, trs_v3 *up, trs_v3 *north, trs_v3 *east);
/* Destroys part `part` of vessel `id` (impact speed, or a TRS_DESTROY_* code);
 * a part with propellant in it blasts its neighbours. */
void trs_vessel_destroy_part(trs_world *w, int id, int part, double speed);
/* Refreshes mass properties and caches after editing vessel->craft. */
void trs_vessel_refresh(trs_world *w, int id);
/* Heat flux (W/m^2) the jet of engine e, burning mdot kg/s at specific
 * impulse isp, puts into part i of the same craft (0 when i is out of the
 * jet), and the skin temperature (K) part i settles at under it. */
double trs_plume_flux(const trs_craft *c, int e, int i, double mdot, double isp);
double trs_plume_soak_temp(const trs_craft *c, int i, double flux);
/* share of the stagnation heat flux a part's leading end takes (1 flat, less tapering) */
double trs_lead_slant(const trs_part_def *d, double cs);
/* Vessel-vessel contact forces for this tick (vessel_contact.c). */
void trs_world_vessel_contacts(trs_world *w);
/* Lets vessel v pass through `other` until they are apart and t >= until. */
void trs_vessel_add_ghost(trs_vessel *v, int other, double until);
/* ---- the wild (wild.c) ---------------------------------------------- */
/* Where an undisturbed animal (instance li of kind L, in the cell centred
 * at `centre`) is at time t on its loop, and the way it faces: the
 * scenery draws it there too. Body-fixed. */
void trs_life_animal_at(const trs_life *L, const trs_life_instance *li, trs_v3 centre, double t, trs_v3 *pos, trs_v3 *fwd);
/* The vessel the player is with (-1: none): the wild and the nests wake round it. */
void trs_world_set_focus(trs_world *w, int id);
/* internal: dt of the wild (called every physics step; it moves at 10 Hz) */
void trs_wild_step(trs_world *w, double dt);
/* the live one with this id (-1: gone) */
int trs_wild_find(const trs_world *w, int id);
/* Hurts animal i: hp off, and it reacts to who did it. */
void trs_critter_hurt(trs_world *w, int i, double hp, trs_thing by);
/* Forgets the live animals (they are back on their loops) and the cells. */
void trs_wild_clear(trs_world *w);
/* Is it alive and one of ours as the lasers see it (a penguin)? Its
 * kind's data. */
const trs_life *trs_critter_kind(const trs_world *w, const trs_critter *c);

/* ---- lasers and damage (combat.c) ------------------------------------ */
/* Part `part` of vessel `id` takes hp of damage (code: how it went, a
 * TRS_DESTROY_*): past its def's hp it breaks after this tick. */
void trs_part_hurt(trs_world *w, int id, int part, double hp, double code);
/* Where a thing is now (SOI frame of body `body`) and how big (radius, m);
 * 0 if it's gone or elsewhere. */
int trs_thing_where(const trs_world *w, int body, trs_thing th, trs_v3 *at, double *radius);
/* What a ray from o along unit d (SOI frame of body `body`) meets first
 * within `range` m: a vessel's part (not vessel `skip`'s), an animal, the
 * ground. *dist gets how far; *part the part's index for a vessel. */
trs_thing trs_world_ray(const trs_world *w, int body, trs_v3 o, trs_v3 d, double range, int skip, double *dist, int *part);
/* The lasers' orders: guard mode (0..2), a lock (TRS_THING_NONE lets go),
 * fire by hand at a point (SOI frame), or not. Return the lasers aboard. */
int trs_world_set_guard(trs_world *w, int id, int mode);
int trs_world_lock(trs_world *w, int id, trs_thing th);
int trs_world_hand_fire(trs_world *w, int id, int on, trs_v3 at);
/* Something aboard that flies it (a pod, a probe, a penguin, a Jaeger's
 * hull): a craft, not wreckage. */
int trs_vessel_alive_craft(const trs_vessel *vs);
/* Whether a thing is alive and fair game for guard mode `mode` from vessel id. */
int trs_thing_threat(const trs_world *w, int id, trs_thing th, int mode);
/* A laser part's beam origin and its mount axis (SOI frame). */
void trs_laser_mount(const trs_world *w, const trs_vessel *vs, int part, trs_v3 *at, trs_v3 *axis);
/* internal: one tick of every vessel's lasers */
void trs_world_lasers_step(trs_world *w, double h);

/* ---- the Jaegers (alien.c) ------------------------------------------- */
/* The craft a nest puts up, by craft id (copied). */
void trs_world_set_nest_craft(trs_world *w, const char *craft_id, const trs_craft *c);
/* internal: the nests put up and take down their ships, every step */
void trs_world_nests_step(trs_world *w, double h);
/* internal: one tick of a Jaeger's piloting (lift push, aim, lasers) */
void trs_alien_pilot(trs_world *w, trs_vessel *vs, double h);
/* A raider has lost its pilot (its hull broke): the nest counts the loss,
 * and what is left is wreckage. */
void trs_alien_downed(trs_world *w, int id);
/* Takes every Jaeger ship home (before a rails warp). Returns how many. */
int trs_world_recall_aliens(trs_world *w);
/* Where nest k of body b hangs its ships (body-fixed). */
trs_v3 trs_nest_home(const trs_body *b, int k);

/* Linear and angular momentum (about the SOI body centre) of one vessel. */
void trs_vessel_momentum(const trs_world *w, int id, trs_v3 *p, trs_v3 *L);

/* Current state relative to the vessel's SOI body, whatever its mode. */
void trs_vessel_state(const trs_world *w, int id, trs_v3 *r, trs_v3 *v);
/* Osculating orbit at the current time, whatever its mode. */
void trs_vessel_orbit(const trs_world *w, int id, trs_orbit *o);
void trs_vessel_set_mode(trs_world *w, int id, trs_mode mode);
void trs_vessel_set_state(trs_world *w, int id, int body, trs_v3 r, trs_v3 v);

/* One fixed physics tick for every PHYSICS vessel (rails vessels just ride
 * along analytically). Handles SOI transitions at tick boundaries. */
void trs_world_step_physics(trs_world *w);

/* Advance by dt on rails. Every vessel must be on rails. Stops early when
 * vessel `stop_on` (or -1 for none) hits atmosphere/surface; returns that
 * event in *stop (kind NONE when the full dt elapsed). */
void trs_world_warp(trs_world *w, double dt, int stop_on, trs_event *stop);

/* Next patched-conic event for an orbit around `body`, in [t, t_max]. */
trs_event trs_next_event(const trs_system *sys, int body, const trs_orbit *o, double t, double t_max);

/* Re-expresses a state from `from` body to `to` body (parent or child). */
void trs_rebase_state(const trs_system *sys, int from, int to, double t, trs_v3 *r, trs_v3 *v);

#endif
