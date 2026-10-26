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
## Scripted flight to Kelp, blind through ControlAPI: ascent to a parking
## orbit, the planner's transfer burn at the window, out of Floe's sphere,
## a correction on the way that aims the pass into Kelp's air, then the
## capsule alone, shield first, through the thick air on its parachute.
class_name KelpMission
extends MoonMission

const KELP_PE := 50000.0  # aim the pass this far up: inside Kelp's 110 km of air, so it enters

var landed_on_kelp := false
var entry_speed := 0.0


func kelp_index() -> int:
	return api.session.core.body_find("kelp")


func node_of(r: PackedFloat64Array) -> Array:
	return [r[0], r[1], r[2], r[3]]


func fly_to_kelp() -> bool:
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
	var core := api.session.core
	var r: PackedFloat64Array = core.plan_transfer(api.session.active, kelp_index(), KELP_PE, false)
	if r.size() < 6 or r[4] < 0.5:
		note("no transfer to Kelp found")
		return false
	note("transfer planned: %.0f m/s at T+%.0f s, Kelp pass %.0f km" % [Vector3(r[1], r[2], r[3]).length(), r[0], r[5] / 1000.0])
	execute_node(node_of(r))
	# out of Floe's sphere, and a week on: the correction
	var patches: Array = api.handle({"cmd": "trajectory", "patches": 4, "horizon": 6.0e6}).patches
	var sun_t := -1.0
	for p in patches:
		if p.body == "austra":
			sun_t = float(p.t_start)
			break
	if sun_t < 0.0:
		note("the burn didn't leave Floe")
		return false
	api.handle({"cmd": "warp_to", "t": sun_t + 7.0 * 86400.0})
	_track_soi()
	r = core.plan_transfer(api.session.active, kelp_index(), KELP_PE, true)
	if r.size() < 6 or r[4] < 0.5:
		note("no correction reaches Kelp")
		return false
	note("correction planned: %.1f m/s, Kelp pass %.1f km" % [Vector3(r[1], r[2], r[3]).length(), r[5] / 1000.0])
	execute_node(node_of(r))
	# into Kelp's sphere
	patches = api.handle({"cmd": "trajectory", "patches": 4, "horizon": 6.0e6}).patches
	for p in patches:
		if p.body == "kelp":
			api.handle({"cmd": "warp_to", "t": float(p.t_start) + 5.0})
			break
	_track_soi()
	if tel().body != "kelp":
		note("missed Kelp")
		return false
	note("in Kelp's sphere: periapsis %.1f km" % (tel().orbit.pe_alt / 1000.0))
	# a last touch-up from the edge of the sphere: the burn above wasn't
	# instant, so the pass isn't quite where it was aimed
	if absf(tel().orbit.pe_alt - KELP_PE) > 5000.0:
		r = core.plan_transfer(api.session.active, kelp_index(), KELP_PE, true)
		if r.size() >= 6 and r[4] > 0.5:
			execute_node(node_of(r))
			note("touch-up: %.1f m/s, periapsis now %.1f km" % [Vector3(r[1], r[2], r[3]).length(), tel().orbit.pe_alt / 1000.0])
	return enter_kelp()


## An hour out the capsule leaves everything else behind; shield first
## through the air, chute armed low, down onto the moss.
func enter_kelp() -> bool:
	var patches: Array = api.handle({"cmd": "predict", "patches": 2, "horizon": 3.0e6}).patches
	if not patches.is_empty() and patches[0].end == "atmosphere":
		api.handle({"cmd": "warp_to", "t": float(patches[0].t_end) - 3600.0})
	api.handle({"cmd": "throttle", "value": 0.0})
	while tel().next_stage >= 0:
		api.handle({"cmd": "stage"})
	note("capsule alone: %d parts" % tel().parts)
	api.handle({"cmd": "sas", "mode": "retrograde"})
	var rr: Dictionary = api.handle({"cmd": "warp_to", "t": tel().t + 3.0e6})
	entry_speed = tel().surface_speed
	note("entry interface (%s) at %.0f m/s" % [rr.stopped, entry_speed])
	api.handle({"cmd": "sas", "mode": "surface_retrograde"})
	var chute := false
	var max_g := 0.0
	var max_heat := 0.0
	var hot := ""
	for i in 50 * 3600:
		var t := tel()
		if not t.alive:
			note("lost on the way down")
			return false
		if t.g_force > max_g and not t.landed and t.altitude_terrain > 30.0:
			max_g = t.g_force
		if t.get("heat_frac", 0.0) > max_heat:
			max_heat = t.heat_frac
			hot = t.heat_part
		if not chute and t.altitude < 40000.0 and t.vertical_speed < 0.0:
			api.handle({"cmd": "chute"})
			chute = true
			note("chute armed")
		if t.landed and t.surface_speed < 0.3:
			break
		step(5)
	var t := tel()
	peak_g = max_g
	peak_heat = max_heat
	landed_on_kelp = t.landed and t.body == "kelp" and t.get("crew_capacity", 0) > 0
	note("down on Kelp: landed %s, peak %.1f g, peak heat %.0f%% (%s), worst contact %.1f m/s, %d parts" % [t.landed, max_g, 100.0 * max_heat, hot,
		t.get("max_impact", 0.0), t.parts])
	return landed_on_kelp
