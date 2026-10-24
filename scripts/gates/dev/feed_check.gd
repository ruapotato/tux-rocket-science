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
## Dev: the Sidecar's side tanks feed the core and run dry first; an engine
## no tank reaches says so when it lights; the hangar lists its warnings.
extends RefCounted

const DIR := "user://dev_shots/"
var _log := PackedStringArray()
var _ok := true


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var gpu := DisplayServer.get_name() != "headless"

	# 1. Sidecar: liftoff, side tanks dry first, then SPACE drops them
	var scene := await _flight(tree, FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "sidecar.craft.json"))
	var s: FlightSession = scene.session
	scene._throttle = 1.0
	scene._stage()
	await _frames(tree, 3)
	var t := s.telemetry()
	_log.append("sidecar liftoff: %d engines lit, thrust %.0f kN, toast '%s'" % [t.engines_lit, t.thrust / 1000.0, scene.hud._toast.text])
	if gpu:
		scene._cam_dist = 40.0
		scene._cam_pitch = 0.05
		s.step_ticks(150)
		await _frames(tree, 20)
		await _shot(scene, "sidecar_climb")
	var ticks := 0
	while (t.get("starved", []) as Array).is_empty() and ticks < 50 * 120:
		s.step_ticks(10)
		ticks += 10
		t = s.telemetry()
	await _frames(tree, 2)
	t = s.telemetry()
	var st: Array = t.get("starved", [])
	_log.append("after %.1f s: starved %s, engines lit %d, liquid %.0f / %.0f kg, hint '%s'" % [
		ticks / 50.0, st, t.engines_lit, t.liquid, t.liquid_capacity, scene.hud._hint.text])
	_ok = _ok and st.size() == 2 and int(t.engines_lit) == 1 and st[0].tank
	scene._stage()
	await _frames(tree, 2)
	t = s.telemetry()
	_log.append("SPACE: toast '%s', engines lit %d, starved %s" % [scene.hud._toast.text, t.engines_lit, t.starved])
	_ok = _ok and (t.starved as Array).is_empty() and int(t.engines_lit) == 1

	# 2. an engine on a bare radial decoupler: no tank can reach it
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	var tank := m.attach_stack(CraftModel.held_single("tank_l"), 0, "bottom")[0]
	m.attach_stack(CraftModel.held_single("engine_lifter"), tank, "bottom")
	var dr := m.attach_surface(CraftModel.held_single("decoupler_radial"), tank, 0.0, 0.0, 1)
	m.attach_stack(CraftModel.held_single("engine_small"), dr[0], "outer")
	scene = await _flight(tree, m.save_text())
	s = scene.session
	scene._throttle = 1.0
	scene._stage()
	await _frames(tree, 3)
	var toast: String = scene.hud._toast.text
	var hint: String = scene.hud._hint.text
	_log.append("bare radial engine: toast '%s'\n  hint '%s'" % [toast, hint])
	_ok = _ok and toast.contains("no tank reaches it") and hint.contains("Siphon Feed Line")
	if gpu:
		await _frames(tree, 10)
		await _shot(scene, "starved_engine")

	# 3. the hangar's warnings for that craft, and the Sidecar there
	for c in tree.root.get_children():
		if c is Node3D:
			c.queue_free()
	Sim.pending_craft_text = m.save_text()
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	tree.root.add_child(hangar)
	await _frames(tree, 5)
	hangar.model.load_text(m.save_text())
	await _frames(tree, 3)
	var ws: Array = hangar.craft_warnings()
	for w in ws:
		_log.append("  hangar %s: %s" % ["ERR " if w.bad else "tip ", w.text])
	_ok = _ok and ws.size() >= 3
	if gpu:
		await _shot(hangar, "hangar_warnings")
		hangar.model.load_text(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "sidecar.craft.json"))
		hangar._frame_craft()
		await _frames(tree, 5)
		await _shot(hangar, "hangar_sidecar")
		_log.append("  sidecar warnings: %s" % [hangar.craft_warnings()])
		# close on a feed line: high on a side tank, facing the core
		for i in hangar.model.parts.size():
			if hangar.model.parts[i].part == "feed_line":
				var fp: Array = hangar.model.parts[i].pos
				hangar._cam_target = hangar._craft_root.global_transform * Vector3(fp[0], fp[1], fp[2])
				break
		# holding a feed line on a side tank: facing the core, then facing away
		var side := -1
		for i in hangar.model.parts.size():
			if hangar.model.parts[i].part == "tank_l" and hangar.model.parts[i].parent >= 0 and hangar.model.parts[hangar.model.parts[i].parent].part == "decoupler_radial":
				side = i
				break
		hangar._held = CraftModel.held_single("feed_line")
		var held: Dictionary = hangar._held
		var p_in: Array = hangar.model.surface_placements(held, side, PI, -0.6, 1)
		hangar._show_feed_aim(p_in)
		var t_in: String = hangar._aim_text
		var p_out: Array = hangar.model.surface_placements(held, side, 0.0, -0.6, 1)
		hangar._show_feed_aim(p_out)
		var t_out: String = hangar._aim_text
		_log.append("  aim facing the core: '%s'; facing away: '%s'" % [t_in, t_out])
		_ok = _ok and t_in.contains("pipes") and t_out.contains("reaches nothing")
		hangar._show_feed_aim(p_in)
		hangar._cam_dist = 3.0
		hangar._cam_yaw = 0.3
		hangar._cam_pitch = 0.15
		hangar._update_camera()
		await _frames(tree, 5)
		await _shot(hangar, "hangar_hose")
		hangar._show_feed_aim([])
		hangar._held = {}
	return {"status": "PASS" if _ok else "FAIL", "log": "\n".join(_log)}


func _flight(tree: SceneTree, text: String) -> Node3D:
	for c in tree.root.get_children():
		if c is Node3D:
			c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
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
	n.get_viewport().get_texture().get_image().save_png(DIR + "feed_" + name + ".png")
	_log.append("saved feed_" + name)
