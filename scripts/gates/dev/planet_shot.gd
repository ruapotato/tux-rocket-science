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
	var text := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	Sim.pending_craft_text = text
	var flight: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(flight)
	await tree.process_frame
	var s: FlightSession = flight.session
	var body := OS.get_environment("BODY") if OS.get_environment("BODY") != "" else "floe"
	s.spawn_in_orbit(text, body, float(OS.get_environment("ALT")) if OS.get_environment("ALT") != "" else 1500000.0)
	s.core.vessel_set_mode(s.active, "rails")
	s.warp_to(s.time() + s.telemetry().orbit.period * 0.5)
	flight._clear_views()
	flight._cam_dist = 30.0
	flight._cam_pitch = 1.38
	flight._cam_yaw = 0.0
	for i in 60:
		await tree.process_frame
	await RenderingServer.frame_post_draw
	tree.root.get_texture().get_image().save_png("user://dev_shots/planet_%s.png" % body)
	return {"status": "PASS", "log": "ok"}
