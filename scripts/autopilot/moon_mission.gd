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
## Scripted Krill mission, flown blind through ControlAPI only:
## ascent -> transfer burn (maneuver node) -> SOI change -> capture ->
## powered landing -> liftoff -> escape back into Floe's SOI.
class_name MoonMission
extends RefCounted

const KRILL_PE := 40000.0

var api: ControlAPI
var log := PackedStringArray()
var ok := false
var landed_on_krill := false
var landed_home := false
var soi_path := PackedStringArray(["floe"])
var _nodes_burnt := 0


func _init(control: ControlAPI) -> void:
	api = control


func tel() -> Dictionary:
	return api.handle({"cmd": "query"}).telemetry


func step(n := 5) -> void:
	api.handle({"cmd": "step", "ticks": n})
	_track_soi()


func _track_soi() -> void:
	var b: String = tel().get("body", "")
	if b != "" and soi_path[soi_path.size() - 1] != b:
		soi_path.append(b)
		note("entered the SOI of %s" % b.capitalize())


func note(msg: String) -> void:
	var t := tel()
	log.append("T+%8.0f s  %-6s alt %8.1f km  %s" % [t.get("t", 0.0), t.get("body", "?"), t.get("altitude", 0.0) / 1000.0, msg])


static func v3(a: Array) -> Vector3:
	return Vector3(a[0], a[1], a[2])


func point(dir: Vector3) -> void:
	var d := dir.normalized()
	api.handle({"cmd": "sas", "mode": "target", "target": [d.x, d.y, d.z]})


func aligned(dir: Vector3, deg := 3.0) -> bool:
	return v3(tel().nose).angle_to(dir) < deg_to_rad(deg)


## Stage while the current stage is spent, never past `min_stage`.
func stage_if_spent(min_stage := 0) -> void:
	var t := tel()
	if t.next_stage >= min_stage and t.get("thrust_max", 0.0) <= 0.0:
		api.handle({"cmd": "stage"})
		note("stage -> %d" % (int(t.next_stage) - 1))


func body_info(id: String) -> Dictionary:
	var core := api.session.core
	return JSON.parse_string(core.body_info(core.body_find(id)))


# ---- burns ------------------------------------------------------------------------

## Burns `dv` m/s along fixed direction `dir` (sim frame) and returns what
## was delivered (from trs_core's integrated thrust acceleration).
func burn(dir: Vector3, dv: float, min_stage := 0) -> float:
	point(dir)
	for i in 50 * 40:
		if aligned(dir):
			break
		step(5)
	var start: float = tel().dv_expended
	for i in 50 * 900:
		var t := tel()
		var done: float = t.dv_expended - start
		var remaining := dv - done
		if remaining <= 0.02:
			break
		if t.get("thrust_max", 0.0) <= 0.0:
			if t.next_stage < min_stage:
				break
			stage_if_spent(min_stage)
			continue
		var accel: float = t.thrust_max / t.mass
		api.handle({"cmd": "throttle", "value": clampf(remaining / (accel * 1.5), 0.02, 1.0)})
		point(dir)
		step(1 if remaining < accel * 0.5 else 5)
	api.handle({"cmd": "throttle", "value": 0.0})
	return tel().dv_expended - start


## Executes maneuver node [t, pro, nrm, rad]: warp, point, burn centred on t.
func execute_node(node: Array, min_stage := 0) -> void:
	var dir := v3(api.handle({"cmd": "node_vector", "nodes": [node], "index": 0}).vector)
	var dv := dir.length()
	var bt: float = api.handle({"cmd": "burn_time", "dv": dv}).seconds
	if bt < 0:
		stage_if_spent(min_stage)
		bt = api.handle({"cmd": "burn_time", "dv": dv}).seconds
	var start := float(node[0]) - bt * 0.5
	api.handle({"cmd": "warp_to", "t": start - 45.0})
	point(dir)
	while tel().t < start:
		step(5)
	var got := burn(dir, dv, min_stage)
	_nodes_burnt += 1
	note("node %d: %.1f m/s planned, %.1f delivered (%.0f s burn)" % [_nodes_burnt, dv, got, bt])


# ---- transfer ------------------------------------------------------------------------

## The Krill patch of a trajectory with one node, or {}.
func krill_patch(node: Array) -> Dictionary:
	var patches: Array = api.handle({"cmd": "trajectory", "nodes": [node], "patches": 6, "horizon": 6.0e5}).patches
	for p in patches:
		if p.body == "krill":
			return p
	return {}


