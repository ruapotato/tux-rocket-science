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
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "hopper.craft.json")
	var flight: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(flight)
	flight._cam_yaw = -0.6
	flight._cam_pitch = float(OS.get_environment("PITCH")) if OS.get_environment("PITCH") != "" else 0.02
	flight._cam_dist = float(OS.get_environment("DIST")) if OS.get_environment("DIST") != "" else 40.0
	flight._cam.fov = 12.0
	for i in 150:
		await tree.process_frame
	if OS.get_environment("NO_TERRAIN") != "":
		flight._terrain.visible = false
		for i in 5:
			await tree.process_frame
	if OS.get_environment("MAGENTA") != "":
		flight._scaled._sky_mat.set_shader_parameter("horizon", Color(1, 0, 1))
		flight._scaled._sky_mat.set_shader_parameter("zenith", Color(1, 0, 1))
		for i in 5:
			await tree.process_frame
	await RenderingServer.frame_post_draw
	tree.root.get_texture().get_image().save_png("user://dev_shots/snow_zoom.png")
	return {"status": "PASS", "log": "ok"}
