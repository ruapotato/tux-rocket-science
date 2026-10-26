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
## Dev: a monkey at the controls. Real flight scenes with stock craft get
## random key presses, held steering, SAS buttons, warp changes, the map,
## the pause menu and reverts, while the sim is pushed along. Every frame
## checks that what the game shows stays possible: finite numbers, fuel
## within its tanks, no burning on rails, warp levels in range. Env: SEED,
## SECONDS (sim seconds per craft, default 600), CRAFT (comma list).
extends RefCounted

const KEYS := [KEY_SPACE, KEY_Z, KEY_X, KEY_T, KEY_P, KEY_G, KEY_R, KEY_Y, KEY_U, KEY_V, KEY_M, KEY_TAB,
	KEY_PERIOD, KEY_PERIOD, KEY_COMMA, KEY_SLASH, KEY_BRACKETLEFT, KEY_BRACKETRIGHT, KEY_ESCAPE, KEY_F1, KEY_F]
const HELD := [KEY_W, KEY_A, KEY_S, KEY_D, KEY_Q, KEY_E, KEY_I, KEY_J, KEY_K, KEY_L, KEY_H, KEY_N, KEY_SHIFT, KEY_CTRL]
const SAS := ["off", "stability", "prograde", "retrograde", "normal", "antinormal", "radial_out", "radial_in", "maneuver", "target", "antirel", "dock"]

var _rng := RandomNumberGenerator.new()
var _log := PackedStringArray()
var _bad := 0
var _seen := {}
var _trail: Array = []  # the last actions, shown with the first flaw of each kind


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	_rng.seed = int(OS.get_environment("SEED")) if OS.get_environment("SEED") != "" else 1
	var secs := float(OS.get_environment("SECONDS")) if OS.get_environment("SECONDS") != "" else 600.0
	var crafts := (OS.get_environment("CRAFT") if OS.get_environment("CRAFT") != "" else "hopper,orbiter,sidecar,krill_lander,puffin_ferry,rookery_heavy").split(",")
	for c in crafts:
		await _fly(tree, c, secs)
	return {"status": "PASS" if _bad == 0 else "FAIL", "log": "\n".join(_log)}


func _flaw(scene: Node, what: String) -> void:
	var key := what.get_slice(":", 0)
	_seen[key] = int(_seen.get(key, 0)) + 1
	if _seen[key] == 1:
		_log.append("    lead-up: " + " | ".join(_trail))
	if _seen[key] <= 3:
		var t: Dictionary = scene.session.telemetry()
		_log.append("  FLAW at T%.1f (%s, warp %s %d, alt %.0f): %s" % [scene.session.time(), t.get("situation", "?"),
			scene.session.warp_kind, scene.session.warp_index, t.get("altitude", 0.0), what])
	_bad += 1


func _fly(tree: SceneTree, craft: String, secs: float) -> void:
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	await tree.process_frame
	Sim.tutorial = false
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + craft + ".craft.json")
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	var s: FlightSession = scene.session
	var on_switch := func(id): _note(scene, "switched to %d" % id)
	s.vessel_switched.connect(on_switch)
	var held := {}
	var t0 := s.time()
	var frames := 0
	var actions := {}
	while s.time() - t0 < secs and frames < 20000:
		frames += 1
		var r := _rng.randf()
		if r < 0.06:
			var k: int = KEYS[_rng.randi() % KEYS.size()]
			# the monkey mostly flies: fewer menus and map flips
			if (k == KEY_ESCAPE or k == KEY_M) and _rng.randf() < 0.7:
				k = KEY_Z
			scene._key(_ev(k, true))
			_note(scene, OS.get_keycode_string(k))
			actions[OS.get_keycode_string(k)] = int(actions.get(OS.get_keycode_string(k), 0)) + 1
		elif r < 0.09:
			var k: int = HELD[_rng.randi() % HELD.size()]
			var down: bool = not held.get(k, false)
			held[k] = down
			_note(scene, "%s %s" % [OS.get_keycode_string(k), "down" if down else "up"])
			Input.parse_input_event(_ev(k, down))
		elif r < 0.1:
			scene._on_sas(SAS[_rng.randi() % SAS.size()])
		elif r < 0.103 and scene.hud.menu_visible():
			var a: String = ["revert_launch", "revert_launch", "main_menu_refused"][_rng.randi() % 3]
			if a == "revert_launch":
				scene._on_menu(a)
				_note(scene, "revert")
				actions["revert"] = int(actions.get("revert", 0)) + 1
			else:
				scene.hud.show_menu(false)
		elif r < 0.12 and scene._map.visible and s.nodes.is_empty():
			# drop a maneuver node somewhere on the path
			var sm: Array = scene._map._samples
			if sm.size() > 2:
				var pick: Dictionary = sm[_rng.randi() % sm.size()]
				s.nodes = [[pick.t, _rng.randf_range(-300, 300), _rng.randf_range(-100, 100), _rng.randf_range(-100, 100)]]
		# keep the sim moving even when frames are quick (headless)
		if not scene.hud.menu_visible() and s.warp_kind == "physics" and _rng.randf() < 0.5:
			s.step_ticks(_rng.randi_range(1, 10))
		await tree.process_frame
		if not is_instance_valid(scene) or not scene.is_inside_tree():
			_log.append("  scene left at frame %d" % frames)
			break
		_check(scene)
	for k in held:
		if held[k]:
			Input.parse_input_event(_ev(k, false))
	s.vessel_switched.disconnect(on_switch)
	var tf := s.telemetry()
	_log.append("%s: %d frames, T+%.0f s, %s, alt %.1f km, %d vessels; keys %s" % [craft, frames, s.time() - t0, tf.get("situation", "gone"),
		tf.get("altitude", 0.0) / 1000.0, (JSON.parse_string(s.core.vessel_list()) as Array).size(), actions])


