# Tux Rocket Science
# Copyright (C) 2026 Tux Rocket Science contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
## A flight in progress, with no rendering: owns a trs_core world, the active
## vessel, warp rules and staging. The flight scene renders one of these; the
## control socket and the gates drive one directly ("blind").
class_name FlightSession
extends RefCounted

signal sim_event(e: Dictionary)
signal vessel_switched(id: int)

const PHYSICS_HZ := 50
const TICK := 1.0 / PHYSICS_HZ
const PHYSICS_WARP: Array[int] = [1, 2, 3, 4]
const RAILS_WARP: Array[float] = [1.0, 5.0, 10.0, 50.0, 100.0, 1000.0, 10000.0, 100000.0]
const PHYSICS_RANGE := 2500.0  # m: beyond this other vessels are packed on rails

var core: TrsSim
## The vessel being flown. Vessel slots are reused once a vessel is gone,
## so each assignment also notes the vessel's serial: a new piece of debris
## landing in the same slot is not mistaken for the vessel still flying.
var active := -1:
	set(v):
		active = v
		_active_serial = core.vessel_serial(v) if core != null and v >= 0 else -1
var _active_serial := -1
var launch_text := ""
var launch_body := "floe"
var launch_site := "pad"  # or "runway" (planes: on their wheels, nose east)
const RUNWAY_M := 400.0   # the runway's west end, metres east of the pad
const YARD_ALT := 250000.0  # m: the Orbital Yard's circular equatorial orbit
var warp_kind := "physics"  # "physics" or "rails"
var warp_index := 0
const PE_CLEAR := 10000.0  # m a low point must clear the air top (or sea level) by to warp freely
const WARP_TO_LEAD := 60.0  # s of sim time a skip-ahead stops short of the ground or the air
const WARP_LEAD := 15.0  # s of real time on-rails warp keeps before the ground or the air
var ticks := 0
var events: Array[Dictionary] = []
## Maneuver nodes: [[t, prograde, normal, radial], ...] in time order
var nodes: Array = []
var _accum := 0.0
var _telemetry: Dictionary = {}
var _telemetry_tick := -1
var _pre_launch := ""  # the world just before the last launch (for reverts)
var target := -1:  # the vessel picked as target (-1: none)
	set(v):
		target = v
		_target_serial = core.vessel_serial(v) if core != null and v >= 0 else -1
var _target_serial := -1


func _init() -> void:
	core = TrsSim.new()
	core.load_system(ProjectSettings.globalize_path(Sim.SYSTEM_PATH))
	core.load_catalog(ProjectSettings.globalize_path(Sim.CATALOG_PATH))


## Resets the world and puts the craft on the body's launch pad.
## fresh: a new, empty world (tests, the tutorial); otherwise the craft
## joins the world as it is, with everything else still flying.
func launch(craft_text: String, body := "floe", fresh := true) -> bool:
	if fresh:
		core.load_system(ProjectSettings.globalize_path(Sim.SYSTEM_PATH))
	launch_text = craft_text
	launch_body = body
	_pre_launch = core.world_save()
	if launch_site == "orbit":
		active = core.flight_spawn_orbit(craft_text, body, YARD_ALT, 0.0)
	else:
		active = core.flight_launch_runway(craft_text, body) if launch_site == "runway" else core.flight_launch(craft_text, body)
	ticks = 0
	warp_kind = "physics"
	warp_index = 0
	events.clear()
	nodes.clear()
	_telemetry_tick = -1
	if active < 0:
		push_error("launch failed: " + core.last_error())
		return false
	core.vessel_set_sas(active, "stability", PackedFloat64Array())
	return true


## Starts in a circular orbit (debug / tests).
func spawn_in_orbit(craft_text: String, body: String, altitude: float, inc_deg := 0.0) -> bool:
	core.load_system(ProjectSettings.globalize_path(Sim.SYSTEM_PATH))
	launch_text = craft_text
	launch_body = body
	active = core.flight_spawn_orbit(craft_text, body, altitude, inc_deg)
	ticks = 0
	_telemetry_tick = -1
	return active >= 0


## Puts the world back as it was just before the launch and launches again.
func revert_to_launch() -> bool:
	if _pre_launch == "" or not core.world_load(_pre_launch):
		return launch(launch_text, launch_body)
	return launch(launch_text, launch_body, false)


## Puts the world back as it was before the launch (the craft never flew).
func revert_before_launch() -> void:
	if _pre_launch != "":
		core.world_load(_pre_launch)
	active = -1
	_telemetry_tick = -1