func _score(p: Dictionary) -> float:
	if p.is_empty():
		return 1e12
	if p.pe_alt < 8000.0:
		return 1e9 + absf(p.pe_alt - KRILL_PE)
	return absf(p.pe_alt - KRILL_PE)


func plan_transfer() -> Array:
	var t := tel()
	var floe := body_info("floe")
	var krill := body_info("krill")
	var mu: float = floe.mu
	var r1: float = t.orbit.sma
	var r2: float = krill.sma
	var at := (r1 + r2) * 0.5
	var dv := sqrt(mu * (2.0 / r1 - 1.0 / at)) - sqrt(mu / r1)
	var period: float = t.orbit.period
	var best := []
	var best_s := INF
	for k in 240:
		var node := [t.t + 300.0 + k * period / 240.0, dv + 25.0, 0.0, 0.0]
		var s := _score(krill_patch(node))
		if s < best_s:
			best_s = s
			best = node
	if best.is_empty() or best_s >= 1e12:
		return []
	# refine: coordinate search on time and prograde dv
	var steps := [period / 240.0, 4.0]
	for it in 40:
		var improved := false
		for dim in [0, 1]:
			for sgn in [-1.0, 1.0]:
				var cand: Array = best.duplicate()
				cand[dim] += sgn * steps[dim]
				var s := _score(krill_patch(cand))
				if s < best_s:
					best_s = s
					best = cand
					improved = true
		if not improved:
			steps[0] *= 0.5
			steps[1] *= 0.5
		if best_s < 1500.0:
			break
	var kp := krill_patch(best)
	note("transfer planned: %.1f m/s prograde in %.0f s, Krill periapsis %.1f km" % [best[1], best[0] - t.t, kp.get("pe_alt", 0.0) / 1000.0])
	return best


## Small correction a while after the transfer burn, if the encounter drifted.
func course_correct() -> void:
	var t := tel()
	var node_t: float = t.t + 900.0
	var base := [node_t, 0.0, 0.0, 0.0]
	var best := base
	var best_s := _score(krill_patch(base))
	if best_s < 5000.0:
		note("no course correction needed (Krill Pe %.1f km)" % (krill_patch(base).get("pe_alt", 0.0) / 1000.0))
		return
	var step_v := 2.0
	for it in 60:
		var improved := false
		for dim in [1, 2, 3]:
			for sgn in [-1.0, 1.0]:
				var cand: Array = best.duplicate()
				cand[dim] += sgn * step_v
				var s := _score(krill_patch(cand))
				if s < best_s:
					best_s = s
					best = cand
					improved = true
		if not improved:
			step_v *= 0.5
			if step_v < 0.05:
				break
	note("course correction: [%.2f, %.2f, %.2f] m/s -> Krill Pe %.1f km" % [best[1], best[2], best[3], krill_patch(best).get("pe_alt", 0.0) / 1000.0])
	if absf(best[1]) + absf(best[2]) + absf(best[3]) > 0.05:
		execute_node(best)


# ---- Krill ------------------------------------------------------------------------------

func warp_into_krill() -> bool:
	var patches: Array = api.handle({"cmd": "trajectory", "patches": 6, "horizon": 6.0e5}).patches
	for p in patches:
		if p.body == "krill":
			api.handle({"cmd": "warp_to", "t": p.t_start + 5.0})
			_track_soi()
			return tel().body == "krill"
	return false


func capture() -> void:
	var t := tel()
	var krill := body_info("krill")
	var mu: float = krill.mu
	var rp: float = t.orbit.pe_alt + krill.radius
	var v_pe := sqrt(mu * (2.0 / rp - 1.0 / t.orbit.sma))
	var dv := v_pe - sqrt(mu / rp)
	var bt: float = api.handle({"cmd": "burn_time", "dv": dv}).seconds
	if bt < 0:
		bt = 60.0
	api.handle({"cmd": "warp_to", "t": t.t + t.orbit.t_to_pe - bt * 0.5 - 40.0})
	api.handle({"cmd": "sas", "mode": "retrograde"})
	for i in 50 * 40:
		var tt := tel()
		if tt.orbit.t_to_pe < bt * 0.5 or tt.orbit.t_to_pe > tt.orbit.get("period", 1e9) * 0.5 and tt.orbit.period > 0:
			break
		step(5)
	for i in 50 * 600:
		var tt := tel()
		if tt.orbit.ap_alt != null and tt.orbit.ap_alt < tt.orbit.pe_alt + 25000.0:
			break
		stage_if_spent(0)
		api.handle({"cmd": "throttle", "value": 1.0})
		step(5)
	api.handle({"cmd": "throttle", "value": 0.0})
	var o: Dictionary = tel().orbit
	note("captured: %.1f x %.1f km around Krill" % [o.pe_alt / 1000.0, (o.ap_alt if o.ap_alt != null else -1.0) / 1000.0])


