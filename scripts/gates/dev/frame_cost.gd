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
## Dev: what the flight scene's frame costs (scripts only, from _process)
## in orbit with a target set: flight view, then the map.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var g := Sim.game_session()
	var R1 := 720000.0
	var st: int = g.core.flight_spawn_state(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "igloo_station.craft.json"), "floe",
		PackedFloat64Array([R1, 0, 0, 0, sqrt(3.528e12 / R1), 0]))
	var R2 := 700000.0
	var ch: int = g.core.flight_spawn_state(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "puffin_ferry.craft.json"), "floe",
		PackedFloat64Array([0, R2, 0, -sqrt(3.528e12 / R2), 0, 0]))
	Sim.pending_vessel = ch
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 10:
		await tree.process_frame
	scene.hud.hide_help()
	scene.session.target = st
	var s: FlightSession = scene.session
	for what in ["closest_approach", "target_info", "telemetry_raw", "vessel_list", "render_feed", "hatch"]:
		var t0 := Time.get_ticks_usec()
		for i in 20:
			match what:
				"closest_approach": s.closest_approach()
				"target_info": s.target_info()
				"telemetry_raw": JSON.parse_string(s.core.telemetry(s.active))
				"vessel_list": JSON.parse_string(s.core.vessel_list())
				"render_feed": s.core.vessel_render(s.active, s.active)
				"hatch": scene._hatch_near()
		log.append("  %s: %.3f ms" % [what, (Time.get_ticks_usec() - t0) / 20000.0])
	var parts := {"controls": 0.0, "advance": 0.0, "render": 0.0, "missions": 0.0}
	for i in 60:
		var a := Time.get_ticks_usec()
		scene._controls(1.0 / 60.0)
		var b := Time.get_ticks_usec()
		s.advance(1.0 / 60.0)
		var c := Time.get_ticks_usec()
		scene._render(1.0 / 60.0)
		var d := Time.get_ticks_usec()
		scene._check_missions()
		var e := Time.get_ticks_usec()
		parts.controls += (b - a) / 60000.0
		parts.advance += (c - b) / 60000.0
		parts.render += (d - c) / 60000.0
		parts.missions += (e - d) / 60000.0
		await tree.process_frame
	log.append("  per frame: %s" % [parts])
	for mode in ["flight", "map"]:
		scene._map.visible = mode == "map"
		var worst := 0.0
		var total := 0.0
		for i in 120:
			var t0 := Time.get_ticks_usec()
			scene._process(1.0 / 60.0)
			var dt := (Time.get_ticks_usec() - t0) / 1000.0
			worst = maxf(worst, dt)
			total += dt
			await tree.process_frame
		log.append("%s: scripts %.2f ms a frame on average, worst %.2f ms" % [mode, total / 120.0, worst])
	return {"status": "PASS", "log": "\n".join(log)}
