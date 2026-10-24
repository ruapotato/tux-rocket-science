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
## Dev: a capsule falling into the air under physics warp drops back to
## normal time, a step at a time, once it crosses into the atmosphere.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await tree.process_frame
	var s: FlightSession = scene.session
	var craft: Dictionary = JSON.parse_string(Sim.pending_craft_text)
	craft.parts = craft.parts.slice(0, 4)
	var t := s.telemetry()
	var up := Vector3(t.up[0], t.up[1], t.up[2])
	var east := Vector3(t.east[0], t.east[1], t.east[2])
	var p := Vector3(t.position[0], t.position[1], t.position[2]) + up * 71500.0
	var v := east * 2200.0 - up * 300.0
	s.core.vessel_remove(s.active)
	s.active = s.core.flight_spawn_state(JSON.stringify(craft), "floe", PackedFloat64Array([p.x, p.y, p.z, v.x, v.y, v.z]))
	scene._clear_views()
	scene._launched = true
	scene._was_in_air = false
	s.set_warp("physics", FlightSession.PHYSICS_WARP.size() - 1)
	var log := PackedStringArray()
	var entered_at := -1.0
	var normal_at := -1.0
	for i in 600:
		await tree.process_frame
		var tt := s.telemetry()
		if tt.in_atmosphere and entered_at < 0.0:
			entered_at = Time.get_ticks_msec() / 1000.0
			log.append("entered the air at %.0f m, warp index %d" % [tt.altitude, s.warp_index])
		if entered_at >= 0.0 and s.warp_index == 0:
			normal_at = Time.get_ticks_msec() / 1000.0
			log.append("normal time %.2f s later, at %.0f m" % [normal_at - entered_at, tt.altitude])
			break
	var ok := entered_at >= 0.0 and normal_at >= 0.0 and normal_at - entered_at < 3.0
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(log)}