## Braking from orbit, then a guided vertical descent onto the surface.
func land() -> bool:
	var t := tel()
	if t.next_stage == 0:
		api.handle({"cmd": "stage"})
		note("lander separated from the transfer stage")
	api.handle({"cmd": "legs", "deployed": true})
	var g: float = body_info("krill").surface_gravity
	# 1. kill most of the orbital (surface) speed
	api.handle({"cmd": "sas", "mode": "surface_retrograde"})
	for i in 50 * 30:
		if aligned(-v3(tel().surface_velocity).normalized(), 5.0):
			break
		step(5)
	for i in 50 * 600:
		t = tel()
		if t.horizontal_speed < 40.0 or t.altitude_terrain < 6000.0:
			break
		api.handle({"cmd": "throttle", "value": 1.0})
		step(5)
	api.handle({"cmd": "throttle", "value": 0.0})
	note("braked: %.0f m/s horizontal, %.0f m/s vertical" % [t.horizontal_speed, t.vertical_speed])
	# 2. guided descent: vertical speed schedule on altitude above terrain
	for i in 50 * 900:
		t = tel()
		if t.landed and t.surface_speed < 0.3:
			break
		var agl: float = t.altitude_terrain
		var accel: float = t.get("thrust_max", 0.0) / t.mass
		if accel <= 0.0:
			return false
		var brake := maxf(accel - g, 0.5) * 0.55
		var vt := -minf(1.6 + sqrt(2.0 * brake * maxf(agl - 4.0, 0.0)), 250.0)
		var up := v3(t.up)
		var vs: float = t.vertical_speed
		var vh := v3(t.surface_velocity) - up * vs
		# vertical: acceleration needed to track the schedule (gravity is free)
		var a_up := g + 1.2 * (vt - vs)
		# horizontal: null drift, bounded so it never dominates
		var a_h := -vh * 0.6
		if a_h.length() > maxf(a_up, 0.0) * tan(deg_to_rad(35.0)):
			a_h = a_h.normalized() * maxf(a_up, 0.0) * tan(deg_to_rad(35.0))
		var thr := 0.0
		if a_up > 0.0:
			var want := up * a_up + a_h
			point(want)
			thr = clampf(want.length() / accel, 0.0, 1.0)
		else:
			point(up)  # coasting down: keep the engine ready, pointing up
		if agl < 2.0 and absf(vs) < 1.5:
			thr = 0.0
		api.handle({"cmd": "throttle", "value": thr})
		step(1 if agl < 60.0 else 5)
	api.handle({"cmd": "throttle", "value": 0.0})
	api.handle({"cmd": "sas", "mode": "stability"})
	# settle: legs spring, maybe a little slide on a slope; wait for rest
	for i in 150:
		step(5)
		t = tel()
		if t.landed and t.surface_speed < 0.3 and i > 30:
			break
	t = tel()
	var upright: bool = t.pitch_deg > 60.0
	landed_on_krill = t.landed and t.body == "krill" and upright and t.get("crew_capacity", 0) > 0
	note("touchdown: landed %s, upright %s (pitch %.0f deg), worst contact %.1f m/s, %d parts, %.0f m/s left" % [
		t.landed, upright, t.pitch_deg, t.get("max_impact", 0.0), t.parts, t.dv_total])
	return landed_on_krill


