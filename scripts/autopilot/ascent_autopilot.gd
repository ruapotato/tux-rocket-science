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
## Scripted ascent to a circular orbit, flown only through ControlAPI
## commands and telemetry (the same interface agents use).
##   1. vertical climb, then a pitch program toward the east
##   2. stage whenever the live readout says the current stage is spent
##   3. cut throttle when the apoapsis reaches the target, coast out of the air
##   4. circularize at apoapsis until the periapsis clears the target
class_name AscentAutopilot
extends RefCounted

var api: ControlAPI
var target_ap := 84000.0
var target_pe := 78500.0
var turn_start := 250.0   # m AGL: kick over before the air gets thick, then follow prograde
var turn_end := 50000.0   # m, nose horizontal by here
var keep_stage := 1  # stages below this are never fired during ascent
var phase := "liftoff"
var log := PackedStringArray()
var max_q := 0.0
var max_heat := 0.0  # hottest part against its limit, 0..1
var hot_part := ""
var _stage_cooldown := 0.0
var _last_stage_t := -100.0


func _init(control: ControlAPI) -> void:
	api = control


func _note(t: Dictionary, msg: String) -> void:
	log.append("T+%6.1f s  alt %6.1f km  %s" % [t.t, t.altitude / 1000.0, msg])


static func v3(a: Array) -> Vector3:
	return Vector3(a[0], a[1], a[2])


func _point(t: Dictionary, pitch_from_vertical: float) -> void:
	var up := v3(t.up)
	var east := v3(t.east)
	var dir := up * cos(pitch_from_vertical) + east * sin(pitch_from_vertical)
	api.handle({"cmd": "sas", "mode": "target", "target": [dir.x, dir.y, dir.z]})


## Call once per control step; returns true when finished (or failed).
func step(dt: float) -> bool:
	var t: Dictionary = api.handle({"cmd": "query"}).telemetry
	if not t.get("alive", false):
		phase = "lost"
		return true
	max_q = maxf(max_q, t.get("dynamic_pressure", 0.0))
	if t.get("heat_frac", 0.0) > max_heat:
		max_heat = t.heat_frac
		hot_part = t.heat_part
	var orb: Dictionary = t.orbit
	var ap: float = orb.ap_alt if orb.ap_alt != null else 1e12
	_stage_cooldown -= dt
	# staging: the live readout's current stage has no delta-v left
	if phase != "liftoff" and phase != "done" and _stage_cooldown <= 0.0 and t.next_stage >= 0 and t.throttle > 0.0:
		var stages: Array = t.get("stages", [])
		var spent: bool = stages.is_empty() or stages[0].dv_vac < 0.5
		# never drop the crew pod's decoupler while climbing (keep it for reentry)
		var pod_drop: bool = t.next_stage < keep_stage
		if spent and not pod_drop:
			api.handle({"cmd": "stage"})
			_note(t, "stage -> %d" % (t.next_stage - 1))
			_stage_cooldown = 1.0
			_last_stage_t = t.t
	match phase:
		"liftoff":
			api.handle({"cmd": "throttle", "value": 1.0})
			_point(t, 0.0)
			api.handle({"cmd": "stage"})
			_note(t, "ignition, clamps released")
			phase = "climb"
		"climb":
			var h: float = t.altitude_terrain
			var p := 0.0
			if h > turn_start:
				p = clampf(pow((h - turn_start) / (turn_end - turn_start), 0.5), 0.0, 1.0) * deg_to_rad(88.0)
			# apoapsis feedback: once the apoapsis is close, build horizontal speed
			if ap > target_ap * 0.6:
				var k := clampf((ap - target_ap * 0.6) / (target_ap * 0.4), 0.0, 1.0)
				p = maxf(p, lerpf(deg_to_rad(65.0), deg_to_rad(88.0), k))
			# in thick air don't stray far from the airflow
			if t.dynamic_pressure > 4000.0 and t.surface_speed > 60.0:
				var sv := v3(t.surface_velocity).normalized()
				var up := v3(t.up)
				var pro := acos(clampf(sv.dot(up), -1.0, 1.0))
				p = clampf(p, pro - deg_to_rad(6.0), pro + deg_to_rad(6.0))
			_point(t, p)
			if ap >= target_ap:
				api.handle({"cmd": "throttle", "value": 0.0})
				api.handle({"cmd": "sas", "mode": "prograde"})
				_note(t, "MECO, apoapsis %.1f km" % (ap / 1000.0))
				phase = "coast"
		"coast":
			api.handle({"cmd": "sas", "mode": "prograde"})
			# top up the apoapsis while drag nibbles at it
			api.handle({"cmd": "throttle", "value": 0.3 if ap < target_ap - 500.0 and t.in_atmosphere else 0.0})
			if not t.in_atmosphere:
				var burn := _circ_burn_time(t)
				var falling: bool = t.vertical_speed < 0.0
				if orb.t_to_ap != null and (orb.t_to_ap < burn * 0.5 + 2.0 or falling):
					_note(t, "circularization burn (%.0f s est.)" % burn)
					phase = "circularize"
		"circularize":
			# thrust horizontally, pitching up just enough to stop sinking
			var up := v3(t.up)
			var vel := v3(t.velocity)
			var horiz := (vel - up * vel.dot(up)).normalized()
			var lift := clampf(-t.vertical_speed * 0.02 + (target_pe - t.altitude) * 2e-5, -0.15, 0.7)
			var dir := (horiz * cos(lift) + up * sin(lift)).normalized()
			api.handle({"cmd": "sas", "mode": "target", "target": [dir.x, dir.y, dir.z]})
			api.handle({"cmd": "throttle", "value": 1.0 if orb.pe_alt < target_pe - 3000.0 else 0.25})
			if orb.pe_alt >= target_pe:
				api.handle({"cmd": "throttle", "value": 0.0})
				_note(t, "orbit: Pe %.1f km, Ap %.1f km" % [orb.pe_alt / 1000.0, ap / 1000.0])
				phase = "done"
				return true
			if t.get("thrust_max", 0.0) <= 0.0 and t.next_stage <= 0:
				phase = "out_of_fuel"
				_note(t, "out of propellant, Pe %.1f km" % (orb.pe_alt / 1000.0))
				return true
	return false


## Burn time for the circularization delta-v at apoapsis.
func _circ_burn_time(t: Dictionary) -> float:
	var orb: Dictionary = t.orbit
	var mu := 3.528e12
	var R := 600000.0
	var ra: float = orb.ap_alt + R
	var a: float = orb.sma
	var v_ap := sqrt(mu * (2.0 / ra - 1.0 / a))
	var dv := sqrt(mu / ra) - v_ap
	var thrust: float = maxf(t.get("thrust_max", 0.0), 1.0)
	if thrust <= 1.0:
		var stages: Array = t.get("stages", [])
		if stages.size() > 1:
			thrust = 60000.0
	return dv * t.mass / thrust
