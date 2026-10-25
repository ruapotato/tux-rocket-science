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
	var text := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "krill_lander.craft.json")
	Sim.pending_craft_text = text
	var flight: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(flight)
	await tree.process_frame
	var s: FlightSession = flight.session
	s.spawn_in_orbit(text, "floe", 90000.0)
	s.core.vessel_set_mode(s.active, "rails")
	s.warp_to(s.time() + s.telemetry().orbit.period * 0.45)
	flight._clear_views()
	flight._assign_crew()
	flight._cam_dist = 45.0
	flight._cam_pitch = 0.25
	flight._cam_yaw = 1.0
	s.set_throttle(0.0)
	for k in 3:  # ignite (SRBs burn), drop SRBs, drop core
		s.stage()
		for i in 60:
			s.step_ticks(1)
			await tree.process_frame
	for i in 20:
		await tree.process_frame
	await RenderingServer.frame_post_draw
	tree.root.get_texture().get_image().save_png("user://dev_shots/separation.png")
	while s.telemetry().next_stage >= 1:
		s.stage()  # light the transfer engine, drop the transfer stage, light the lander
	s.set_throttle(0.3)  # nudge away from the spent stage
	for i in 150:
		s.step_ticks(4)
		await tree.process_frame
	s.set_throttle(0.0)
	flight._cam_dist = 9.0
	flight._cam_pitch = 0.05
	s.set_legs(true)
	for i in 120:
		await tree.process_frame
	await RenderingServer.frame_post_draw
	tree.root.get_texture().get_image().save_png("user://dev_shots/legs_out.png")
	var out := PackedStringArray()
	var vv = flight._views.get(s.active)
	var feed: PackedFloat64Array = s.core.vessel_render(s.active, s.active)
	for k in feed.size() / VesselView.RPP:
		if feed[k * VesselView.RPP + 10] > 0.0:
			out.append("feed uid %d legs %.1f" % [int(feed[k * VesselView.RPP]), feed[k * VesselView.RPP + 10]])
	for uid in vv._parts:
		var e: Dictionary = vv._parts[uid]
		if Sim.part(e.part).has("leg"):
			out.append("view uid %d legs %.2f" % [uid, e.legs])
	return {"status": "PASS", "log": "\n".join(out)}
