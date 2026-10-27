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
## Dev: hangar shots of a ship built from the habitation parts: habs, a
## junction with a side hab, a spine, the spin ring. Env CRAFT=file shoots
## a saved craft instead.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	tree.root.add_child(hangar)
	for i in 5:
		await tree.process_frame
	var log := PackedStringArray()
	var m := CraftModel.new()
	if OS.get_environment("CRAFT") != "":
		m.load_text(FileAccess.get_file_as_string(OS.get_environment("CRAFT")))
	else:
		m.place_root(CraftModel.held_single("pod_3"))
		var ad := m.attach_stack(CraftModel.held_single("adapter_21"), 0, "bottom")[0]
		var h1 := m.attach_stack(CraftModel.held_single("hab_burrow"), ad, "bottom")[0]
		var j := m.attach_stack(CraftModel.held_single("junction"), h1, "bottom")[0]
		log.append("side hab: %s" % [m.attach_stack(CraftModel.held_single("hab_burrow"), j, "east")])
		var ring := m.attach_stack(CraftModel.held_single("spin_ring"), j, "bottom")[0]
		var w := m.attach_stack(CraftModel.held_single("hab_rookery"), ring, "bottom")[0]
		var sp := m.attach_stack(CraftModel.held_single("spine"), w, "bottom")[0]
		var tr := m.attach_stack(CraftModel.held_single("truss"), sp, "bottom")[0]
		m.attach_stack(CraftModel.held_single("engine_big"), tr, "bottom")
		log.append("parts: %d" % m.parts.size())
	await _shot(hangar, m, "ship", 0.7, 0.25, log, 1.0)
	await _shot(hangar, m, "ship_close", 0.5, 0.1, log, 0.35)
	return {"status": "PASS", "log": "\n".join(log)}


func _shot(hangar: Node3D, m: CraftModel, name: String, yaw: float, pitch: float, log: PackedStringArray, zoom: float) -> void:
	var tree := hangar.get_tree()
	hangar.model.load_text(m.save_text())
	for i in 3:
		await tree.process_frame
	hangar._frame_craft()
	hangar._cam_yaw = yaw
	hangar._cam_pitch = pitch
	hangar._cam_dist *= zoom
	hangar._update_camera()
	for i in 6:
		await tree.process_frame
	await RenderingServer.frame_post_draw
	DirAccess.make_dir_recursive_absolute("user://dev_shots")
	hangar.get_viewport().get_texture().get_image().save_png("user://dev_shots/big_%s.png" % name)
	log.append("saved big_" + name)
