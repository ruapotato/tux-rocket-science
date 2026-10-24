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
## Dev: does a rocket without fins tumble in thick fast air, and one with
## fins weathervane? Climbs the Orbiter (with / without fins) to high
## dynamic pressure, kicks it 6 degrees off the airflow with the controls
## let go, and watches the angle of attack.
extends RefCounted


func run(_runner: Node) -> Dictionary:
	var log := PackedStringArray()
	var text := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	for fins in [true, false]:
		var craft: Dictionary = JSON.parse_string(text)
		if not fins:
			craft.parts = craft.parts.filter(func(p): return p.part != "fin")
		for sas in ["off", "surface_prograde"]:
			var api := ControlAPI.new()
			api.session.launch(JSON.stringify(craft), "floe")
			var ap := AscentAutopilot.new(api)
			for i in 3000:
				ap.step(0.1)
				api.handle({"cmd": "step", "ticks": 5})
				var t: Dictionary = api.handle({"cmd": "query"}).telemetry
				if not t.get("alive", false) or t.altitude > 9000.0:
					break
			var t0: Dictionary = api.handle({"cmd": "query"}).telemetry
			if not t0.get("alive", false):
				log.append("%s: lost on the climb: %s" % ["fins" if fins else "no fins", "\n    ".join(ap.log)])
				continue
			var s := api.session
			# kick: point 6 degrees off the airflow, then let go (or hold prograde)
			var sv := Vector3(t0.surface_velocity[0], t0.surface_velocity[1], t0.surface_velocity[2]).normalized()
			var side := sv.cross(Vector3(t0.up[0], t0.up[1], t0.up[2])).normalized()
			var kick := sv.rotated(side, deg_to_rad(6.0))
			api.handle({"cmd": "sas", "mode": "target", "target": [kick.x, kick.y, kick.z]})
			for i in 150:
				api.handle({"cmd": "step", "ticks": 2})
			api.handle({"cmd": "sas", "mode": sas})
			var worst := 0.0
			var series := PackedStringArray()
			for k in 40:
				api.handle({"cmd": "step", "ticks": 10})
				var t: Dictionary = api.handle({"cmd": "query"}).telemetry
				var nose := Vector3(t.nose[0], t.nose[1], t.nose[2])
				var flow := Vector3(t.surface_velocity[0], t.surface_velocity[1], t.surface_velocity[2]).normalized()
				var aoa := rad_to_deg(nose.angle_to(flow))
				worst = maxf(worst, aoa)
				if k % 5 == 4:
					series.append("%.0f" % aoa)
			var t1: Dictionary = api.handle({"cmd": "query"}).telemetry
			log.append("%-9s SAS %-16s q %.1f kPa at %.1f km: AoA over 8 s %s deg (worst %.0f)" % [
				"fins" if fins else "no fins", sas, t0.dynamic_pressure / 1000.0, t0.altitude / 1000.0, " ".join(series), worst])
	return {"status": "PASS", "log": "\n".join(log)}
