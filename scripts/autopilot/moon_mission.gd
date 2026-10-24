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
## powered landing -> liftoff -> parking orbit -> return burn (planned on
## the patched trajectory) -> capsule separation -> shield-first entry.
class_name MoonMission
extends RefCounted

const KRILL_PE := 40000.0
const KRILL_PARK := 30000.0   # parking orbit after liftoff
const FLOE_PE := 34000.0      # return periapsis: deep enough to capture, shallow enough to stay gentle
const LANDER_STAGE := 1       # stage 0 drops the capsule; nothing powered ever fires it

var api: ControlAPI
var log := PackedStringArray()
var ok := false
var landed_on_krill := false
var landed_home := false
var soi_path := PackedStringArray(["floe"])
var _nodes_burnt := 0
var peak_g := 0.0
var peak_heat := 0.0


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
func stage_if_spent(min_stage := LANDER_STAGE) -> void:
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
func burn(dir: Vector3, dv: float, min_stage := LANDER_STAGE) -> float:
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
func execute_node(node: Array, min_stage := LANDER_STAGE) -> void:
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
		stage_if_spent()
		api.handle({"cmd": "throttle", "value": 1.0})
		step(5)
	api.handle({"cmd": "throttle", "value": 0.0})
	var o: Dictionary = tel().orbit
	note("captured: %.1f x %.1f km around Krill" % [o.pe_alt / 1000.0, (o.ap_alt if o.ap_alt != null else -1.0) / 1000.0])


## Braking from orbit, then a guided vertical descent onto the surface.
func land() -> bool:
	var t := tel()
	if t.next_stage == LANDER_STAGE:
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
	# 1. clear the ground, then fly east holding a climb toward the parking
	#    altitude: no air, so nearly all the thrust can go into orbital speed
	var g: float = body_info("krill").surface_gravity
	for i in 50 * 900:
		t = tel()
		if not t.alive:
			note("lost climbing from Krill")
			return false
		var o: Dictionary = t.orbit
		if o.ap_alt != null and o.ap_alt > KRILL_PARK:
			break
		var accel: float = t.get("thrust_max", 0.0) / t.mass
		if accel <= 0.0:
			if t.next_stage < LANDER_STAGE:
				note("out of propellant climbing from Krill: Pe %.1f km, %.0f m/s" % [o.pe_alt / 1000.0, t.surface_speed])
				return false
			stage_if_spent()
			continue
		var up := v3(t.up)
		var east := v3(t.east)
		var r: float = t.altitude + body_info("krill").radius
		var vs: float = t.vertical_speed
		var vh: Vector3 = v3(t.velocity) - up * vs
		var dir := up
		if t.altitude_terrain > 250.0:
			var vs_t := clampf((KRILL_PARK - t.altitude) / 25.0, -15.0, 60.0)
			var a_up: float = g - vh.length_squared() / r + 0.8 * (vs_t - vs)
			var s_up := clampf(a_up / accel, 0.0, 0.97)
			var fwd: Vector3 = vh.normalized() if vh.length() > 20.0 else east
			dir = fwd * sqrt(1.0 - s_up * s_up) + up * s_up
		point(dir)
		api.handle({"cmd": "throttle", "value": 1.0})
		step(2)
	api.handle({"cmd": "throttle", "value": 0.0})
	# 2. coast up, then circularize: thrust horizontally until the periapsis clears the hills
	api.handle({"cmd": "sas", "mode": "prograde"})
	for i in 50 * 600:
		t = tel()
		if t.orbit.t_to_ap == null or t.orbit.t_to_ap < 15.0 or t.vertical_speed < 0.0:
			break
		step(5)
	for i in 50 * 300:
		t = tel()
		if t.orbit.pe_alt > KRILL_PARK - 8000.0 or t.get("thrust_max", 0.0) <= 0.0:
			break
		var up := v3(t.up)
		var vel := v3(t.velocity)
		var horiz := (vel - up * vel.dot(up)).normalized()
		var lift := clampf(-t.vertical_speed * 0.03, -0.2, 0.5)
		point(horiz * cos(lift) + up * sin(lift))
		api.handle({"cmd": "throttle", "value": 1.0})
		step(2)
	api.handle({"cmd": "throttle", "value": 0.0})
	var po: Dictionary = tel().orbit
	note("parking orbit: %.1f x %.1f km, %.0f m/s left" % [po.pe_alt / 1000.0, po.ap_alt / 1000.0 if po.ap_alt != null else -1.0, tel().dv_total])
	if po.pe_alt < 8000.0:
		return false
	# 3. the return burn, planned on the patched trajectory
	var node := plan_return()
	if node.is_empty():
		note("no return trajectory found")
		return false
	execute_node(node)
	for k in 200:
		if tel().body != "krill":
			break
		api.handle({"cmd": "warp_to", "t": tel().t + 1800.0})
		_track_soi()
	return tel().body == "floe"


## Floe periapsis of the first Floe patch after `node`, or null when the
## trajectory never gets home.
func floe_pe(node: Array):
	var patches: Array = api.handle({"cmd": "trajectory", "nodes": [node], "patches": 6, "horizon": 6.0e5}).patches
	for p in patches:
		if p.body == "floe" and p.t_start >= float(node[0]) - 1e-6:
			return p.pe_alt
	return null


func _return_score(node: Array) -> float:
	var pe = floe_pe(node)
	return 1e12 if pe == null else absf(pe - FLOE_PE)


