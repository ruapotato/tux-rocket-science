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
## Dev: the Waddle Ark from the Orbital Yard; shots outside, floating in the
## hab, standing on its aft wall under thrust, and on the spinning ring's
## deck. Env ONLY=name for one shot.
extends RefCounted

const DIR := "user://dev_shots/"
var _log := PackedStringArray()


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	for c in tree.root.get_children():
		if c is Node3D or c is Control:
			c.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "waddle_ark.craft.json")
	Sim.pending_site = "orbit"
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var t := s.telemetry()
	_log.append("%s at %.0f km, rooms %d, spin design %.3f rad/s" % [t.name, t.altitude / 1000.0, int(t.get("rooms", 0)), float(t.get("spin_design", 0.0))])
	scene._cam_dist = 70.0
	scene._cam_pitch = 0.3
	await _frames(tree, 30)
	await _shot(scene, "ark_outside")
	# inside, floating
	scene._toggle_inside()
	s.step_ticks(10)
	await _frames(tree, 20)
	t = s.telemetry()
	_log.append("inside: %s" % [t.get("walker", {})])
	_log.append("hint: %s" % scene.hud._hint.text)
	await _frames(tree, 10)
	await _shot(scene, "hab_float")
	# full throttle: the floor is the aft wall
	scene._throttle = 1.0
	scene._stage()
	for k in 8:
		s.step_ticks(10)
		await _frames(tree, 2)
	t = s.telemetry()
	_log.append("under thrust: felt %.3f g, standing %s, thrust %.0f kN; hint: %s" % [float(t.walker.felt_g), t.walker.standing, float(t.get("thrust", 0.0)) / 1000.0, scene.hud._hint.text])
	await _frames(tree, 30)
	await _shot(scene, "hab_thrust")
	scene._throttle = 0.0
	s.set_throttle(0.0)
	s.step_ticks(5)
	# spin up, then into the ring
	scene._toggle_spin()
	var ticks := 0
	while ticks < 50 * 120:
		s.step_ticks(50)
		ticks += 50
		t = s.telemetry()
		if absf(float(t.spin_rate) - float(t.spin_design)) < 1e-3 * float(t.spin_design):
			break
	_log.append("spun up in %.1f s: %.4f rad/s" % [ticks / 50.0, float(t.spin_rate)])
	scene._leave_inside()
	var parts: Array = JSON.parse_string(s.core.vessel_parts(s.active))
	var ring := -1
	for k in parts.size():
		if parts[k].part == "spin_ring":
			ring = k
	scene._toggle_inside()  # into the first room, then over to the ring
	s.core.walker_leave(s.active)
	s.core.walker_enter(s.active, ring)
	for k in 6:
		s.step_ticks(25)
		await _frames(tree, 2)
	t = s.telemetry()
	_log.append("on the deck: felt %.3f g, standing %s; hint: %s" % [float(t.walker.felt_g), t.walker.standing, scene.hud._hint.text])
	await _frames(tree, 40)
	await _shot(scene, "ring_deck")
	# walking spinward along the deck
	var key := InputEventKey.new()
	key.keycode = KEY_W
	key.physical_keycode = KEY_W
	key.pressed = true
	Input.parse_input_event(key)
	for k in 90:
		await _frames(tree, 1)
	t = s.telemetry()
	_log.append("walking: v %s, standing %s, view yaw %.2f pitch %.2f" % [t.walker.v, t.walker.standing, scene._in_yaw, scene._in_pitch])
	await _shot(scene, "ring_deck2")
	key.pressed = false
	Input.parse_input_event(key)
	return {"status": "PASS", "log": "\n".join(_log)}


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _shot(n: Node, name: String) -> void:
	var only := OS.get_environment("ONLY")
	if only != "" and only != name:
		return
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png(DIR + "interior_" + name + ".png")
	_log.append("saved interior_" + name)
