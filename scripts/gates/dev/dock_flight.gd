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
## Dev: a docking through the flight scene. A station holds retrograde with
## its port facing back; a chaser 40 m behind picks it as target, holds
## DOCK, and closes on RCS the way a pilot would (slide to null the drift
## sideways, creep in at 0.3 m/s). Latch, then U to let go.
extends RefCounted

const DIR := "user://dev_shots/"


func _craft(name: String, engine: bool) -> String:
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("probe_core"))
	m.attach_stack(CraftModel.held_single("dock_port"), 0, "top")
	var mt := m.attach_stack(CraftModel.held_single("mono_tank"), 0, "bottom")[0]
	var t := m.attach_stack(CraftModel.held_single("tank_m"), mt, "bottom")[0]
	if engine:
		m.attach_stack(CraftModel.held_single("engine_small"), t, "bottom")
	m.attach_surface(CraftModel.held_single("rcs_block"), mt, PI / 4, 0.0, 4)
	m.craft_name = name
	return m.save_text()


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var gpu := DisplayServer.get_name() != "headless"
	var g := Sim.game_session()
	var R := 600000.0 + 120000.0
	var v := sqrt(3.5316e12 / R)
	var st: int = g.core.flight_spawn_state(_craft("Igloo Station", false), "floe", PackedFloat64Array([R, 0, 0, 0, v, 0]))
	g.core.vessel_set_sas(st, "retrograde", PackedFloat64Array())
	var ch: int = g.core.flight_spawn_state(_craft("Chaser", true), "floe", PackedFloat64Array([R, -40.0, 0, 0, v, 0]))
	Sim.save_game()
	Sim.pending_vessel = ch
	var scene := await _flight(tree)
	var s: FlightSession = scene.session
	s.step_ticks(50 * 40)  # the station turns round
	s.target = st
	scene._toggle_rcs()
	scene._on_sas("dock")
	var ti := s.target_info()
	log.append("start: range %.1f m, port %.1f m, %.1f deg off" % [ti.distance, ti.port_distance, ti.port_angle_deg])
	scene._cam_dist = 22.0
	scene._cam_pitch = 0.2
	var docked_t := -1.0
	var shot_close := false
	for k in 50 * 400:
		ti = s.target_info()
		if ti.is_empty():
			docked_t = s.time()
			break
		var t := s.telemetry()
		var n := Vector3(t.nose[0], t.nose[1], t.nose[2]).normalized()
		var x := Vector3(t.side[0], t.side[1], t.side[2]).normalized()
		var z := x.cross(n)
		# wanted relative velocity: toward the target port, 0.3 m/s along,
		# plus a pull onto the line; RCS slides to make it so
		var pd := Vector3(ti.port_dir[0], ti.port_dir[1], ti.port_dir[2])
		var ax := Vector3(ti.port_axis[0], ti.port_axis[1], ti.port_axis[2])
		var sep := pd * float(ti.port_distance)
		var along := sep.dot(ax)
		var lateral := sep - ax * along
		var want := ax * minf(0.3, 0.05 + along * 0.05) + lateral * 0.2
		var rel := -Vector3(ti.rel_v[0], ti.rel_v[1], ti.rel_v[2])  # our velocity relative to the target
		var err := (want - rel) * 4.0
		s.set_translate(clampf(err.dot(x), -1, 1), clampf(err.dot(n), -1, 1), clampf(err.dot(z), -1, 1))
		s.step_ticks(1)
		if k % 10 == 0:
			await tree.process_frame
		if gpu and not shot_close and float(ti.port_distance) < 6.0:
			shot_close = true
			await _shot(scene, "dock_approach")
			log.append("approach: port %.2f m, %.1f deg off, %.2f m aside, closing %.2f m/s, HUD:\n%s" % [ti.port_distance, ti.port_angle_deg, ti.port_lateral, ti.port_closing, scene._target_text()])
	var tel := s.telemetry()
	log.append("docked at T+%.1f: active %d '%s', %d parts, toast '%s', vessels %d" % [docked_t, s.active, tel.name, tel.parts, scene.hud._toast.text, Sim.vessels().size()])
	var ok := docked_t > 0.0 and int(tel.parts) == 17
	await _frames(tree, 20)
	if gpu:
		await _shot(scene, "docked")
	scene._key(_ev(KEY_U))
	await _frames(tree, 2)
	log.append("U: toast '%s', vessels %d" % [scene.hud._toast.text, Sim.vessels().size()])
	ok = ok and Sim.vessels().size() == 2
	s.step_ticks(50 * 6)
	await _frames(tree, 20)
	if gpu:
		await _shot(scene, "undocked")
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(log)}


func _ev(code: Key) -> InputEventKey:
	var e := InputEventKey.new()
	e.keycode = code
	e.pressed = true
	return e


func _shot(scene: Node, name: String) -> void:
	await RenderingServer.frame_post_draw
	scene.get_viewport().get_texture().get_image().save_png(DIR + name + ".png")


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _flight(tree: SceneTree) -> Node3D:
	for c in tree.root.get_children():
		if c is Node3D:
			c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	Sim.tutorial = false
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	return scene
