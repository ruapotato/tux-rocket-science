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
## Dev: the map's transfer-window line: an Orbiter in a 100 km orbit, the
## map focused on Kelp, then on Austra.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var g := Sim.game_session()
	var R := 700000.0
	var id: int = g.core.flight_spawn_state(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json"), "floe",
		PackedFloat64Array([R, 0, 0, 0, sqrt(3.528e12 / R), 0]))
	Sim.pending_vessel = id
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	scene._map.visible = true
	scene._map.focus_vessel = false
	for b in [3, 0]:
		scene._map.focus_body = b
		scene._map._dist = 5000.0 if b == 0 else 30.0
		for i in 10:
			await tree.process_frame
		log.append("focus %d:\n%s" % [b, scene._map._panel.get_parsed_text()])
		if DisplayServer.get_name() != "headless":
			await RenderingServer.frame_post_draw
			scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/map_transfer_%d.png" % b)
	scene._map.focus_body = 3
	for i in 3:
		await tree.process_frame
	var kinfo: Dictionary = scene._map._bodies[3].info
	var direct: PackedFloat64Array = scene.session.core.plan_transfer(scene.session.active, 3, float(kinfo.atmosphere_height) + 40000.0, false)
	var t0 := Time.get_ticks_msec()
	var f0 := Engine.get_process_frames()
	await scene._map._plan()
	var frames := Engine.get_process_frames() - f0
	log.append("plan button (%d ms, %d frames meanwhile): %s; nodes %s; direct %s" % [Time.get_ticks_msec() - t0, frames, scene._map._plan_note, scene.session.nodes, direct])
	var ok: bool = frames > 10 and scene.session.nodes.size() == 1 and absf(float(scene.session.nodes[0][1]) - direct[1]) < 1e-9
	for i in 10:
		await tree.process_frame
	log.append(scene._map._panel.get_parsed_text())
	if DisplayServer.get_name() != "headless":
		await RenderingServer.frame_post_draw
		scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/map_transfer_planned.png")
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(log)}
