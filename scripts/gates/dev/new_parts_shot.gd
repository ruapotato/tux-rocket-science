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
## Dev: hangar shots of craft built from the wide parts, the tri-mount and
## the probe core.
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
	# a wide lifter: pod, upper stage, adapter, wide tank on a tri-mount
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	m.attach_stack(CraftModel.held_single("parachute"), 0, "top")
	var ut := m.attach_stack(CraftModel.held_single("tank_m"), 0, "bottom")[0]
	var ue := m.attach_stack(CraftModel.held_single("engine_vac"), ut, "bottom")[0]
	var d1 := m.attach_stack(CraftModel.held_single("decoupler_stack"), ue, "bottom")[0]
	var ad := m.attach_stack(CraftModel.held_single("adapter_21"), d1, "bottom")[0]
	var t := m.attach_stack(CraftModel.held_single("tank_xl"), ad, "bottom")[0]
	var mt := m.attach_stack(CraftModel.held_single("mount_tri"), t, "bottom")[0]
	for nn in ["bottom", "bottom_2", "bottom_3"]:
		m.attach_stack(CraftModel.held_single("engine_lifter"), mt, nn)
	m.attach_surface(CraftModel.held_single("fin"), t, PI / 3, -1.4, 3)
	await _shot(hangar, m, "wide_tri", 0.6, 0.15, log)
	await _shot(hangar, m, "wide_tri_under", 0.6, -0.7, log)
	# the same with the heavy engine
	m = CraftModel.new()
	m.place_root(CraftModel.held_single("probe_core"))
	var t2 := m.attach_stack(CraftModel.held_single("tank_xm"), 0, "bottom")
	log.append("xm tank under a probe: %s" % [t2])
	var dx := m.attach_stack(CraftModel.held_single("decoupler_xl"), t2[0], "bottom")[0]
	var t3 := m.attach_stack(CraftModel.held_single("tank_xl"), dx, "bottom")[0]
	m.attach_stack(CraftModel.held_single("engine_big"), t3, "bottom")
	await _shot(hangar, m, "probe_heavy", 0.9, 0.2, log)
	return {"status": "PASS", "log": "\n".join(log)}


func _shot(hangar: Node3D, m: CraftModel, name: String, yaw: float, pitch: float, log: PackedStringArray) -> void:
	var tree := hangar.get_tree()
	hangar.model.load_text(m.save_text())
	for i in 3:
		await tree.process_frame
	hangar._frame_craft()
	hangar._cam_yaw = yaw
	hangar._cam_pitch = pitch
	hangar._update_camera()
	for i in 6:
		await tree.process_frame
	await RenderingServer.frame_post_draw
	DirAccess.make_dir_recursive_absolute("user://dev_shots")
	hangar.get_viewport().get_texture().get_image().save_png("user://dev_shots/parts_%s.png" % name)
	log.append("saved parts_" + name)
