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
## Dev: the hangar's warning for a tank hung under a solid booster.
extends RefCounted

const DIR := "user://dev_shots/"


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	m.attach_stack(CraftModel.held_single("parachute"), 0, "top")
	var t := m.attach_stack(CraftModel.held_single("tank_m"), 0, "bottom")[0]
	var sb := m.attach_stack(CraftModel.held_single("srb"), t, "bottom")[0]
	var t2 := m.attach_stack(CraftModel.held_single("tank_s"), sb, "bottom")[0]
	m.attach_surface(CraftModel.held_single("fin"), sb, 0.0, -0.8, 3)
	Sim.pending_craft_text = m.save_text()
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	tree.root.add_child(hangar)
	for i in 5:
		await tree.process_frame
	hangar.model.load_text(m.save_text())
	hangar._frame_craft()
	for i in 8:
		await tree.process_frame
	var ws: Array = hangar.craft_warnings()
	var log := PackedStringArray()
	for w in ws:
		log.append(("ERR  " if w.bad else "tip  ") + w.text)
	if DisplayServer.get_name() != "headless":
		await RenderingServer.frame_post_draw
		hangar.get_viewport().get_texture().get_image().save_png(DIR + "exhaust_warning.png")
		log.append("saved exhaust_warning.png")
	var ok := false
	for w in ws:
		ok = ok or w.text.begins_with("HOT EXHAUST")
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(log)}
