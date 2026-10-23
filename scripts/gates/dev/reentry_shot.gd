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
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	var pod := '{"format":"trs-craft","version":1,"name":"Capsule","description":"","parts":[{"part":"pod_1","parent":-1,"node":"","parent_node":"","pos":[0,0,0],"rot":[0,0,0,1],"stage":-1,"sym":0},{"part":"parachute","parent":0,"node":"bottom","parent_node":"top","pos":[0,0.675,0],"rot":[0,0,0,1],"stage":0,"sym":0}]}'
	Sim.pending_craft_text = pod
	var flight: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(flight)
	await tree.process_frame
	var s: FlightSession = flight.session
	# a steep return: 52 km up, 3.2 km/s, 12 degrees below the horizon
	var core := s.core
	var R := 600000.0 + 52000.0
	var ang := deg_to_rad(-12.0)
	var state := PackedFloat64Array([R, 0, 0, 3200.0 * sin(ang), 3200.0 * cos(ang), 0])
	core.vessel_remove(s.active)
	s.active = core.flight_spawn_state(pod, "floe", state)
	flight._clear_views()
	flight._cam_dist = 14.0
	flight._cam_pitch = 0.2
	s.set_sas("surface_retrograde")
	for i in 600:
		s.step_ticks(1)
		await tree.process_frame
		if flight._reentry and flight._reentry.level > 0.7:
			break
	await RenderingServer.frame_post_draw
	tree.root.get_texture().get_image().save_png("user://dev_shots/reentry.png")
	var t := s.telemetry()
	return {"status": "PASS", "log": "alt %.0f speed %.0f level %.2f" % [t.altitude, t.surface_speed, flight._reentry.level if flight._reentry else -1.0]}