func leave_krill() -> bool:
	var t := tel()
	note("liftoff from Krill")
	for i in 50 * 600:
		t = tel()
		if not t.alive:
			return false
		var o: Dictionary = t.orbit
		if o.ap_alt == null or o.ap_alt > 2.6e6 or o.ecc >= 1.0:
			break
		var agl: float = t.altitude_terrain
		var up := v3(t.up)
		var east := v3(t.east)
		var th := clampf((agl - 400.0) / 6000.0, 0.0, 1.0) * deg_to_rad(84.0)
		if t.vertical_speed < 5.0 and agl < 3000.0:
			th = minf(th, deg_to_rad(45.0))
		point(up * cos(th) + east * sin(th))
		stage_if_spent(0)
		api.handle({"cmd": "throttle", "value": 1.0})
		step(5)
	api.handle({"cmd": "throttle", "value": 0.0})
	note("escape trajectory: e %.3f" % tel().orbit.ecc)
	for k in 200:
		if tel().body != "krill":
			break
		api.handle({"cmd": "warp_to", "t": tel().t + 1800.0})
		_track_soi()
	return tel().body == "floe"


## Drop the Floe periapsis into the air, reenter, chute down.
func return_home() -> bool:
	var t := tel()
	# burn retrograde (relative to Floe) near apoapsis until the periapsis dips in
	if t.orbit.t_to_ap != null and t.orbit.t_to_ap > 600.0:
		api.handle({"cmd": "warp_to", "t": t.t + t.orbit.t_to_ap - 120.0})
	api.handle({"cmd": "sas", "mode": "retrograde"})
	for i in 50 * 40:
		if aligned(-v3(tel().velocity).normalized(), 3.0):
			break
		step(5)
	for i in 50 * 900:
		t = tel()
		if t.orbit.pe_alt < 32000.0 or t.get("thrust_max", 0.0) <= 0.0:
			break
		api.handle({"cmd": "throttle", "value": 1.0 if t.orbit.pe_alt > 60000.0 else 0.3})
		step(5)
	api.handle({"cmd": "throttle", "value": 0.0})
	note("deorbit: Floe periapsis %.1f km, %.0f m/s left" % [tel().orbit.pe_alt / 1000.0, tel().dv_total])
	if tel().orbit.pe_alt > 65000.0:
		return false
	var r: Dictionary = api.handle({"cmd": "warp_to", "t": tel().t + 3.0e6})
	note("entry interface (%s)" % r.stopped)
	api.handle({"cmd": "sas", "mode": "surface_retrograde"})
	var chute_armed := false
	var max_g := 0.0
	for i in 50 * 3600:
		t = tel()
		if not t.alive:
			return false
		max_g = maxf(max_g, t.g_force)
		if not chute_armed and t.altitude < 30000.0:
			api.handle({"cmd": "chute"})  # arms the chute; it opens once slow enough
			chute_armed = true
			note("chute armed")
		if t.landed and t.surface_speed < 0.3:
			break
		step(5)
	t = tel()
	landed_home = t.landed and t.body == "floe" and t.get("crew_capacity", 0) > 0
	note("splashdown on the ice: landed %s, peak %.1f g, worst contact %.1f m/s, %d parts" % [
		t.landed, max_g, t.get("max_impact", 0.0), t.parts])
	return landed_home


# ---- the whole mission ------------------------------------------------------------------

func fly() -> bool:
	note("launch")
	var ascent := AscentAutopilot.new(api)
	var guard := 0
	var done := false
	while not done and guard < 50 * 60 * 30:
		done = ascent.step(0.1)
		var tt := tel()
		if ascent.phase == "coast" and not tt.in_atmosphere and tt.orbit.t_to_ap != null and tt.orbit.t_to_ap > 60.0:
			api.handle({"cmd": "warp_to", "t": tt.t + tt.orbit.t_to_ap - 50.0})
			continue
		step(5)
		guard += 5
	for line in ascent.log:
		log.append("    ascent: " + line)
	if ascent.phase != "done":
		note("ascent failed: " + ascent.phase)
		return false
	var node := plan_transfer()
	if node.is_empty():
		note("no Krill encounter found")
		return false
	execute_node(node)
	course_correct()
	if not warp_into_krill():
		note("missed Krill")
		return false
	capture()
	if not land():
		note("landing failed")
		return false
	if not leave_krill():
		note("did not get back to Floe")
		return false
	var t := tel()
	note("home SOI reached: Floe periapsis %.0f km, %.0f m/s left" % [t.orbit.pe_alt / 1000.0, t.dv_total])
	ok = landed_on_krill and soi_path.has("krill") and soi_path[soi_path.size() - 1] == "floe"
	return_home()
	return ok
