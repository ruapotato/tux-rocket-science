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
## Dev: an uncrewed probe through the flight scene: launch, fly, crash or
## land, recover, and the Rookery Heavy's first seconds.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("probe_core"))
	m.attach_stack(CraftModel.held_single("parachute"), 0, "top")
	var t := m.attach_stack(CraftModel.held_single("tank_s"), 0, "bottom")[0]
	m.attach_stack(CraftModel.held_single("engine_lifter"), t, "bottom")
	var scene := await _flight(tree, m.save_text())
	scene._throttle = 1.0
	scene._stage()
	for i in 30:
		await tree.process_frame
	scene.session.step_ticks(500)
	await tree.process_frame
	var tel: Dictionary = scene.session.telemetry()
	log.append("probe: alt %.0f m, %s, toast '%s', hint '%s'" % [tel.altitude, tel.situation, scene.hud._toast.text, scene.hud._hint.text])
	scene.session.deploy_chutes()
	var guard := 0
	while not scene.session.telemetry().landed and guard < 50 * 600:
		scene.session.step_ticks(50)
		guard += 50
	for i in 10:
		await tree.process_frame
	tel = scene.session.telemetry()
	log.append("down: landed %s alive %s, %d parts, toast '%s'" % [tel.landed, tel.alive, tel.parts, scene.hud._toast.text])
	if DisplayServer.get_name() != "headless":
		await RenderingServer.frame_post_draw
		DirAccess.make_dir_recursive_absolute("user://dev_shots")
		scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/probe_down.png")
	if scene.has_method("_recover"):
		scene._recover()
		for i in 5:
			await tree.process_frame
		log.append("recovered")
	scene = await _flight(tree, FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "rookery_heavy.craft.json"))
	scene._throttle = 1.0
	scene._stage()
	for i in 10:
		await tree.process_frame
	scene.session.step_ticks(150)
	for i in 30:
		await tree.process_frame
	if DisplayServer.get_name() != "headless":
		scene._cam_dist = 45.0
		scene._cam_pitch = 0.1
		for i in 10:
			await tree.process_frame
		await RenderingServer.frame_post_draw
		scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/heavy_liftoff.png")
	tel = scene.session.telemetry()
	log.append("heavy: alt %.0f m, %d lit, thrust %.0f kN" % [tel.altitude, tel.engines_lit, tel.thrust / 1000.0])
	return {"status": "PASS", "log": "\n".join(log)}


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