## Vessels sitting on (or right by) the launch pad of `body`, which a new
## launch would land on top of: [{id, name, ...}] from vessel_list.
func pad_blockers(body := "floe") -> Array:
	var out := []
	if launch_site == "orbit":
		return out
	var at := RUNWAY_M if launch_site == "runway" else 0.0
	for v in JSON.parse_string(core.vessel_list()):
		if v.body == body and v.situation in ["prelaunch", "landed"] and v.site_m >= 0.0 and absf(v.site_m - at) < 60.0:
			out.append(v)
	return out


func time() -> float:
	return core.time()


# ---- controls --------------------------------------------------------------

var throttle_set := 0.0  # the last throttle asked for

func set_throttle(x: float) -> void:
	throttle_set = x
	core.vessel_set_throttle(active, x)
	core.vessel_set_drive(active, x)  # a Stillwater Drive aboard follows the throttle
	_telemetry_tick = -1


func set_input(pitch: float, yaw: float, roll: float) -> void:
	core.vessel_set_input(active, pitch, yaw, roll)


func set_sas(mode: String, target := PackedFloat64Array()) -> bool:
	_telemetry_tick = -1
	return core.vessel_set_sas(active, mode, target)


func set_rcs(on: bool) -> void:
	core.vessel_set_rcs(active, on)
	_telemetry_tick = -1


## RCS translation, vessel-local (Y = towards the nose), each in [-1, 1].
## A penguin on EVA: wanted ground speed east / north (m/s), and a jump.
func set_walk(east: float, north: float, jump := false) -> void:
	core.vessel_set_walk(active, east, north, jump)


func set_translate(x: float, y: float, z: float) -> void:
	core.vessel_set_translate(active, x, y, z)


func set_legs(deployed: bool) -> void:
	core.vessel_set_legs(active, deployed)


## Arms every stowed parachute; each opens once the air is thick and slow
## enough. Returns how many were armed.
func deploy_chutes() -> int:
	_telemetry_tick = -1
	return core.vessel_deploy_chutes(active)


func stage() -> Array:
	if warp_kind == "rails" and warp_index > 0:
		set_warp("rails", 0)
	var created: Array = JSON.parse_string(core.vessel_stage(active))
	_telemetry_tick = -1
	_collect_events()
	return created


## Where the target is and how it moves relative to us, and how the
## nearest pair of free docking ports lines up ({} without a target).
func target_info() -> Dictionary:
	if target < 0 or not still(target, _target_serial) or target == active:
		target = -1
		return {}
	return JSON.parse_string(core.target_info(active, target))


const APPROACH_ORBITS := 3.0
## How far ahead to look for a closest approach: APPROACH_ORBITS orbits
## (or a few hours on an open path).
func approach_horizon() -> float:
	var per: float = float(telemetry().get("orbit", {}).get("period", 0.0))
	return clampf(per * APPROACH_ORBITS if per > 0.0 else 2.0e4, 600.0, 2.0e6)


## The closest approach to the target along our path (planned burns and
## all): {t, dist, rel_speed, body_index, pos, target_pos, edge}, or {}.
## edge: still closing when the look-ahead ends (so not a true pass).
func closest_approach() -> Dictionary:
	if target_info().is_empty():
		return {}
	var hz := approach_horizon()
	var ca: Dictionary = JSON.parse_string(core.closest_approach(active, target, pack_nodes(nodes), hz))
	if not ca.is_empty():
		ca["edge"] = float(ca.t) > time() + hz - 1.0
	return ca


## Picks the next target: the other vessels in order of distance.
func cycle_target() -> int:
	var others := []
	for v in JSON.parse_string(core.vessel_list()):
		if int(v.id) != active:
			others.append([_distance_to_active(int(v.id)), int(v.id)])
	others.sort()
	if others.is_empty():
		target = -1
		return target
	var ids := others.map(func(x): return x[1])
	var k := ids.find(target)
	target = ids[(k + 1) % ids.size()] if k < ids.size() - 1 else -1
	return target


## Lets go at the first docked port: the new vessel's id, or -1.
func undock() -> int:
	var nid: int = core.vessel_undock(active)
	_telemetry_tick = -1
	_collect_events()
	return nid


func switch_to(id: int) -> void:
	if core.vessel_alive(id):
		active = id
		_telemetry_tick = -1
		vessel_switched.emit(id)


# ---- time ------------------------------------------------------------------

