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
## Dev: the map with a target: a station in a 120 km orbit, a chaser in a
## 100 km orbit 30 degrees behind. The target's orbit shows in magenta and
## the closest approach is marked on both paths; a planned raise of the
## chaser's apoapsis brings it closer.
extends RefCounted

const DIR := "user://dev_shots/"


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var gpu := DisplayServer.get_name() != "headless"
	var g := Sim.game_session()
	var mu := 3.5316e12
	var orbit := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "puffin_ferry.craft.json")
	var R1 := 720000.0
	var st: int = g.core.flight_spawn_state(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "igloo_station.craft.json"), "floe",
		PackedFloat64Array([R1, 0, 0, 0, sqrt(mu / R1), 0]))
	var R2 := 700000.0
	var a := deg_to_rad(-30.0)
	var v2 := sqrt(mu / R2)
	var ch: int = g.core.flight_spawn_state(orbit, "floe", PackedFloat64Array([R2 * cos(a), R2 * sin(a), 0, -v2 * sin(a), v2 * cos(a), 0]))
	Sim.save_game()
	Sim.pending_vessel = ch
	for c in tree.root.get_children():
		if c is Node3D:
			c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	s.target = st
	var ca := s.closest_approach()
	log.append("no burn: closest %.1f km in %.0f s at %.1f m/s" % [ca.dist / 1000.0, ca.t - s.time(), ca.rel_speed])
	scene._map.visible = true
	scene._map.focus_on_vessel()
	scene._map._dist = 30.0
	scene._map._pitch = 1.2
	for i in 30:
		await tree.process_frame
	if gpu:
		await _shot(scene, "approach")
	# a prograde burn now: the closest approach changes, and is still exact
	s.nodes = [[s.time() + 60.0, 9.0, 0.0, 0.0]]
	ca = s.closest_approach()
	log.append("after a 9 m/s prograde node: closest %.2f km in %.0f s at %.1f m/s" % [ca.dist / 1000.0, ca.t - s.time(), ca.rel_speed])
	for i in 10:
		await tree.process_frame
	log.append("target panel:\n" + scene.hud._target_text.get_parsed_text())
	if gpu:
		await _shot(scene, "approach_node")
	return {"status": "PASS", "log": "\n".join(log)}


func _shot(n: Node, name: String) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png(DIR + "map_" + name + ".png")
