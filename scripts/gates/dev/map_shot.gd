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
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "krill_lander.craft.json")
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await tree.process_frame
	var s: FlightSession = scene.session
	s.spawn_in_orbit(Sim.pending_craft_text, "floe", 82000.0)
	# light the transfer stage so burn time estimates work
	var api := ControlAPI.new(s)
	var m := MoonMission.new(api)
	var node := m.plan_transfer()
	s.nodes = [node]
	scene._map.visible = true
	scene._map.focus_body = 1
	scene._map._dist = 260.0
	scene._map._pitch = 1.1
	scene._map.selected_node = 0
	scene.hud.toast("", 0.01)
	for i in 30:
		await tree.process_frame
	await RenderingServer.frame_post_draw
	scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/map_transfer.png")
	scene._map._dist = 12.0
	for i in 10:
		await tree.process_frame
	await RenderingServer.frame_post_draw
	scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/map_node.png")
	for b in scene._map._bodies:
		m.log.append("%s at %s scale %s" % [b.info.id, b.node.position, b.node.scale])
	return {"status": "PASS", "log": "\n".join(m.log)}