## Requests a warp level; returns "" or the reason it was refused.
func set_warp(kind: String, index: int) -> String:
	if index <= 0:
		_end_rails()
		warp_kind = "physics"
		warp_index = 0
		return ""
	if kind == "physics":
		_end_rails()
		warp_kind = "physics"
		warp_index = clampi(index, 0, PHYSICS_WARP.size() - 1)
		return ""
	if warp_kind == "rails" and index < warp_index:
		warp_index = index  # slowing down is always allowed
		return ""
	var reason := rails_refusal()
	if reason != "":
		return reason
	var cap := rails_cap(telemetry())
	if cap <= 0:
		return "too close to the ground at this descent rate"
	if warp_kind != "rails":
		_begin_rails()
	warp_kind = "rails"
	warp_index = clampi(index, 1, cap)
	return ""


## One step up (dir > 0) or down the warp ladder: physics 1x..4x, then on
## to the rails rates when they are allowed. "" or why it went no further.
func step_warp(dir: int) -> String:
	var kind := warp_kind
	var idx := warp_index + dir
	if warp_index == 0 and dir > 0:
		kind = "rails" if rails_refusal() == "" else "physics"
		idx = 1
	elif kind == "physics" and idx >= PHYSICS_WARP.size():
		var why := rails_refusal()
		if why != "":
			return "%dx is the limit %s" % [PHYSICS_WARP[-1], why.trim_prefix("cannot warp ")]
		kind = "rails"
		idx = 1
	if kind == "physics":
		idx = clampi(idx, 0, PHYSICS_WARP.size() - 1)
	var reason := set_warp(kind, idx)
	return "Warp refused: " + reason if reason != "" else ""


## The fastest on-rails warp level that still leaves WARP_LEAD seconds of
## real time before the craft reaches the ground (or the top of the air) at
## its present descent rate; 0 means none.
func rails_cap(t: Dictionary) -> int:
	if not t.get("alive", false):
		return 0
	var h: float = t.altitude_terrain
	var top: float = t.get("atmosphere_height", 0.0)
	if top > 0.0 and t.altitude > top:
		h = minf(h, t.altitude - top)
	var down := -float(t.vertical_speed)
	if t.get("interstellar", false) or t.landed or down <= 0.0:
		return RAILS_WARP.size() - 1
	# a path whose low point clears the air (or the mountains) comes to no harm
	var pe = t.get("orbit", {}).get("pe_alt", null)
	if pe != null and float(pe) > (top if top > 0.0 else 0.0) + PE_CLEAR:
		return RAILS_WARP.size() - 1
	var cap := 0
	for i in RAILS_WARP.size():
		if i > 0 and RAILS_WARP[i] * WARP_LEAD <= h / down:
			cap = i
	return cap


## Between the stars with the drive braking: the fastest warp level at
## which a frame takes off no more than a tenth of the speed that is left.
func braking_cap(t: Dictionary, real_dt: float) -> int:
	var acc: float = t.get("drive_accel", 0.0)
	var u: float = t.get("proper_speed", 0.0)
	var vg: Array = t.get("velocity_galactic", [0, 0, 0])
	var nose: Array = t.get("nose", [0, 1, 0])
	var along: float = vg[0] * nose[0] + vg[1] * nose[1] + vg[2] * nose[2]
	if acc <= 0.0 or along >= 0.0:
		return RAILS_WARP.size() - 1
	var cap := 0
	for i in RAILS_WARP.size():
		if i > 0 and RAILS_WARP[i] * maxf(real_dt, 1.0 / 60.0) * acc <= 0.1 * u:
			cap = i
	return cap


## Why on-rails warp is not allowed right now ("" if it is).
func rails_refusal() -> String:
	var t := telemetry()
	if not t.get("alive", false):
		return "no vessel"
	if t.get("interstellar", false):
		return ""  # out here the drive burns through warp, by the ship's clock
	if float(t.get("drive_accel", 0.0)) > 0.0:
		return "cannot warp while the drive pushes inside a star's heliopause"
	if t.landed and t.get("thrust", 0.0) <= 0.0:
		return ""
	if t.get("thrust", 0.0) > 0.0:
		return "cannot warp while under thrust"
	if t.in_atmosphere:
		return "cannot warp in the atmosphere"
	return ""


func _begin_rails() -> void:
	var vessels: Array = JSON.parse_string(core.vessel_list())
	for v in vessels:
		var tv: Dictionary = JSON.parse_string(core.telemetry(v.id))
		if tv.landed:
			core.vessel_set_mode(v.id, "landed")
		elif tv.in_atmosphere and v.id != active:
			core.vessel_remove(v.id)  # debris inside the air is not tracked on rails
		else:
			core.vessel_set_mode(v.id, "rails")
	_telemetry_tick = -1


