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
## Dev: builds the stock Skua (a single-stage spaceship on alien tech) and
## writes data/craft/skua.craft.json; with a screen, shoots it in the
## hangar. Nose to tail: a Glasswing prow, a box hab for four, a square
## tank, the Hearth Core and the Hearth Torch; mirrored wings, four legs.
extends RefCounted


static func skua() -> CraftModel:
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("hab_box"))
	m.set_size(0, [1.0, 1.0, 1.4])
	var prow := CraftModel.held_single("glasswing_wedge")
	prow.parts[0]["size"] = [1.0, 0.9, 1.4]
	m.attach_stack(prow, 0, "top")
	var tank := CraftModel.held_single("tank_box")
	tank.parts[0]["size"] = [1.0, 1.1, 1.4]
	var t := m.attach_stack(tank, 0, "bottom")[0]
	var core := m.attach_stack(CraftModel.held_single("hearth_core"), t, "bottom")[0]
	m.attach_stack(CraftModel.held_single("hearth_torch"), core, "bottom")
	var wing := CraftModel.held_single("wing")
	wing.parts[0]["size"] = [1.0, 1.2, 1.3]
	m.attach_surface(wing, t, PI / 2, -0.8, CraftModel.MIRROR)
	m.attach_surface(CraftModel.held_single("landing_leg"), t, PI / 4, -1.2, 4)
	m.craft_name = "Skua"
	m.description = "A spaceship on alien tech: one stage from the pad to orbit, to Krill and back, and on to the other worlds. The Hearth Torch's thrust is weight in the cabin (Enter to walk about). Fly it like a rocket: Z, Space, and lean east."
	return m


func run(runner: Node) -> Dictionary:
	var m := skua()
	var text := m.save_text()
	var path := ProjectSettings.globalize_path("res://data/craft/skua.craft.json")
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_string(text)
	f.close()
	var st: Dictionary = m.stats()
	var log := "wrote %s: %d parts, %.2f t, stages %d, %s" % [path, m.parts.size(), st.mass / 1000.0, (st.stages as Array).size(),
		str((st.stages as Array).map(func(x): return "dv %.0f twr %.2f" % [x.dv_vac, x.twr_sl]))]
	if DisplayServer.get_name() != "headless":
		var tree := runner.get_tree()
		tree.paused = false
		if tree.current_scene:
			tree.current_scene.queue_free()
		var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
		tree.root.add_child(hangar)
		for i in 5:
			await tree.process_frame
		for v in [[0.7, 0.25, "skua"], [2.4, 0.1, "skua_back"]]:
			hangar.model.load_text(text)
			for i in 3:
				await tree.process_frame
			hangar._frame_craft()
			hangar._cam_yaw = v[0]
			hangar._cam_pitch = v[1]
			hangar._update_camera()
			for i in 6:
				await tree.process_frame
			await RenderingServer.frame_post_draw
			DirAccess.make_dir_recursive_absolute("user://dev_shots")
			hangar.get_viewport().get_texture().get_image().save_png("user://dev_shots/%s.png" % v[2])
	return {"status": "PASS", "log": log}
