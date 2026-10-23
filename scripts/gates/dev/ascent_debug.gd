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


func run(_r: Node) -> Dictionary:
	var api := ControlAPI.new()
	api.handle({"cmd": "load_craft", "path": OS.get_environment("CRAFT") if OS.get_environment("CRAFT") != "" else "krill_lander"})
	api.handle({"cmd": "launch"})
	var ap := AscentAutopilot.new(api)
	var out := PackedStringArray()
	var last := -100.0
	for i in 50 * 60 * 10:
		if ap.step(0.1):
			break
		var t: Dictionary = api.session.telemetry()
		if t.t - last >= 10.0:
			last = t.t
			var nose := Vector3(t.nose[0], t.nose[1], t.nose[2])
			var tgt: PackedFloat64Array = api.session.core.sas_direction(api.session.active, "target")
			out.append("t %5.0f %-11s alt %5.1f km Ap %6.1f Pe %7.1f pitch %5.1f hdg %5.1f thr %.2f twr %.2f stg %d ang_err %.1f deg mass %.1f" % [
				t.t, ap.phase, t.altitude / 1000, (t.orbit.ap_alt if t.orbit.ap_alt != null else -1) / 1000, t.orbit.pe_alt / 1000, t.pitch_deg, t.heading_deg, t.throttle, t.twr, t.next_stage,
				rad_to_deg(nose.angle_to(Vector3(tgt[0], tgt[1], tgt[2]))), t.mass / 1000])
		api.handle({"cmd": "step", "ticks": 5})
	out.append_array(ap.log)
	return {"status": "PASS", "log": "\n".join(out)}
