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
## Dev: renders the flight scene at a few moments for visual review.
extends RefCounted

const DIR := "user://dev_shots/"


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	DirAccess.make_dir_recursive_absolute(DIR)
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "krill_lander.craft.json")
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 40)
	await _shot(scene, "pad")
	# fly: full throttle, stage, 40 s of autopilot climb
	var api := ControlAPI.new(scene.session)
	scene._throttle = 1.0
	scene._stage()
	scene.session.set_sas("target", PackedFloat64Array(api.handle({"cmd": "query"}).telemetry.up))
	for i in 40:
		scene.session.step_ticks(5)
		await tree.process_frame
	scene._cam_yaw = 2.2
	scene._cam_pitch = 0.05
	await _frames(tree, 20)
	await _shot(scene, "ascent")
	# orbit view
	scene.session.spawn_in_orbit(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json"), "floe", 90000.0, 0.0)
	scene._clear_views()
	scene._cam_dist = 25.0
	scene._cam_pitch = -0.35
	await _frames(tree, 60)
	await _shot(scene, "orbit")
	scene.session.spawn_in_orbit(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "hopper.craft.json"), "krill", 12000.0, 0.0)
	scene._clear_views()
	scene._cam_pitch = -0.6
	await _frames(tree, 80)
	await _shot(scene, "krill_low")
	return {"status": "PASS", "log": "saved to " + ProjectSettings.globalize_path(DIR)}


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _shot(n: Node, name: String) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png(DIR + "flight_" + name + ".png")