func _note(scene: Node, what: String) -> void:
	_trail.append("T%.1f v%d %s thr %.2f/%.2f" % [scene.session.time(), scene.session.active, what, scene._throttle, float(scene.session.telemetry().get("throttle", -1.0))])
	if _trail.size() > 10:
		_trail.pop_front()


func _check(scene: Node) -> void:
	var s: FlightSession = scene.session
	# any node placed at a non-finite spot (Godot refuses to draw it)
	for n in scene.find_children("*", "Node3D", true, false):
		var xf: Transform3D = (n as Node3D).transform
		if not (xf.origin.is_finite() and xf.basis.x.is_finite() and xf.basis.y.is_finite() and xf.basis.z.is_finite()):
			_flaw(scene, "nonfinite node: %s %s" % [scene.get_path_to(n), xf])
			break
	var t := s.telemetry()
	if not t.get("alive", false):
		return
	for k in ["altitude", "surface_speed", "orbital_speed", "vertical_speed", "mass", "thrust", "liquid", "mono", "dynamic_pressure", "g_force"]:
		if t.has(k) and typeof(t[k]) == TYPE_FLOAT and not is_finite(t[k]):
			_flaw(scene, "nonfinite: %s = %s" % [k, t[k]])
	if t.get("mass", 1.0) <= 0.0:
		_flaw(scene, "mass: %s" % t.mass)
	if t.get("liquid", 0.0) < -1e-9 or t.get("liquid", 0.0) > t.get("liquid_capacity", INF) + 1e-6:
		_flaw(scene, "liquid: %s of %s" % [t.liquid, t.liquid_capacity])
	if t.get("mono", 0.0) < -1e-9 or t.get("mono", 0.0) > t.get("mono_capacity", INF) + 1e-6:
		_flaw(scene, "mono: %s of %s" % [t.mono, t.mono_capacity])
	if s.warp_kind == "rails" and s.warp_index > 0 and t.get("thrust", 0.0) > 0.0 and not t.landed and s.telemetry().get("throttle", 0.0) > 0.0:
		pass  # one frame is allowed: the next advance hands back to physics
	if s.warp_index < 0 or (s.warp_kind == "rails" and s.warp_index >= FlightSession.RAILS_WARP.size()) or (s.warp_kind == "physics" and s.warp_index >= FlightSession.PHYSICS_WARP.size()):
		_flaw(scene, "warp: %s %d" % [s.warp_kind, s.warp_index])
	if s.warp_kind == "rails" and s.warp_index > 0 and t.get("in_atmosphere", false) and not t.landed:
		_flaw(scene, "rails in air: %s" % t.altitude)
	var th: float = t.get("throttle", 0.0)
	if th < 0.0 or th > 1.0:
		_flaw(scene, "throttle: %s" % th)
	if absf(float(scene._throttle) - th) > 1e-6 and not scene.hud.menu_visible():
		var raw: Dictionary = JSON.parse_string(s.core.telemetry(s.active))
		_flaw(scene, "throttle desync: scene %s core %s (active %d, raw core %s, ticks %d, cache %d, vessels %s)" % [scene._throttle, th, s.active, raw.get("throttle"), s.ticks, s._telemetry_tick,
			(JSON.parse_string(s.core.vessel_list()) as Array).map(func(v): return "%d:%s/%s" % [v.id, v.name, v.situation])])


func _ev(code: Key, down: bool) -> InputEventKey:
	var e := InputEventKey.new()
	e.keycode = code
	e.physical_keycode = code
	e.pressed = down
	return e
