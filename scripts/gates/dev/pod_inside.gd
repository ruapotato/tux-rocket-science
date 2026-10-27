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
## Dev (GPU): inside the Hopper's Pebble-1 on the pad (env CRAFT=file.craft.json
## for another), looking about; saves pod_inside_<k>.png.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			c.queue_free()
	await tree.process_frame
	var craft := OS.get_environment("CRAFT") if OS.get_environment("CRAFT") != "" else "hopper.craft.json"
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + craft)
	Sim.pending_site = "pad"
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 30:
		await tree.process_frame
	scene.hud.hide_help()
	scene._toggle_inside()
	for k in 3:
		scene._in_yaw = TAU * k / 3.0
		for i in 40:
			await tree.process_frame
		await RenderingServer.frame_post_draw
		scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/pod_inside_%d.png" % k)
	var t: Dictionary = scene.session.telemetry()
	log.append("walker %s; hint %s" % [t.get("walker", {}), scene.hud._hint.text])
	return {"status": "PASS", "log": "\n".join(log)}
