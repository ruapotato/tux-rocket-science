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
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + OS.get_environment("CRAFT") + ".craft.json") if OS.get_environment("CRAFT") != "" else FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "krill_lander.craft.json")
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	tree.root.add_child(hangar)
	Sim.pending_craft_text = ""
	await tree.process_frame
	var lo := INF
	var hi := -INF
	for v in hangar._part_nodes:
		lo = minf(lo, v.global_position.y)
		hi = maxf(hi, v.global_position.y)
	var shots := [["top", hi - 1.5, 6.0, 0.6], ["mid", (lo + hi) * 0.5, 8.0, 1.2], ["bottom", lo + 2.0, 9.0, 2.2]]
	for s in shots:
		hangar._cam_target = Vector3(0, s[1], 0)
		hangar._cam_dist = s[2]
		hangar._cam_yaw = s[3]
		hangar._cam_pitch = 0.1
		hangar._update_camera()
		for i in 6:
			await tree.process_frame
		await RenderingServer.frame_post_draw
		tree.root.get_texture().get_image().save_png("user://dev_shots/close_%s.png" % s[0])
	return {"status": "PASS", "log": "ok"}
