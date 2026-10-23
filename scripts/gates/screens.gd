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
## --screens: renders the fixed review shots to res://screens/*.png.
## Needs a GPU (run without --headless):  godot -- --screens
##   hangar · launchpad · orbit · map · moon_surface
extends RefCounted

const OUT := "res://screens/"
var _log := PackedStringArray()


func run(runner: Node) -> Dictionary:
	if DisplayServer.get_name() == "headless":
		return {"status": "PENDING", "log": "--screens needs a renderer; run without --headless"}
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(OUT))
	var lander := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "krill_lander.craft.json")

	# 1. hangar
	Sim.pending_craft_text = lander
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	tree.root.add_child(hangar)
	hangar._cam_yaw = 0.9
	hangar._update_camera()
	await _settle(tree, 20)
	await _shot(tree, "hangar")
	hangar.queue_free()
	await tree.process_frame

	# 2. launchpad
	Sim.pending_craft_text = lander
	var flight: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(flight)
	flight._cam_yaw = 2.4
	flight._cam_pitch = 0.05
	flight._cam_dist = 55.0
	await _settle(tree, 120)
	await _shot(tree, "launchpad")

	# 3. orbit: the orbiter over Floe, in sunlight
	var s: FlightSession = flight.session
	s.spawn_in_orbit(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json"), "floe", 90000.0)
	s.core.vessel_set_mode(s.active, "rails")
	s.warp_to(s.time() + s.telemetry().orbit.period * 0.45)  # round to the day side
	flight._clear_views()
	flight._assign_crew()
	flight._cam_dist = 26.0
	flight._cam_pitch = 0.3
	flight._cam_yaw = 1.2
	await _settle(tree, 120)
	await _shot(tree, "orbit")

	# 4. map view with a planned Krill transfer
	var m := MoonMission.new(ControlAPI.new(s))
	var node := m.plan_transfer()
	s.nodes = [node]
	flight._map.visible = true
	flight._map.focus_body = 1
	flight._map._dist = 240.0
	flight._map._pitch = 1.05
	flight._map.selected_node = 0
	flight.hud.toast("", 0.01)
	await _settle(tree, 30)
	await _shot(tree, "map")
	flight._map.visible = false

	# 5. the lander standing on Krill, sun low for long shadows
	var cm := CraftModel.new()
	cm.load_text(lander)
	for i in cm.parts.size():
		if cm.parts[i].part == "decoupler_stack" and cm.parts[cm.parts[i].parent].part == "engine_small":
			cm.remove(i)
			break
	var core := s.core
	core.load_system(ProjectSettings.globalize_path(Sim.SYSTEM_PATH))
	var sub: PackedFloat64Array = core.subsolar_point(core.body_find("krill"))
	s.active = core.flight_place_landed(cm.save_text(), "krill", sub[0] + 8.0, sub[1] + 55.0)
	s.set_legs(true)
	s.nodes.clear()
	flight._clear_views()
	flight._assign_crew()
	flight._cam_dist = 22.0
	flight._cam_pitch = 0.12
	flight._cam_yaw = 0.7
	await _settle(tree, 200)
	await _shot(tree, "moon_surface")
	flight.queue_free()
	return {"status": "PASS", "log": "\n".join(_log)}


func _settle(tree: SceneTree, frames: int) -> void:
	for i in frames:
		await tree.process_frame


func _shot(tree: SceneTree, name: String) -> void:
	for n in tree.root.find_children("*", "FlightHUD", true, false):
		n.toast("", 0.01)
	await tree.process_frame
	await RenderingServer.frame_post_draw
	var img := tree.root.get_texture().get_image()
	var path := ProjectSettings.globalize_path(OUT + name + ".png")
	img.save_png(path)
	_log.append("%-13s %dx%d  %s" % [name, img.get_width(), img.get_height(), path])