## A prograde burn from the parking orbit that leaves Krill backwards along
## its orbit, so the capsule falls straight into Floe's air.
func plan_return() -> Array:
	var t := tel()
	var krill := body_info("krill")
	var floe := body_info("floe")
	# patched-conic estimate: the excess speed that drops a Krill-distance
	# orbit's periapsis to the target, then the burn that leaves with it
	var rk: float = krill.sma
	var rp: float = floe.radius + FLOE_PE
	var v_ap := sqrt(floe.mu * (2.0 / rk - 2.0 / (rk + rp)))
	var v_inf := sqrt(floe.mu / rk) - v_ap
	var r0: float = t.orbit.sma
	var dv := sqrt(v_inf * v_inf + 2.0 * krill.mu / r0) - sqrt(krill.mu / r0)
	var period: float = t.orbit.period
	var best := []
	var best_s := INF
	for k in 120:
		var node := [t.t + 120.0 + k * period / 120.0, dv, 0.0, 0.0]
		var s := _return_score(node)
		if s < best_s:
			best_s = s
			best = node
	if best.is_empty() or best_s >= 1e12:
		return []
	var steps := [period / 120.0, 5.0]
	for it in 60:
		var improved := false
		for dim in [0, 1]:
			for sgn in [-1.0, 1.0]:
				var cand: Array = best.duplicate()
				cand[dim] += sgn * steps[dim]
				var s := _return_score(cand)
				if s < best_s:
					best_s = s
					best = cand
					improved = true
		if not improved:
			steps[0] *= 0.5
			steps[1] *= 0.5
		if best_s < 500.0:
			break
	note("return planned: %.1f m/s prograde in %.0f s (estimate %.0f), Floe periapsis %.1f km" % [best[1], best[0] - t.t, dv, floe_pe(best) / 1000.0])
	return best


## Trims the Floe periapsis with a small burn, if the return drifted.
func return_correct() -> void:
	var t := tel()
	var base := [t.t + 600.0, 0.0, 0.0, 0.0]
	var best := base
	var best_s := _return_score(base)
	if best_s < 1500.0:
		note("no return correction needed (Floe Pe %.1f km)" % (floe_pe(base) / 1000.0))
		return
	var step_v := 2.0
	for it in 80:
		var improved := false
		for dim in [1, 2, 3]:
			for sgn in [-1.0, 1.0]:
				var cand: Array = best.duplicate()
				cand[dim] += sgn * step_v
				var s := _return_score(cand)
				if s < best_s:
					best_s = s
					best = cand
					improved = true
		if not improved:
			step_v *= 0.5
			if step_v < 0.02:
				break
	note("return correction: [%.2f, %.2f, %.2f] m/s -> Floe Pe %.1f km" % [best[1], best[2], best[3], floe_pe(best) / 1000.0])
	if absf(best[1]) + absf(best[2]) + absf(best[3]) > 0.02:
		execute_node(best)


## Trim the periapsis, drop the lander, come in shield first, chute down.
func return_home() -> bool:
	return_correct()
	note("on the way home: Floe periapsis %.1f km, %.0f m/s left" % [tel().orbit.pe_alt / 1000.0, tel().dv_total])
	if tel().orbit.pe_alt > 65000.0 or tel().orbit.pe_alt < 0.0:
		return false
	# coast to an hour out, separate the capsule, then on to the air
	var patches: Array = api.handle({"cmd": "predict", "patches": 2, "horizon": 3.0e6}).patches
	if not patches.is_empty() and patches[0].end == "atmosphere":
		api.handle({"cmd": "warp_to", "t": float(patches[0].t_end) - 3600.0})
	api.handle({"cmd": "throttle", "value": 0.0})
	api.handle({"cmd": "stage"})
	note("capsule separated: %d parts" % tel().parts)
	api.handle({"cmd": "sas", "mode": "retrograde"})
	var r: Dictionary = api.handle({"cmd": "warp_to", "t": tel().t + 3.0e6})
	note("entry interface (%s) at %.0f m/s" % [r.stopped, tel().surface_speed])
	api.handle({"cmd": "sas", "mode": "surface_retrograde"})
	var chute_armed := false
	var max_g := 0.0
	var max_heat := 0.0
	var hot_part := ""
	for i in 50 * 3600:
		var t := tel()
		if not t.alive:
			return false
		max_g = maxf(max_g, t.g_force)
		if t.get("heat_frac", 0.0) > max_heat:
			max_heat = t.heat_frac
			hot_part = t.heat_part
		if not chute_armed and t.altitude < 30000.0 and t.vertical_speed < 0.0:
			api.handle({"cmd": "chute"})  # arms the chute; it opens once slow enough
			chute_armed = true
			note("chute armed")
		if t.landed and t.surface_speed < 0.3:
			break
		if OS.get_environment("TRS_TRACE") != "" and i % 20 == 0:
			note("  %.0f m/s, %.1f g, %d parts, heat %.0f%% %s" % [t.surface_speed, t.g_force, t.parts, 100.0 * t.get("heat_frac", 0.0), t.get("heat_part", "")])
		step(5)
	var t := tel()
	landed_home = t.landed and t.body == "floe" and t.get("crew_capacity", 0) > 0
	peak_g = max_g
	peak_heat = max_heat
	note("splashdown on the ice: landed %s, peak %.1f g, peak heat %.0f%% (%s), worst contact %.1f m/s, %d parts" % [
		t.landed, max_g, 100.0 * max_heat, hot_part, t.get("max_impact", 0.0), t.parts])
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
