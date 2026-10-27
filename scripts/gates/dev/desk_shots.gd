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
## Dev (GPU): the Orbiter on the pad with the ship's computer up (F2);
## shots of the desktop with the Flight app, a terminal with `ship` in it,
## and a game. Saves desk_<k>.png.
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
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	Sim.pending_site = "pad"
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 10:
		await tree.process_frame
	scene.hud.hide_help()
	print("MARK before desk")
	var t0 := Time.get_ticks_msec()
	scene._toggle_desk()
	log.append("desktop up in %d ms" % (Time.get_ticks_msec() - t0))
	print("MARK desk up")
	for i in 30:
		await tree.process_frame
	print("MARK 30 frames")
	var de: Control = scene._de
	log.append("holder %s, desk %s, wall %s, panel %s" % [scene._desk_box.size, de.size, de.wall.size, de.panel.size])
	await _shot(scene, "desk_flight", log)
	de._launch("term")
	for i in 5:
		await tree.process_frame
	var win: Control = de._find("terminal")
	var term: Node = null
	if win:
		for n in win.find_children("*", "", true, false):
			if n.has_method("feed"):
				term = n
	if term:
		for line in ["ship", "ship help", "ship throttle 0.5", "cat /dev/ship/throttle_pct"]:
			term.call("feed", line + "\n")
			for i in 3:
				await tree.process_frame
	for i in 20:
		await tree.process_frame
	await _shot(scene, "desk_term", log)
	log.append("throttle now %.2f (asked 0.5 from the terminal)" % float(scene.session.telemetry().throttle))
	de._launch("g2048")
	for i in 20:
		await tree.process_frame
	await _shot(scene, "desk_game", log)
	return {"status": "PASS", "log": "\n".join(log)}


func _shot(n: Node, name: String, log: PackedStringArray) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png("user://dev_shots/%s.png" % name)
	log.append("saved " + name)
