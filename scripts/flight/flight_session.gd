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
var active := -1
var launch_text := ""
var launch_body := "floe"
var warp_kind := "physics"  # "physics" or "rails"
var warp_index := 0
var ticks := 0
var events: Array[Dictionary] = []
## Maneuver nodes: [[t, prograde, normal, radial], ...] in time order
var nodes: Array = []
var _accum := 0.0
var _telemetry: Dictionary = {}
var _telemetry_tick := -1


func _init() -> void:
	core = TrsSim.new()
	core.load_system(ProjectSettings.globalize_path(Sim.SYSTEM_PATH))
	core.load_catalog(ProjectSettings.globalize_path(Sim.CATALOG_PATH))


## Resets the world and puts the craft on the body's launch pad.
func launch(craft_text: String, body := "floe") -> bool:
	core.load_system(ProjectSettings.globalize_path(Sim.SYSTEM_PATH))
	launch_text = craft_text
	launch_body = body
	active = core.flight_launch(craft_text, body)
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


func revert_to_launch() -> bool:
	return launch(launch_text, launch_body)


func time() -> float:
	return core.time()


# ---- controls --------------------------------------------------------------

func set_throttle(x: float) -> void:
	core.vessel_set_throttle(active, x)
	_telemetry_tick = -1


func set_input(pitch: float, yaw: float, roll: float) -> void:
	core.vessel_set_input(active, pitch, yaw, roll)


func set_sas(mode: String, target := PackedFloat64Array()) -> bool:
	_telemetry_tick = -1
	return core.vessel_set_sas(active, mode, target)


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
	var reason := rails_refusal()
	if reason != "":
		return reason
	_begin_rails()
	warp_kind = "rails"
	warp_index = clampi(index, 0, RAILS_WARP.size() - 1)
	return ""


## Why on-rails warp is not allowed right now ("" if it is).
func rails_refusal() -> String:
	var t := telemetry()
	if not t.get("alive", false):
		return "no vessel"
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
		sim_event.emit(e)
	if not core.vessel_alive(active):
		_pick_new_active()


## If the active vessel is gone (crash), follow the piece with the crew,
## else the biggest piece.
func _pick_new_active() -> void:
	var vessels: Array = JSON.parse_string(core.vessel_list())
	var best := -1
	var score := -1.0
	for v in vessels:
		var s: float = v.get("crew", 0) * 1000.0 + v.get("parts", 0)
		if s > score:
			score = s
			best = v.id
	if best >= 0:
		switch_to(best)
	else:
		active = -1


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
	_begin_rails()
	var stopped := ""
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
