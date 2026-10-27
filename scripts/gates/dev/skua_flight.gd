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
## Dev (GPU): the Skua off the pad on its Hearth Torch; shots on the pad,
## climbing, and in the cabin under thrust.
extends RefCounted

const DIR := "user://dev_shots/"


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
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
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "skua.craft.json")
	Sim.pending_site = "pad"
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	scene._cam_dist = 30.0
	scene._cam_pitch = 0.15
	for i in 90:
		await tree.process_frame
	await _shot(scene, "skua_pad", log)
	scene._throttle = 1.0
	scene._stage()
	for i in 200:
		await tree.process_frame
	await _shot(scene, "skua_climb", log)
	var t := s.telemetry()
	log.append("%.0f m up at %.0f m/s, %.2f g" % [float(t.altitude_terrain), float(t.vertical_speed), float(t.g_force)])
	scene._toggle_inside()
	for i in 90:
		await tree.process_frame
	await _shot(scene, "skua_cabin", log)
	log.append("hint: " + scene.hud._hint.text)
	return {"status": "PASS", "log": "\n".join(log)}


func _shot(n: Node, name: String, log: PackedStringArray) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png(DIR + name + ".png")
	log.append("saved " + name)
