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
	var layer := CanvasLayer.new()
	tree.root.add_child(layer)
	var bg := ColorRect.new()
	bg.color = Color(0.06, 0.08, 0.12)
	bg.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	layer.add_child(bg)
	var grid := GridContainer.new()
	grid.columns = 4
	grid.position = Vector2(20, 20)
	grid.add_theme_constant_override("h_separation", 16)
	grid.add_theme_constant_override("v_separation", 16)
	layer.add_child(grid)
	var cases := [
		["calm", {"g_force": 1.0, "landed": true, "altitude": 70.0, "atmosphere_height": 70000.0}, 0.0, ""],
		["thrilled", {"g_force": 2.0, "thrust": 1.0, "surface_speed": 400.0, "altitude": 9000.0, "atmosphere_height": 70000.0}, 0.0, ""],
		["squished", {"g_force": 5.5, "thrust": 1.0, "altitude": 30000.0, "atmosphere_height": 70000.0}, 0.0, ""],
		["panicking", {"g_force": 1.0, "vertical_speed": -120.0, "altitude_terrain": 800.0, "altitude": 1200.0, "atmosphere_height": 70000.0}, 0.0, ""],
		["dizzy", {"g_force": 1.2, "altitude": 20000.0, "atmosphere_height": 70000.0}, 3.0, ""],
		["weightless", {"g_force": 0.0, "altitude": 90000.0, "atmosphere_height": 70000.0}, 0.0, ""],
		["cheering", {"g_force": 1.0, "landed": true, "altitude": 100.0}, 0.0, "landed"],
		["boom", {"g_force": 1.0, "altitude": 1000.0, "atmosphere_height": 70000.0}, 0.0, "boom"],
	]
	var ps := []
	for k in cases.size():
		var p := PenguinPortrait.new()
		p.custom_minimum_size = Vector2(200, 220)
		p.set_member(Crew.crew[k % Crew.crew.size()])
		grid.add_child(p)
		ps.append(p)
	for f in 90:
		for k in cases.size():
			if f == 0 and cases[k][3] != "":
				ps[k].react(cases[k][3], 10.0)
			ps[k].update_state(cases[k][1], cases[k][2], 1.0 / 60.0)
		await tree.process_frame
	await RenderingServer.frame_post_draw
	tree.root.get_texture().get_image().save_png("user://dev_shots/penguins.png")
	var names := PackedStringArray()
	for c in Crew.crew:
		names.append("%s (%s, nerve %.2f, glee %.2f)" % [c.name, c.accessory, c.nerve, c.glee])
	return {"status": "PASS", "log": "\n".join(names)}
