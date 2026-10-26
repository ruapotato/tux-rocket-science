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
## Dev: shots of the space program: main menu in a career, Mission
## Control, R&D, the hangar's locked parts and costs, and the flight's
## science panel.
extends RefCounted

const DIR := "user://dev_shots/"
var _log := PackedStringArray()


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	DirAccess.remove_absolute(ProjectSettings.globalize_path(Sim.save_dir + "career.json"))
	Career.load_from(Sim.save_dir + "career.json", "career")
	Career.research = 12.0
	Career.done = ["hop"]
	var menu: Control = load("res://scenes/main_menu.tscn").instantiate()
	_clear(tree)
	tree.root.add_child(menu)
	await _frames(tree, 10)
	await _shot(menu, "menu")
	menu._show_program("missions")
	await _frames(tree, 5)
	await _shot(menu, "missions")
	menu._show_program("rnd")
	await _frames(tree, 5)
	await _shot(menu, "rnd")
	menu.queue_free()
	_clear(tree)
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	tree.root.add_child(hangar)
	await _frames(tree, 5)
	hangar.model.load_text(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json"))
	hangar._frame_craft()
	await _frames(tree, 5)
	await _shot(hangar, "hangar")
	_log.append("hangar warnings: %s" % [hangar.craft_warnings().map(func(w): return w.text.left(80))])
	hangar.queue_free()
	_clear(tree)
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "hopper.craft.json")
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	scene._toggle_science()
	await _frames(tree, 10)
	await _shot(scene, "science")
	Career.load_from(Sim.save_dir + "career.json", "sandbox")
	return {"status": "PASS", "log": "\n".join(_log)}


func _clear(tree: SceneTree) -> void:
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _shot(n: Node, name: String) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png(DIR + "program_" + name + ".png")
