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
## Dev: a miner. In the hangar its mining line and warnings; on the pad at
## Floe, C switches the drill and refinery on: ore comes in at the drill's
## rate x Floe's ore, the refinery fills the tank, and the PROPELLANT panel
## says so. Shots of the drill close up (GPU runs).
extends RefCounted

var _log := PackedStringArray()


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	_clear(tree)
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("probe_core"))
	var bin := m.attach_stack(CraftModel.held_single("ore_bin"), 0, "bottom")[0]
	var ref := m.attach_stack(CraftModel.held_single("refinery"), bin, "bottom")[0]
	var tank := m.attach_stack(CraftModel.held_single("tank_m"), ref, "bottom")[0]
	m.attach_stack(CraftModel.held_single("engine_lifter"), tank, "bottom")
	m.attach_surface(CraftModel.held_single("solar_panel"), bin, 0.0, 0.0, 4)
	m.attach_surface(CraftModel.held_single("battery"), ref, PI / 4, 0.0, 2)
	m.attach_surface(CraftModel.held_single("landing_leg"), tank, PI / 4, -0.6, 4)
	m.attach_surface(CraftModel.held_single("drill"), tank, 0.0, -0.5, 1)
	m.attach_surface(CraftModel.held_single("fin"), tank, PI / 4, -0.5, 4)
	var text := m.save_text()

	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	tree.root.add_child(hangar)
	await _frames(tree, 5)
	hangar.model.load_text(text)
	hangar._frame_craft()
	await _frames(tree, 10)
	_log.append("hangar:\n" + hangar._summary.get_parsed_text())
	for w in hangar.craft_warnings():
		_log.append("  %s %s" % ["ERR " if w.bad else "tip ", w.text])
	await _shot(hangar, "mining_hangar")

	_clear(tree)
	Sim.pending_craft_text = text
	Sim.pending_site = "pad"
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var t0 := s.telemetry()
	scene._key(_ev(KEY_C))
	s.step_ticks(10)
	var a := s.telemetry()
	s.step_ticks(500)
	await _frames(tree, 3)
	var t := s.telemetry()
	var want := 0.25 * float(t.ore_abundance) * 10.0
	var fuel_text := ""
	for c in scene.hud._fuel.get_node("V").get_children():
		if c is Label:
			fuel_text += " | " + c.text
	_log.append("pad: mode %s landed %s; after C: drills on %d, refineries on %d; 10 s: ore %.3f kg (rate %.4f, want about %.3f), refine %.4f kg/s, mining %.2f kW, touching %d" % [
		t.mode, t.landed, int(a.drills_on), int(a.refineries_on), t.ore, t.ore_rate, want, t.refine_rate, t.mining_kw, int(t.drills_touching)])
	_log.append("panel:" + fuel_text)
	var ok: bool = int(a.drills_on) == 1 and int(a.refineries_on) == 1 and float(t.ore) + float(t.liquid) - float(t0.liquid) > 0.0
	if DisplayServer.get_name() != "headless":
		scene._cam_dist = 9.0
		scene._cam_pitch = 0.1
		await _frames(tree, 20)
		await _shot(scene, "mining_pad")
	scene._key(_ev(KEY_C))
	s.step_ticks(5)
	t = s.telemetry()
	ok = ok and int(t.drills_on) == 0 and float(t.ore_rate) == 0.0
	_log.append("C again: drills on %d, rate %.3f" % [int(t.drills_on), t.ore_rate])
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(_log)}


func _clear(tree: SceneTree) -> void:
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()


func _ev(code: Key) -> InputEventKey:
	var e := InputEventKey.new()
	e.keycode = code
	e.pressed = true
	return e


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _shot(n: Node, name: String) -> void:
	if DisplayServer.get_name() == "headless":
		return
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png("user://dev_shots/%s.png" % name)
	_log.append("saved " + name)