func _end_rails() -> void:
	if warp_kind != "rails":
		return
	var vessels: Array = JSON.parse_string(core.vessel_list())
	for v in vessels:
		if v.id == active or _distance_to_active(v.id) < PHYSICS_RANGE:
			core.vessel_set_mode(v.id, "physics")
	_telemetry_tick = -1


func _distance_to_active(id: int) -> float:
	var r := core.vessel_render(id, active)
	if r.size() < 4:
		return INF
	return Vector3(r[1], r[2], r[3]).length()


## Advances by fixed ticks regardless of warp (blind control, gates).
func step_ticks(n: int) -> void:
	for i in n:
		core.step_physics(1)
		ticks += 1
		if ticks % 25 == 0:
			_manage_ranges()
	_collect_events()
	_telemetry_tick = -1


## Advances by a frame of real time, honouring the warp setting.
func advance(real_dt: float) -> void:
	if warp_kind == "rails" and warp_index > 0:
		# on rails nothing burns: throttling up hands the craft back to physics
		var t := telemetry()
		if t.get("interstellar", false):
			# between the stars warp runs on the ship's clock, drive and all;
			# braking, it eases off so the last of the speed comes off gently
			var cap := braking_cap(t, real_dt)
			if cap < warp_index:
				set_warp("rails", cap)
				events.append({"kind": "warp_eased", "rate": RAILS_WARP[cap] if cap > 0 else 1.0, "t": time()})
				if cap <= 0:
					return
			core.warp_ship(active, real_dt * RAILS_WARP[warp_index])
			_collect_events()
			_telemetry_tick = -1
			if t.has("course") and not telemetry().has("course"):
				set_warp("rails", 0)
				events.append({"kind": "arrived", "body": t.course_name, "t": time()})
			return
		if (t.get("thrust", 0.0) > 0.0 or float(t.get("drive", 0.0)) > 0.0 and float(t.get("drive_max", 0.0)) > 0.0) and not t.landed:
			set_warp("rails", 0)
			events.append({"kind": "warp_stopped", "reason": "engines firing", "t": time()})
			return
		# closing on the ground or the air, warp eases off level by level
		var cap := rails_cap(t)
		if cap < warp_index:
			set_warp("rails", cap)
			events.append({"kind": "warp_eased", "rate": RAILS_WARP[cap] if cap > 0 else 1.0, "t": time()})
			if cap <= 0:
				return
		var ev: Dictionary = JSON.parse_string(core.warp(real_dt * RAILS_WARP[warp_index], active))
		if ev.event == "refused":
			set_warp("rails", 0)
		elif ev.event == "atmosphere" or ev.event == "surface":
			set_warp("rails", 0)
			events.append({"kind": "warp_stopped", "reason": ev.event, "t": ev.t})
		_collect_events()
		_telemetry_tick = -1
		return
	_accum += real_dt * PHYSICS_WARP[warp_index]
	var n := int(_accum / TICK)
	_accum -= n * TICK
	n = mini(n, 8 * PHYSICS_WARP[warp_index])  # never spiral on slow frames
	if n > 0:
		step_ticks(n)


func _manage_ranges() -> void:
	# distances are to the active vessel: if it just broke up, follow a
	# surviving piece first, or every piece would look infinitely far away
	if not still(active, _active_serial):
		_pick_new_active()
		if active < 0:
			return
	var vessels: Array = JSON.parse_string(core.vessel_list())
	for v in vessels:
		if v.id == active:
			continue
		var d := _distance_to_active(v.id)
		if d > PHYSICS_RANGE and v.mode == "physics":
			var tv: Dictionary = JSON.parse_string(core.telemetry(v.id))
			if tv.landed:
				core.vessel_set_mode(v.id, "landed")
			elif tv.in_atmosphere or tv.situation == "suborbital":
				core.vessel_remove(v.id)
			else:
				core.vessel_set_mode(v.id, "rails")
		elif d < PHYSICS_RANGE * 0.9 and v.mode != "physics":
			core.vessel_set_mode(v.id, "physics")


func _collect_events() -> void:
	var evs: Array = JSON.parse_string(core.events_drain())
	for e in evs:
		events.append(e)
		# docking absorbs one vessel into the other: follow it there
		if e.kind == "docked":
			if int(e.other) == active:
				switch_to(int(e.vessel))
			if int(e.other) == target or int(e.vessel) == target:
				target = -1
		sim_event.emit(e)
	if not still(active, _active_serial):
		_pick_new_active()


