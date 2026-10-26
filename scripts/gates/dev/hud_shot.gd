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
## Dev: the flight screen as it first opens (default camera, HUD layout)
## for several stock craft, and again after the first stage is spent.
## CRAFT=name for one craft.
extends RefCounted

const DIR := "user://dev_shots/"
var _log := PackedStringArray()


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var names := ["rookery_heavy", "sidecar", "orbiter", "hopper", "krill_lander"]
	if OS.get_environment("CRAFT") != "":
		names = [OS.get_environment("CRAFT")]
	for n in names:
		for c in tree.root.get_children():
			if c is Node3D:
				c.queue_free()
		if tree.current_scene:
			tree.current_scene.queue_free()
		Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + n + ".craft.json")
		var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
		tree.root.add_child(scene)
		await _frames(tree, 5)
		scene.hud.hide_help()
		await _frames(tree, 30)
		await _shot(scene, n + "_pad")
		_log.append("%s: height %.1f m, camera %.1f m\n%s" % [n, scene._craft_height(), scene._cam_dist, scene.hud._stages.get_parsed_text()])
		scene._throttle = 1.0
		scene._stage()
		scene.session.step_ticks(50 * 8)
		await _frames(tree, 10)
		await _shot(scene, n + "_climb")
	return {"status": "PASS", "log": "\n".join(_log)}


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _shot(n: Node, name: String) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png(DIR + "hud_" + name + ".png")
	_log.append("saved hud_" + name)
