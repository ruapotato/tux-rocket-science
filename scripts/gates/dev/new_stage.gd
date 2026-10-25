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
## Dev: the hangar's "+ new stage" button opens an empty stage card, and a
## part chip dropped on it makes that stage.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	tree.root.add_child(hangar)
	for i in 5:
		await tree.process_frame
	hangar.model.load_text(Sim.pending_craft_text)
	await tree.process_frame
	var n0: int = hangar.model.stage_count()
	# press the topmost "+ new stage" (a new first stage)
	var plus: Button = null
	for b in hangar._stage_box.find_children("*", "Button", true, false):
		if (b as Button).text == "+ new stage":
			plus = b
			break
	var found := plus != null
	plus.pressed.emit()
	await tree.process_frame
	var card := false
	for l in hangar._stage_box.find_children("*", "Label", true, false):
		if (l as Label).text.begins_with("New stage"):
			card = true
	# drop the Orbiter's parachute chip on it
	var chute := -1
	for i in hangar.model.parts.size():
		if hangar.model.parts[i].part == "parachute":
			chute = i
	hangar._drop_on_gap({"kind": "stage_part", "index": chute}, hangar._empty_stage)
	await tree.process_frame
	var n1: int = hangar.model.stage_count()
	var top: int = hangar.model.parts[chute].stage
	if DisplayServer.get_name() != "headless":
		await RenderingServer.frame_post_draw
		hangar.get_viewport().get_texture().get_image().save_png("user://dev_shots/new_stage.png")
	var ok: bool = found and card and n1 == n0 + 1 and top == n1 - 1 and hangar._empty_stage == -1
	return {"status": "PASS" if ok else "FAIL", "log": "button %s, empty card %s, stages %d -> %d, parachute now stage %d (first SPACE)" % [found, card, n0, n1, top]}
