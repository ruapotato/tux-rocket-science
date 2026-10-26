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
	log.append("end: alive %s, %.1f m/s, alt %.0f m" % [t.alive, t.surface_speed, t.altitude_terrain])
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
