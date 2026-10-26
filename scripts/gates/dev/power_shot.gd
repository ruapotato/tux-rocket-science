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
## Dev: the power and science parts in the hangar: a probe with a battery,
## four solar panels, an antenna, a thermometer and a barometer, on a lab.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	tree.root.add_child(hangar)
	for i in 5:
		await tree.process_frame
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("probe_core"))
	var lab := m.attach_stack(CraftModel.held_single("snow_lab"), 0, "bottom")[0]
	var tank := m.attach_stack(CraftModel.held_single("tank_m"), lab, "bottom")[0]
	m.attach_surface(CraftModel.held_single("solar_panel"), tank, 0.0, 0.0, 4)
	m.attach_surface(CraftModel.held_single("antenna"), lab, PI / 4, 0.0, 1)
	m.attach_surface(CraftModel.held_single("battery"), lab, PI * 0.75, 0.0, 1)
	m.attach_surface(CraftModel.held_single("thermometer"), lab, -PI / 4, 0.1, 1)
	m.attach_surface(CraftModel.held_single("barometer"), lab, -PI * 0.75, 0.1, 1)
	hangar.model.load_text(m.save_text())
	hangar._frame_craft()
	for i in 10:
		await tree.process_frame
	await RenderingServer.frame_post_draw
	hangar.get_viewport().get_texture().get_image().save_png("user://dev_shots/power_parts.png")
	hangar._cam_dist = 3.5
	hangar._cam_yaw += 0.9
	hangar._cam_pitch = 0.1
	hangar._update_camera()
	for i in 5:
		await tree.process_frame
	await RenderingServer.frame_post_draw
	hangar.get_viewport().get_texture().get_image().save_png("user://dev_shots/power_parts_close.png")
	return {"status": "PASS", "log": "summary:\n" + hangar._summary.get_parsed_text()}
