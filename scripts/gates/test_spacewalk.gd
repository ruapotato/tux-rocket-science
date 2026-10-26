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
## --test-spacewalk: a penguin's walk through the real flight scene. F
## takes one penguin out of the Hopper's pod (it moves seats: the pod is
## empty, the penguin vessel holds them) and the camera looks back at the
## rocket past it; S walks it toward the camera at its walking speed; Space jumps; walking back to the rocket (it
## climbs the hull to the hatch), F climbs back in (the penguin vessel is gone and the pod has its
## crew again); out of reach, F says so. With a screen, shots of each.
extends RefCounted

const DIR := "user://dev_shots/"
var _log := PackedStringArray()
var _failed := 0


func _check(ok: bool, msg: String) -> void:
	_log.append(("ok   " if ok else "FAIL ") + msg)
	if not ok:
		_failed += 1


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	DirAccess.make_dir_recursive_absolute(DIR)
	var gpu := DisplayServer.get_name() != "headless"
	var scene := await _flight(tree, FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "hopper.craft.json"))
	var s: FlightSession = scene.session
	var hop := s.active
	var crew: Array = Sim.crew_of(hop)

	# 1. out through the hatch
	scene._key(_ev(KEY_F))
	var pen := s.active
	var t := s.telemetry()
	var pen_crew: Array = Sim.crew_of(pen)
	_check(pen != hop and t.get("eva", false) and pen_crew == crew and Sim.crew_of(hop).is_empty() and t.name == crew[0]
		and scene._cam_dist < 10.0,
		"F: %s climbs out (vessel %d '%s'), pod now empty, camera in to %.1f m" % [pen_crew, pen, t.name, scene._cam_dist])
	s.step_ticks(100)
	t = s.telemetry()
	_check(int(t.contacts) > 0 and t.sas == "upright" and absf(t.vertical_speed) < 0.1,
		"standing on the pad: %d feet down, SAS %s, vertical %.2f m/s" % [int(t.contacts), t.sas, t.vertical_speed])
	if gpu:
		await _frames(tree, 20)
		await _shot(scene, "out")

	# 2. S walks toward the camera (away from the rocket) at the walking speed
	var top: float = Sim.part("eva_penguin").eva.walk_speed
	Input.parse_input_event(_ev(KEY_S))
	Input.flush_buffered_events()
	scene._eva_controls()
	var cam_back: Vector3 = scene._cam.global_basis.z
	s.step_ticks(50)  # up to speed
	var p0 := _pos(s, hop)
	s.step_ticks(100)
	var walked := _pos(s, hop) - p0
	var up := Frame.vec_arr(s.telemetry().up)
	var flat: Vector3 = cam_back - up * cam_back.dot(up)
	var along := walked.dot(flat.normalized())
	_check(absf(along / (top * 2.0) - 1.0) < 0.05 and walked.length() < along * 1.05,
		"held S: %.2f m toward the camera (away from the rocket) in 2 s (%.1f m/s for 2 s = %.2f), %.2f m off line" % [along, top, top * 2.0,
			sqrt(maxf(0.0, walked.length_squared() - along * along))])
	if gpu:
		for i in 12:
			scene._eva_controls()
			s.step_ticks(2)
			await _frames(tree, 1)
		await _shot(scene, "walk")
	Input.parse_input_event(_ev(KEY_S, false))
	Input.flush_buffered_events()
	scene._eva_controls()
	s.step_ticks(50)

	# 3. out of reach F refuses; Space jumps
	scene._key(_ev(KEY_F))
	_check(s.active == pen and scene.hud._toast.text.contains("within reach"), "F out of reach: stays out ('%s')" % scene.hud._toast.text)
	scene._key(_ev(KEY_SPACE))
	s.step_ticks(3)
	t = s.telemetry()
	var jump: float = Sim.part("eva_penguin").eva.jump_speed
	_check(int(t.contacts) == 0 and absf(t.vertical_speed - (jump - 9.81 * 0.04)) < 0.1,
		"Space: off the ground, rising at %.2f m/s (jump %.1f m/s on the next tick, then 2 ticks of gravity)" % [t.vertical_speed, jump])
	if gpu:
		await _frames(tree, 3)
		await _shot(scene, "jump")
	s.step_ticks(100)

	# 4. back to the pod and in
	var steps := 0
	while steps < 200 and (s.core.eva_hatch(pen, hop) as PackedFloat64Array)[0] < 0:
		var to := -_pos(s, hop)  # the pod, from the penguin
		var tt := s.telemetry()
		var u := Frame.vec_arr(tt.up)
		to -= u * to.dot(u)
		to = to.normalized() * top
		s.set_walk(to.dot(Frame.vec_arr(tt.east)), to.dot(Frame.vec_arr(tt.north)))
		s.step_ticks(5)
		steps += 1
	s.set_walk(0.0, 0.0)
	var hint_near: String = scene._hatch_near()
	scene._key(_ev(KEY_F))
	var gone := not s.core.vessel_alive(pen) or s.core.vessel_serial(pen) != s._active_serial
	_check(s.active == hop and Sim.crew_of(hop) == crew and gone and hint_near == "Hopper",
		"walked back in %.1f s: hatch in reach of '%s', F: %s back aboard, penguin vessel gone" % [steps * 0.1, hint_near, Sim.crew_of(hop)])
	return {"status": "PASS" if _failed == 0 else "FAIL", "log": "\n".join(_log)}


## The active vessel's position relative to vessel `other`, render frame.
func _pos(s: FlightSession, other: int) -> Vector3:
	var r := s.core.vessel_render(s.active, other)
	return Frame.vec(r, 1)


func _ev(code: Key, down := true) -> InputEventKey:
	var e := InputEventKey.new()
	e.keycode = code
	e.physical_keycode = code
	e.pressed = down
	return e


func _flight(tree: SceneTree, text: String) -> Node3D:
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	await _frames(tree, 2)
	Sim.tutorial = false
	Sim.pending_craft_text = text
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	return scene


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _shot(n: Node, name: String) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png(DIR + "eva_" + name + ".png")
	_log.append("saved eva_" + name)
