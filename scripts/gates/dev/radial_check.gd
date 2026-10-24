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
## Dev: what hanging things off radial decouplers does (orientation, staging).
extends RefCounted


func run(runner: Node) -> Dictionary:
	var log := PackedStringArray()
	for manual in [false, true]:
		var m := CraftModel.new()
		m.place_root(CraftModel.held_single("pod_1"))
		var tank := m.attach_stack(CraftModel.held_single("tank_l"), 0, "bottom")[0]
		m.attach_stack(CraftModel.held_single("engine_lifter"), tank, "bottom")
		if manual:
			m.manual_staging = true
		var dr := m.attach_surface(CraftModel.held_single("decoupler_radial"), tank, 0.0, 0.0, 2)
		for what in ["engine_lifter", "tank_m"]:
			var pl := m.stack_placements(CraftModel.held_single(what), dr[0], "outer")
			for p in pl:
				log.append("%s on outer: pos %s up %s" % [what, p.pos, DMath.q_rotate(p.rot, [0.0, 1.0, 0.0])])
		m.attach_stack(CraftModel.held_single("engine_lifter"), dr[0], "outer")
		log.append("manual=%s" % manual)
		for s in range(m.stage_count() - 1, -1, -1):
			var names := []
			for i in m.parts.size():
				if m.parts[i].stage == s:
					names.append(m.parts[i].part)
			log.append("  stage %d: %s" % [s, ", ".join(names)])
	if DisplayServer.get_name() != "headless":
		await _shots(runner, log)
	return {"status": "PASS", "log": "\n".join(log)}


func _shots(runner: Node, log: PackedStringArray) -> void:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	tree.root.add_child(hangar)
	for i in 5:
		await tree.process_frame
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	var tank := m.attach_stack(CraftModel.held_single("tank_l"), 0, "bottom")[0]
	m.attach_stack(CraftModel.held_single("engine_lifter"), tank, "bottom")
	var dr := m.attach_surface(CraftModel.held_single("decoupler_radial"), tank, PI / 2, 0.4, 2)
	var side := m.attach_stack(CraftModel.held_single("tank_m"), dr[0], "outer")
	m.attach_stack(CraftModel.held_single("engine_lifter"), side[0], "bottom")
	var dr3 := m.attach_surface(CraftModel.held_single("decoupler_radial"), tank, 0.0, 0.4, 1)
	hangar.model.load_text(m.save_text())
	m = hangar.model
	for i in 3:
		await tree.process_frame
	hangar._frame_craft()
	hangar._cam_yaw = 1.2
	hangar._cam_pitch = 0.25
	hangar._cam_dist *= 1.3
	hangar._update_camera()
	# holding a tank over the third decoupler's outer node
	hangar._held = CraftModel.held_single("srb")
	for i in 3:
		await tree.process_frame
	var nw := m.node_world(dr3[0], "outer")
	var sp: Vector2 = hangar._cam.unproject_position(hangar._craft_root.global_transform * DMath.to_v3(nw.pos))
	hangar.get_viewport().warp_mouse(sp)
	for i in 6:
		await tree.process_frame
	log.append("snap while hovering the node: %s" % [hangar._snap])
	await RenderingServer.frame_post_draw
	DirAccess.make_dir_recursive_absolute("user://dev_shots")
	hangar.get_viewport().get_texture().get_image().save_png("user://dev_shots/radial_boosters.png")
	log.append("saved radial_boosters; staging:")
	for st in range(m.stage_count() - 1, -1, -1):
		var names := []
		for i in m.parts.size():
			if m.parts[i].stage == st:
				names.append(m.parts[i].part)
		log.append("  stage %d: %s" % [st, ", ".join(names)])
