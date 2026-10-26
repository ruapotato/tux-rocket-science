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
## --test-kelp: the stock Krill Lander flies to Kelp blind through the
## control interface: ascent, the planner's transfer burn at the window,
## out of Floe's sphere round Austra, a correction a week out, into Kelp's
## sphere, and the capsule alone, shield first, down through the thick air
## on its parachute, under 10 g with no part past 90% of its heat headroom.
extends RefCounted


func run(_runner: Node) -> Dictionary:
	var api := ControlAPI.new()
	var r := api.handle({"cmd": "load_craft", "path": "krill_lander"})
	if not r.ok:
		return {"status": "FAIL", "log": r.error}
	api.handle({"cmd": "launch", "body": "floe"})
	var m := KelpMission.new(api)
	var ok := m.fly_to_kelp()
	m.log.append("SOI path: " + " -> ".join(m.soi_path) + ("   landed on Kelp" if m.landed_on_kelp else "   (no Kelp landing)"))
	var gentle := m.peak_g < 10.0 and m.peak_heat < 0.9
	m.log.append("the way down: entry at %.0f m/s, peak %.1f g (limit 10), peak heat %.0f%% (limit 90%%)" % [m.entry_speed, m.peak_g, 100.0 * m.peak_heat])
	ok = ok and gentle
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(m.log)}
