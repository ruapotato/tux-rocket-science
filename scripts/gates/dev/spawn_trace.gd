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
## Dev: trace a craft spawned in orbit (mode, body, radius) tick by tick.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var log := PackedStringArray()
	var mc := CraftModel.new()
	mc.place_root(CraftModel.held_single("probe_core"))
	var ct := mc.attach_stack(CraftModel.held_single("tank_l"), 0, "bottom")[0]
	var ce := mc.attach_surface(CraftModel.held_single("engine_small"), ct, 0.0, -0.8, 2)
	mc.rotate_part(ce[0], [0.0, 0.0, 1.0], deg_to_rad(30.0))
	var cs := FlightSession.new()
	cs.spawn_in_orbit(mc.save_text(), "floe", 400000.0)
	cs.set_throttle(1.0)
	cs.stage()
	for k in 600:
		cs.step_ticks(1)
		var t := cs.telemetry()
		if k % 50 == 0 or k > 270 or t.mode == "interstellar" or t.body != "floe":
			log.append("tick %d: mode %s body %s alt %s speed %s spin %s mass %s ec %s" % [k, t.mode, t.body, t.altitude, t.orbital_speed, t.spin, t.mass, t.get("ec")])
			if t.mode == "interstellar":
				break
	return {"status": "PASS", "log": "\n".join(log)}
