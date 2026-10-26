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
## Dev: the Gull from the runway: brakes off, full throttle, rotate at
## speed, climb out. Logs speed, pitch, height and wheel contact; SHOTS=1
## with a screen saves pictures.
extends RefCounted

const DIR := "user://dev_shots/"


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var gpu := DisplayServer.get_name() != "headless"
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "gull.craft.json")
	Sim.pending_site = "runway"
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var t := s.telemetry()
	log.append("on the runway: %s, brakes %s, %d wheels, contacts %d, pitch %.1f, alt %.1f m (terrain %.2f)" % [t.situation, t.brakes, t.wheels, t.contacts, t.pitch_deg if t.has("pitch_deg") else -99.0, t.altitude, t.altitude_terrain])
	if gpu:
		scene._cam_dist = 25.0
		scene._cam_pitch = 0.45
		await _frames(tree, 30)
		await _shot(scene, "runway")
		scene._cam_dist = 400.0
		scene._cam_pitch = 0.6
		await _frames(tree, 30)
		await _shot(scene, "runway_far")
	s.step_ticks(100)
	t = s.telemetry()
	log.append("settled: contacts %d, speed %.2f, alt terrain %.2f" % [t.contacts, t.surface_speed, t.altitude_terrain])
	scene._throttle = 1.0
	scene._stage()
	s.core.vessel_set_brakes(s.active, false)
	var rotated := false
	for k in 50 * 90:
		s.set_throttle(1.0)
		t = s.telemetry()
		if t.surface_speed > float(OS.get_environment("VR")) if OS.get_environment("VR") != "" else t.surface_speed > 95.0:
			rotated = true
		s.set_input(float(OS.get_environment("PITCH")) if OS.get_environment("PITCH") != "" and rotated and t.altitude_terrain < 40.0 else (1.0 if rotated and t.altitude_terrain < 40.0 else 0.0), 0.0, 0.0)
		s.step_ticks(1)
		if k % 100 == 0:
			log.append("t %.0f: %.1f m/s, alt %.1f m, vs %.1f, contacts %d, thrust %.1f kN, sas %s" % [k / 50.0, t.surface_speed, t.altitude_terrain, t.vertical_speed, t.contacts, t.thrust / 1000.0, t.sas])
		if t.altitude_terrain > 600.0 or not t.alive:
			break
	t = s.telemetry()
	log.append("end: alive %s, %.1f m/s, alt %.0f m, steering by the craft %s" % [t.alive, t.surface_speed, t.altitude_terrain, scene.steer_craft])
	# craft-relative keys: D yaws right (east -> south), then E rolls right
	s.set_input(0, 0, 0)
	if OS.get_environment("DIRECT") != "":
		var ta := s.telemetry()
		for k in 15:
			s.set_input(0, 0, 1)
			s.step_ticks(1)
		var tb := s.telemetry()
		log.append("direct roll +1 for 0.3 s: right wing up %.3f -> %.3f" % [Frame.vec_arr(ta.side).dot(Frame.vec_arr(ta.up)), Frame.vec_arr(tb.side).dot(Frame.vec_arr(tb.up))])
		s.set_input(0, 0, 0)
	for key in ([KEY_E, KEY_Q, KEY_D] if OS.get_environment("ORDER") == "" else [KEY_E]):
		var t0 := s.telemetry()
		var ev := InputEventKey.new()
		ev.keycode = key
		ev.physical_keycode = key
		ev.pressed = true
		Input.parse_input_event(ev)
		Input.flush_buffered_events()
		for k in 15:
			scene._controls(0.02)
			s.step_ticks(1)
		ev = ev.duplicate()
		ev.pressed = false
		Input.parse_input_event(ev)
		Input.flush_buffered_events()
		scene._controls(0.02)
		var t1 := s.telemetry()
		if not t1.get("alive", false):
			log.append("key %s: the Gull is gone" % OS.get_keycode_string(key))
			break
		var bank_now: float = Frame.vec_arr(t1.side).dot(Frame.vec_arr(t1.up))
		var bank_was: float = Frame.vec_arr(t0.side).dot(Frame.vec_arr(t0.up))
		log.append("    right after: right wing up %.3f -> %.3f" % [bank_was, bank_now])
		s.step_ticks(100)
		var n0 := Frame.vec_arr(t0.nose)
		var n1 := Frame.vec_arr(t1.nose)
		var up := Frame.vec_arr(t1.up)
		var east := Frame.vec_arr(t1.east)
		var north := Frame.vec_arr(t1.north)
		var hdg0 := rad_to_deg(atan2(n0.dot(east), n0.dot(north)))
		var hdg1 := rad_to_deg(atan2(n1.dot(east), n1.dot(north)))
		var side := Frame.vec_arr(t1.side)
		log.append("key %s for 0.3 s (then 2 s): heading %.1f -> %.1f, pitch %.1f -> %.1f, bank (side.up) %.2f" % [OS.get_keycode_string(key), hdg0, hdg1,
			rad_to_deg(asin(n0.dot(up))), rad_to_deg(asin(n1.dot(up))), side.dot(up)])
	if gpu:
		await _frames(tree, 20)
		await _shot(scene, "climb")
	return {"status": "PASS", "log": "\n".join(log)}


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _shot(n: Node, name: String) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png(DIR + "gull_" + name + ".png")
