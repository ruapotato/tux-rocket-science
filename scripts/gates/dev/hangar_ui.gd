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
## Dev check: drives the real hangar UI path (palette -> ghost -> click).
extends RefCounted

const SHOTS := "user://dev_shots/"


func run(runner: Node) -> Dictionary:
	runner.get_tree().paused = false
	var log := PackedStringArray()
	DirAccess.make_dir_recursive_absolute(SHOTS)
	if runner.get_tree().current_scene:
		runner.get_tree().current_scene.queue_free()
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	runner.get_tree().root.add_child(hangar)
	for i in 5:
		await runner.get_tree().process_frame
	var m: CraftModel = hangar.model
	var n0 := m.parts.size()
	# hold 3x fins over the upper tank (index 4 in the orbiter)
	hangar.symmetry = 3
	hangar._on_palette("fin")
	var tank: Node3D = hangar._part_nodes[4]
	var sp: Vector2 = hangar._cam.unproject_position(tank.global_position + Vector3(0, 0.2, 0))
	hangar.get_viewport().warp_mouse(sp)
	for i in 6:
		await runner.get_tree().physics_frame
		await runner.get_tree().process_frame
	log.append("snap: %s, ghosts %d" % [hangar._snap.get("kind", "none"), hangar._ghosts.size()])
	await _shot(runner, hangar, "ghost_fins")
	hangar._click()
	await runner.get_tree().process_frame
	log.append("fins placed: %d -> %d parts" % [n0, m.parts.size()])
	var ok := m.parts.size() == n0 + 3
	# hover a part and pick it up
	hangar.get_viewport().warp_mouse(hangar._cam.unproject_position(hangar._part_nodes[6].global_position))
	for i in 4:
		await runner.get_tree().physics_frame
		await runner.get_tree().process_frame
	log.append("hover index %d" % hangar._hover)
	await _shot(runner, hangar, "hover")
	hangar.queue_free()
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(log)}


func _shot(runner: Node, n: Node, name: String) -> void:
	await RenderingServer.frame_post_draw
	var img := n.get_viewport().get_texture().get_image()
	if img:
		img.save_png(SHOTS + name + ".png")
