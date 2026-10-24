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
## Dev: rebuilds a playtester's rocket in the editor model and prints the
## staging it gets.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var log := PackedStringArray()
	for variant in [["parachute", "pod_1", "reaction_wheel", "decoupler_stack", "tank_m", "engine_vac", "decoupler_stack", "tank_m", "tank_m", "srb"],
			["parachute", "pod_1", "reaction_wheel", "decoupler_stack", "tank_m", "decoupler_stack", "tank_m", "tank_m", "srb"]]:
		var m := CraftModel.new()
		m.place_root(CraftModel.held_single("pod_1"))
		m.attach_stack(CraftModel.held_single("parachute"), 0, "top")
		var last := 0
		for k in range(2, variant.size()):
			last = m.attach_stack(CraftModel.held_single(variant[k]), last, "bottom")[0]
		log.append(" / ".join(variant))
		for s in range(m.stage_count() - 1, -1, -1):
			var names := []
			for i in m.parts.size():
				if m.parts[i].stage == s:
					names.append(m.parts[i].part)
			log.append("  stage %d: %s" % [s, ", ".join(names)])
	# side boosters straight on the tank, and an engine stacked on a side engine
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	var tank := m.attach_stack(CraftModel.held_single("tank_l"), 0, "bottom")[0]
	m.attach_stack(CraftModel.held_single("srb"), tank, "bottom")
	var sides := m.attach_surface(CraftModel.held_single("srb"), tank, 0.0, 0.0, 2)
	var vac := m.attach_surface(CraftModel.held_single("engine_small"), tank, PI / 2, 0.0, 1)
	m.attach_stack(CraftModel.held_single("engine_small"), vac[0], "top")
	for s in range(m.stage_count() - 1, -1, -1):
		var names := []
		for i in m.parts.size():
			if m.parts[i].stage == s:
				names.append("%d:%s" % [i, m.parts[i].part])
		log.append("  stage %d: %s" % [s, ", ".join(names)])
	var s := FlightSession.new()
	s.launch(m.save_text(), "floe")
	s.set_throttle(1.0)
	s.stage()
	s.step_ticks(50)
	var t := s.telemetry()
	log.append("after staging: thrust %.0f kN, max %.0f kN, alt %.0f m" % [t.get("thrust", 0.0) / 1000.0, t.get("thrust_max", 0.0) / 1000.0, t.altitude])
	var feed: PackedFloat64Array = s.core.vessel_render(s.active, s.active)
	var ids: Array = JSON.parse_string(s.core.vessel_parts(s.active))
	for k in feed.size() / VesselView.RPP:
		log.append("  part %d %s thr %.2f" % [k, ids[k] if k < ids.size() else "?", feed[k * VesselView.RPP + 8]])
	if DisplayServer.get_name() != "headless":
		var tree := runner.get_tree()
		tree.paused = false
		if tree.current_scene:
			tree.current_scene.queue_free()
		Sim.pending_craft_text = m.save_text()
		var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
		tree.root.add_child(scene)
		for i in 20:
			await tree.process_frame
		scene.hud.hide_help()
		scene._throttle = 1.0
		scene._stage()
		scene._cam_yaw = 0.6
		scene._cam_pitch = 0.1
		scene._cam_dist = 30.0
		scene._cam_pitch = -0.05
		for i in 25:
			await tree.process_frame
		await RenderingServer.frame_post_draw
		scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/user_boosters.png")
		log.append("saved user_boosters")
	return {"status": "PASS", "log": "\n".join(log)}