## If the active vessel is gone (crash), follow the piece with the crew,
## else the biggest piece.
func _pick_new_active() -> void:
	var vessels: Array = JSON.parse_string(core.vessel_list())
	var best := -1
	var score := -1.0
	for v in vessels:
		if v.mode != "physics":
			continue  # far away: not a piece of what just broke up
		var s: float = v.get("crew", 0) * 1000.0 + v.get("parts", 0)
		if s > score:
			score = s
			best = v.id
	if best >= 0:
		switch_to(best)
	else:
		active = -1


## Vessel `id` is alive and still the one that had `serial` (not a new
## vessel in its reused slot).
func still(id: int, serial: int) -> bool:
	return id >= 0 and serial >= 0 and core.vessel_serial(id) == serial


func telemetry() -> Dictionary:
	if _telemetry_tick != ticks or _telemetry.is_empty():
		_telemetry = JSON.parse_string(core.telemetry(active)) if active >= 0 else {"alive": false}
		_telemetry_tick = ticks
	return _telemetry


## Warps on rails to absolute time t (or until something interrupts).
## Returns "" on arrival or the reason it stopped.
func warp_to(t_target: float) -> String:
	var reason := rails_refusal()
	if reason != "":
		return reason
	var stopped := ""
	# a path that meets the ground (or the air) first: stop well short of it
	# (a craft resting on the ground is going nowhere)
	for p in [] if telemetry().get("landed", false) else trajectory([], 8, maxf(t_target - time(), 1.0)):
		if p.end in ["surface", "atmosphere"] and float(p.t_end) < t_target:
			t_target = maxf(time(), float(p.t_end) - WARP_TO_LEAD)
			stopped = "%s ahead" % ("the ground" if p.end == "surface" else "the atmosphere")
			break
	_begin_rails()
	for guard in 100000:
		var now := time()
		if now >= t_target - 1e-6:
			break
		var ev: Dictionary = JSON.parse_string(core.warp(minf(t_target - now, 21600.0), active))
		if ev.event == "refused":
			stopped = "refused"
			break
		if ev.event == "atmosphere" or ev.event == "surface":
			stopped = ev.event
			break
	if stopped.ends_with("ahead"):
		stopped += " (stopped %d s before)" % int(WARP_TO_LEAD)
	_end_rails_now()
	warp_kind = "physics"
	warp_index = 0
	_collect_events()
	_telemetry_tick = -1
	return stopped


func _end_rails_now() -> void:
	warp_kind = "rails"
	_end_rails()


## nodes: [[t, prograde, normal, radial], ...]
## Moves node i by whole orbits (of the path it sits on, the burns before
## it counted) later (+) or earlier (-). Returns its new index, or -1 with
## why in `why` (an open path has no next orbit; the past is gone).
var shift_why := ""
func shift_node(i: int, orbits: int) -> int:
	shift_why = ""
	if i < 0 or i >= nodes.size():
		shift_why = "no such node"
		return -1
	var t: float = nodes[i][0]
	var before := nodes.slice(0, i)
	var patches: Array = JSON.parse_string(core.trajectory(active, pack_nodes(before), 8, maxf(t - time(), 0.0) + 1.0))
	var period := 0.0
	for p in patches:
		if float(p.t_start) <= t + 1e-6 and (float(p.t_end) >= t - 1e-6 or p.end == "horizon"):
			period = float(p.period) if p.ap_alt != null else 0.0
	if period <= 0.0:
		shift_why = "the path there doesn't come round again"
		return -1
	var nt := t + orbits * period
	if nt < time() + 1.0:
		shift_why = "that orbit is already behind you"
		return -1
	var nd: Array = nodes[i].duplicate()
	nd[0] = nt
	nodes.remove_at(i)
	nodes.append(nd)
	nodes.sort_custom(func(a, b): return a[0] < b[0])
	return nodes.find(nd)


static func pack_nodes(nodes: Array) -> PackedFloat64Array:
	var f := PackedFloat64Array()
	for n in nodes:
		for x in n:
			f.append(float(x))
	return f


func trajectory(nodes: Array = [], patches := 6, horizon := 2.0e6) -> Array:
	return JSON.parse_string(core.trajectory(active, pack_nodes(nodes), patches, horizon))


func node_vector(nodes: Array, index: int) -> Vector3:
	var v: PackedFloat64Array = core.node_vector(active, pack_nodes(nodes), index)
	return Vector3(v[0], v[1], v[2])  # sim axes


func predict(patches := 4, horizon := 4.0e6) -> Array:
	return JSON.parse_string(core.predict(active, patches, horizon))
