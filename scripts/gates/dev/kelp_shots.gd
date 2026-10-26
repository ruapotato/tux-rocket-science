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
## Dev: a body (BODY=, default Kelp): from orbit, falling (on a parachute
## where there's air), standing on it, and on the map.
extends RefCounted

const DIR := "user://dev_shots/"
var _log := PackedStringArray()


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var g := Sim.game_session()
	var bid := OS.get_environment("BODY") if OS.get_environment("BODY") != "" else "kelp"
	var info: Dictionary = JSON.parse_string(g.core.body_info(g.core.body_find(bid)))
	_log.append(bid + ": radius %.0f km, g %.2f m/s2, air %.0f km, SOI %.0f km" % [info.radius / 1000.0, info.mu / pow(info.radius, 2),
		info.atmosphere_height / 1000.0, info.soi / 1000.0])
	var orbiter := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	var R: float = info.radius + 150000.0
	var v := sqrt(info.mu / R)
	var id: int = g.core.flight_spawn_state(orbiter, bid, PackedFloat64Array([R, 0, 0, 0, v, 0]))
	Sim.pending_vessel = id
	var scene := await _flight(tree)
	var s: FlightSession = scene.session
	scene._cam_dist = 30.0
	if OS.get_environment("PITCH") != "":
		scene._cam_pitch = float(OS.get_environment("PITCH"))  # look down on a small world
	await _frames(tree, 30)
	await _shot(scene, "orbit")
	# down to 4 km on a chute
	s.core.vessel_remove(s.active)
	s.events.clear()
	var r2: float = info.radius + 4000.0
	var d: int = s.core.flight_spawn_state(orbiter, bid, PackedFloat64Array([r2, 0, 0, -40.0, 0, 0]))
	Sim.pending_vessel = d
	scene = await _flight(tree)  # a fresh scene (the removed orbiter would leave a wreck to look at)
	s = scene.session
	s.core.vessel_deploy_chutes(d)
	s.step_ticks(50 * 3)
	await _frames(tree, 30)
	var t := s.telemetry()
	_log.append("on the chute at %.1f km: %.1f m/s down, pressure %.0f kPa" % [t.altitude / 1000.0, -t.vertical_speed, t.pressure / 1000.0])
	await _shot(scene, "chute")
	var guard := 0
	while not s.telemetry().landed and guard < 50 * 600:
		s.step_ticks(50)
		guard += 50
	await _frames(tree, 30)
	t = s.telemetry()
	_log.append("landed after %.0f s: %s, touchdown %.1f m/s" % [guard / 50.0, t.situation, t.max_impact])
	scene._cam_pitch = 0.3
	scene._cam_dist = scene._framing_dist()
	for i in 60:
		await tree.process_frame  # the blast from the removed orbiter clears
	await _shot(scene, "ground")
	scene._cam_pitch = 1.1
	scene._cam_dist = 80.0
	for i in 10:
		await tree.process_frame
	await _shot(scene, "ground_above")

	var vv = scene._views.get(s.active)
	if vv:
		for uid in vv._parts:
			_log.append("part %s at %s visible %s" % [vv._parts[uid].part, vv._parts[uid].node.global_position, vv._parts[uid].node.is_visible_in_tree()])
			break
	_log.append("terrain: %s" % [s.core.ground_clearance(int(t.body_index), s.active, PackedFloat64Array([0, 0, 0]))])
	scene._map.visible = true
	scene._map.focus_on_vessel()
	scene._map._dist = 3.0
	await _frames(tree, 20)
	await _shot(scene, "map")
	return {"status": "PASS", "log": "\n".join(_log)}


func _flight(tree: SceneTree) -> Node3D:
	for c in tree.root.get_children():
		if c is Node3D:
			c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
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
	var body := OS.get_environment("BODY") if OS.get_environment("BODY") != "" else "kelp"
	n.get_viewport().get_texture().get_image().save_png(DIR + body + "_" + name + ".png")
