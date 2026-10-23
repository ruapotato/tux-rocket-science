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
## Dev: renders the hangar with a given stock craft (default lander).
extends RefCounted


func run(runner: Node) -> Dictionary:
	runner.get_tree().paused = false
	if runner.get_tree().current_scene:
		runner.get_tree().current_scene.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "krill_lander.craft.json")
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	runner.get_tree().root.add_child(hangar)
	Sim.pending_craft_text = ""
	hangar._cam_yaw = 0.9
	hangar._update_camera()
	for i in 8:
		await runner.get_tree().process_frame
	await RenderingServer.frame_post_draw
	DirAccess.make_dir_recursive_absolute("user://dev_shots")
	hangar.get_viewport().get_texture().get_image().save_png("user://dev_shots/hangar_lander.png")
	return {"status": "PASS", "log": "saved"}
