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
## Dev: an RCS tug in orbit: R arms it, the slide keys fire the jets (as a
## player's key presses), the HUD shows mono; plus the hangar readout.
extends RefCounted

const DIR := "user://dev_shots/"


func _tug() -> CraftModel:
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	var mt := m.attach_stack(CraftModel.held_single("mono_tank"), 0, "bottom")[0]
	var t := m.attach_stack(CraftModel.held_single("tank_s"), mt, "bottom")[0]
	m.attach_stack(CraftModel.held_single("engine_small"), t, "bottom")
	m.attach_surface(CraftModel.held_single("rcs_block"), mt, PI / 4, 0.0, 4)
	m.attach_surface(CraftModel.held_single("mono_radial"), t, 0.0, 0.0, 2)
	m.craft_name = "RCS Tug"
	return m


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var gpu := DisplayServer.get_name() != "headless"
	var m := _tug()
	var st: Dictionary = m.stats()
	log.append("hangar: %d blocks, mono %.0f kg, slide %.1f m/s, warnings %s" % [st.rcs_blocks, st.mono, st.rcs_dv, []])
	var scene := await _flight(tree, m.save_text())
	scene.session.spawn_in_orbit(m.save_text(), "floe", 100000.0)
	scene._clear_views()
	scene._cam_dist = 9.0
	scene._cam_pitch = 0.25
	await _frames(tree, 30)
	scene._key(_ev(KEY_R))
	log.append("R: toast '%s'" % scene.hud._toast.text)
	var mono0: float = scene.session.telemetry().mono
	Input.parse_input_event(_ev(KEY_L))
	Input.parse_input_event(_ev(KEY_H))
	await _frames(tree, 40)
	var t: Dictionary = scene.session.telemetry()
	log.append("sliding: rcs %s, force %.0f N, mono %.2f -> %.2f kg, slid %.2f m/s, RCS button '%s'" % [t.rcs, t.rcs_force, mono0, t.mono, t.rcs_dv_expended, scene.hud._action_buttons.rcs.text])
	var ok: bool = t.rcs and t.rcs_force > 0.0 and t.mono < mono0
	if gpu:
		await RenderingServer.frame_post_draw
		scene.get_viewport().get_texture().get_image().save_png(DIR + "rcs_firing.png")
	var up := _ev(KEY_L)
	up.pressed = false
	Input.parse_input_event(up)
	up = _ev(KEY_H)
	up.pressed = false
	Input.parse_input_event(up)
	scene._key(_ev(KEY_R))
	await _frames(tree, 5)
	t = scene.session.telemetry()
	log.append("off: rcs %s, force %.0f N, toast '%s'" % [t.rcs, t.rcs_force, scene.hud._toast.text])
	ok = ok and not t.rcs and t.rcs_force == 0.0
	if gpu:
		Sim.pending_craft_text = m.save_text()
		for c in tree.root.get_children():
			if c is Node3D or c is Control:
				c.queue_free()
		var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
		tree.root.add_child(hangar)
		await _frames(tree, 5)
		hangar.model.load_text(m.save_text())
		hangar._frame_craft()
		await _frames(tree, 8)
		await RenderingServer.frame_post_draw
		hangar.get_viewport().get_texture().get_image().save_png(DIR + "rcs_hangar.png")
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(log)}


func _ev(code: Key) -> InputEventKey:
	var e := InputEventKey.new()
	e.keycode = code
	e.physical_keycode = code
	e.pressed = true
	return e


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _flight(tree: SceneTree, text: String) -> Node3D:
	for c in tree.root.get_children():
		if c is Node3D:
			c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	Sim.pending_craft_text = text
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	return scene
