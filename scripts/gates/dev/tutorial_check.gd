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
## Dev: flies the First Flight walkthrough the way a player would and checks
## each step advances only when its action happens.
extends RefCounted

const DIR := "user://dev_shots/"
var _log := PackedStringArray()


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "hopper.craft.json")
	Sim.tutorial = true
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	var tut: FirstFlight = scene.tutorial
	var s: FlightSession = scene.session
	var gpu := DisplayServer.get_name() != "headless"
	var ok := tut != null and tut.step == 0
	if gpu:
		await _shot(scene, "tutorial_pad")
	# nothing advances on its own
	s.step_ticks(100)
	tut.update()
	ok = ok and tut.step == 0
	_note(tut, "pad, 2 s idle")
	scene._throttle = 1.0
	await _frames(tree, 2)  # the throttle reaches the sim
	tut.update()
	ok = ok and tut.step == 1
	_note(tut, "Z")
	scene._stage()
	await _frames(tree, 1)
	tut.update()
	ok = ok and tut.step == 2
	_note(tut, "SPACE")
	var guard := 0
	while int(s.telemetry().get("engines_lit", 0)) > 0 and guard < 50 * 300:
		s.step_ticks(25)
		guard += 25
	tut.update()
	ok = ok and tut.step == 3
	_note(tut, "burnout after %.0f s" % (guard / 50.0))
	scene._stage()
	tut.update()
	ok = ok and tut.step == 4
	_note(tut, "SPACE drops the tank")
	scene._map.visible = true
	tut.update()
	scene._map.visible = false
	ok = ok and tut.step == 5
	_note(tut, "M")
	if gpu:
		await _frames(tree, 10)
		await _shot(scene, "tutorial_chute_step")
	scene._deploy_chutes()
	tut.update()
	ok = ok and tut.step == 6
	_note(tut, "P")
	guard = 0
	while not s.telemetry().get("landed", false) and guard < 50 * 900:
		s.step_ticks(50)
		guard += 50
	await _frames(tree, 2)
	tut.update()
	ok = ok and tut.step == 7 and not tut.failed
	_note(tut, "landed after %.0f s more" % (guard / 50.0))
	if gpu:
		await _frames(tree, 10)
		await _shot(scene, "tutorial_done")
	scene.queue_free()
	Sim.tutorial = false
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(_log)}


func _note(tut: FirstFlight, what: String) -> void:
	_log.append("%-24s -> step %d %s" % [what, tut.step + 1, FirstFlight.STEPS[tut.step][0]])


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _shot(n: Node, name: String) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png(DIR + "tut_" + name + ".png")
	_log.append("saved tut_" + name)
