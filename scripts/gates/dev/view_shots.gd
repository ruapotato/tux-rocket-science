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
## Dev (GPU): the ~ views. The Orbiter on the pad: the rocket view; ~ to
## the penguin's view in the capsule, its computer's screen lit on the
## console; a click on that screen (or ~) to the full-screen desktop; ~
## back to the rocket. The same in the Waddle Ark's hab (a screen on the
## wall). Saves view_<k>.png.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	for craft in ["orbiter", "waddle_ark"]:
		for v in Sim.vessels():
			Sim.game.core.vessel_remove(int(v.id))
		Sim.seats = {}
		for c in tree.root.get_children():
			if c is Node3D or (c is Control and c.name != "GateRunner"):
				c.queue_free()
		await tree.process_frame
		Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + craft + ".craft.json")
		Sim.pending_site = "pad" if craft == "orbiter" else "orbit"
		var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
		tree.root.add_child(scene)
		for i in 10:
			await tree.process_frame
		scene.hud.hide_help()
		await _shot(scene, "view_%s_rocket" % craft, log)
		scene._cycle_view()
		for i in 40:
			await tree.process_frame
		log.append("%s: inside %s, screen %s, desktop live %s" % [craft, scene._inside != null,
			scene._inside != null and scene._inside.screen != null, scene._desk_vp != null and scene._desk_vp.render_target_update_mode == SubViewport.UPDATE_ALWAYS])
		await _shot(scene, "view_%s_penguin" % craft, log)
		# look at the screen and click it
		if scene._inside and scene._inside.screen:
			var cam: Camera3D = scene._cam
			scene._in_looked = true
			var sp: Vector3 = scene._inside.screen.global_position
			for i in 3:
				cam.look_at(sp, cam.global_basis.y)
				await tree.process_frame
			var at := cam.unproject_position(sp)
			log.append("screen at %s on the view; hit %s" % [at, scene._inside.screen_hit(cam, at)])
			var ev := InputEventMouseButton.new()
			ev.button_index = MOUSE_BUTTON_LEFT
			ev.pressed = true
			ev.position = at
			scene._unhandled_input(ev)
		else:
			scene._cycle_view()
		for i in 20:
			await tree.process_frame
		log.append("desktop open %s" % scene._desk_open)
		await _shot(scene, "view_%s_desk" % craft, log)
		scene._cycle_view()
		for i in 10:
			await tree.process_frame
		log.append("back: desk %s, inside %s" % [scene._desk_open, scene._inside != null])
		await _shot(scene, "view_%s_back" % craft, log)
		scene.queue_free()
		await tree.process_frame
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	return {"status": "PASS", "log": "\n".join(log)}


func _shot(n: Node, name: String, log: PackedStringArray) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png("user://dev_shots/%s.png" % name)
	log.append("saved " + name)
