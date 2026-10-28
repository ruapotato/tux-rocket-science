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
## Dev (GPU): one of the editor's templates flown in real time, with
## screenshots of the flight screen. Env: TEMPLATE (a const in
## templates.gd), CRAFT, SHOTS (game seconds, comma-separated), SPEED
## (physics warp 1-4).
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var gate := preload("res://scripts/gates/test_by_hand.gd").new()
	var scene: Node3D = await gate._scene(tree, OS.get_environment("CRAFT"), "pad")
	var comp: ShipComputer = ShipComputer.get_shared()
	var s: FlightSession = scene.session
	var name := OS.get_environment("TEMPLATE")
	var tpl: Script = load("res://scripts/os/templates.gd")
	var text: String = tpl.get_script_constant_map()[name]
	comp.machine.write_file("/root/scripts/%s.py" % name.to_lower(), text)
	comp.machine.start("/root/scripts/%s.py" % name.to_lower())
	var shots := []
	for x in OS.get_environment("SHOTS").split(",", false):
		shots.append(float(x))
	if OS.get_environment("SPEED") != "":
		s.set_warp("physics", int(OS.get_environment("SPEED")) - 1)
	var log := PackedStringArray()
	var t0 := s.time()
	var k := 0
	var wall := Time.get_ticks_msec()
	while k < shots.size() and Time.get_ticks_msec() - wall < 240000:
		await tree.process_frame
		if OS.has_environment("TRACE") and Engine.get_frames_drawn() % 30 == 0:
			var vline := ""
			for row in str(comp.machine.scripts()).split("\n", false):
				var f := row.split("\t")
				if f.size() >= 4 and f[1] == "1":
					vline = str(comp.machine.read_file("/run/py/%s.vars" % f[0])).split("\n")[1]
			print("t=%.1f h=%.0f %s aim_err=%s turn=%s thrust=%.0f" % [s.time() - t0, float(s.telemetry().get("altitude_terrain", 0.0)), vline,
				comp.machine.api("get aim_error").strip_edges(), comp.machine.api("get turn_rate").strip_edges(), float(s.telemetry().get("thrust", 0.0))])
		if s.time() - t0 >= float(shots[k]):
			await RenderingServer.frame_post_draw
			scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/%s_%d.png" % [name.to_lower(), k])
			log.append("saved %s_%d at %.0f s: %.0f m up" % [name.to_lower(), k, s.time() - t0, float(s.telemetry().get("altitude_terrain", 0.0))])
			k += 1
	for l in comp.log.slice(maxi(0, comp.log.size() - 5)):
		log.append("  said: " + str(l))
	return {"status": "PASS", "log": "\n".join(log)}
