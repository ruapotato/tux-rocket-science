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
## Dev: krill_by_hand.py's landing alone, fast: a lander (env CRAFT, a craft
## file) put in a 30 km orbit at Krill runs the script's landing part, with
## the last few hundred metres traced.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	var gate := preload("res://scripts/gates/test_by_hand.gd").new()
	var scene: Node3D = await gate._scene(tree, "orbiter", "pad")
	var s: FlightSession = scene.session
	s.spawn_in_orbit(FileAccess.get_file_as_string(OS.get_environment("CRAFT")), "krill", 30000.0)
	for i in 5:
		await tree.process_frame
	var comp: ShipComputer = ShipComputer.get_shared()
	scene.computer = comp
	comp.update(scene, s.telemetry())
	var src: String = gate.KRILL
	src = src.substr(src.find("# land on the lander"))
	comp.machine.write_file("/root/land.py", src)
	comp.log.clear()
	comp.machine.start("/root/land.py")
	var seen := comp.log_count
	for k in 200000:
		var tw := s.telemetry()
		var agl := float(tw.get("altitude_terrain", 1e9))
		var wake := comp.wake_at()
		if wake - s.time() > 30.0 and float(tw.get("thrust", 0.0)) == 0.0 and agl > 5000.0 and not comp.programs.running():
			s.warp_to(wake - 10.0)
		else:
			s.step_ticks(1 if agl < 300.0 else 5)
		comp.update(scene, s.telemetry())
		var fresh := mini(comp.log_count - seen, comp.log.size())
		for j in range(comp.log.size() - fresh, comp.log.size()):
			print("  said: ", comp.log[j])
		seen = comp.log_count
		tw = s.telemetry()
		if agl < 400.0 and k % 10 == 0:
			var legs := 0
			for p in comp._parts:
				legs += 1 if p.get("legs", false) else 0
			print("agl %.1f vs %.2f hs %.2f pitch %.1f thr %.2f thrust %.0f max %.0f lit %d legs %d contacts %d landed %s parts %d" % [agl,
				float(tw.vertical_speed), float(tw.horizontal_speed), float(tw.pitch_deg), float(tw.throttle), float(tw.thrust),
				float(tw.get("thrust_max", 0)), int(tw.engines_lit), legs, int(tw.get("contacts", 0)), tw.landed, int(tw.parts)])
		if tw.get("landed", false) and float(tw.surface_speed) < 0.2:
			break
		if not tw.get("alive", true):
			print("dead")
			break
	return {"status": "PASS", "log": "max impact %s" % s.telemetry().get("max_impact", "?")}
