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
## Dev: drives the real UI with input events, like a player would.
extends RefCounted


func _key(tree: SceneTree, code: Key, shift := false) -> void:
	for pressed in [true, false]:
		var e := InputEventKey.new()
		e.keycode = code
		e.physical_keycode = code
		e.pressed = pressed
		e.shift_pressed = shift
		Input.parse_input_event(e)
		await tree.process_frame


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	tree.change_scene_to_file("res://scenes/main_menu.tscn")
	await _frames(tree, 10)
	log.append("menu: %s" % tree.current_scene.name)
	tree.current_scene._hangar()
	await _frames(tree, 15)
	var hangar = tree.current_scene
	log.append("hangar: %s, craft %s, crew button '%s'" % [hangar.name, hangar.model.craft_name, hangar._crew_button.text])
	await _key(tree, KEY_X)  # symmetry
	hangar._on_launch()
	await _frames(tree, 20)
	var flight = tree.current_scene
	log.append("flight: %s, crew %s" % [flight.name, flight.crew_names])
	await _key(tree, KEY_Z)
	await _key(tree, KEY_SPACE)
	await _frames(tree, 60)
	await _key(tree, KEY_PERIOD)  # physics warp (in atmosphere)
	await _frames(tree, 120)
	var t: Dictionary = flight.session.telemetry()
	log.append("after liftoff: %s alt %.0f m, speed %.0f m/s, warp %s %d, mood %s" % [t.situation, t.altitude, t.surface_speed,
		flight.session.warp_kind, flight.session.warp_index, flight._portraits[0].mood if not flight._portraits.is_empty() else "-"])
	await _key(tree, KEY_M)
	await _frames(tree, 20)
	log.append("map visible %s, patches %d" % [flight._map.visible, flight._map._patches.size()])
	await _key(tree, KEY_M)
	await _key(tree, KEY_ESCAPE)
	log.append("menu shown %s" % flight.hud.menu_visible())
	flight._on_menu("revert_launch")
	await _frames(tree, 10)
	log.append("reverted: %s" % flight.session.telemetry().situation)
	flight._on_menu("revert_hangar")
	await _frames(tree, 15)
	log.append("back in %s" % tree.current_scene.name)
	return {"status": "PASS", "log": "\n".join(log)}
