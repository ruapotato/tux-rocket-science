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
## Dev: the Orbiter climbing through thick air: the stick swings the fins'
## flaps (roll: all three the same way round), and with the stick and SAS
## off they come back to rest.
extends RefCounted

const DIR := "user://dev_shots/"


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var gpu := DisplayServer.get_name() != "headless"
	var scene := await _flight(tree, FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json"))
	var s: FlightSession = scene.session
	scene._throttle = 1.0
	scene._stage()
	s.step_ticks(50 * 25)
	await _frames(tree, 5)
	scene._cam_dist = 8.0
	scene._cam_pitch = -0.45
	scene._cam_yaw = 1.2
	var ok := true
	log.append("before: pitch %.1f" % scene.hud.navball.pitch)
	for key in [KEY_W, KEY_E]:
		Input.parse_input_event(_ev(key, true))
		await _frames(tree, 15)
		var t: Dictionary = s.telemetry()
		var flaps := _flaps(scene)
		log.append("pitch %.1f; %s held at %.1f km, %.0f m/s, q %.1f kPa: flaps %s deg" % [scene.hud.navball.pitch,
			OS.get_keycode_string(key), t.altitude / 1000.0, t.surface_speed, t.dynamic_pressure / 1000.0, flaps])
		var moved := 0
		for f in flaps:
			if absf(f) > 5.0:
				moved += 1
		ok = ok and moved >= (2 if key == KEY_W else 3)
		if gpu:
			scene.hud.visible = false
			await _frames(tree, 1)
			await RenderingServer.frame_post_draw
			scene.get_viewport().get_texture().get_image().save_png(DIR + "fins_%s.png" % OS.get_keycode_string(key))
		scene.hud.visible = true
		Input.parse_input_event(_ev(key, false))
		await _frames(tree, 30)
	s.set_sas("off")
	await _frames(tree, 30)
	var rest := _flaps(scene)
	log.append("stick released: flaps %s deg" % [rest])
	for f in rest:
		ok = ok and absf(f) < 1.0
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(log)}


## Shown flap angles of the active vessel's fins, degrees.
func _flaps(scene: Node) -> Array:
	var out := []
	for id in scene._views:
		var view: VesselView = scene._views[id]
		for uid in view._parts:
			var fl = view._parts[uid].get("flap")
			if fl:
				out.append(snappedf(rad_to_deg((fl as Node3D).rotation.z), 0.1))
	return out


func _ev(code: Key, down: bool) -> InputEventKey:
	var e := InputEventKey.new()
	e.keycode = code
	e.physical_keycode = code
	e.pressed = down
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
