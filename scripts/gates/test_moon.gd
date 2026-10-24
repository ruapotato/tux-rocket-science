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
## --test-moon: the stock Krill Lander flies a full mission blind through the
## control interface: ascent, transfer node, SOI change, capture, landing,
## liftoff, the return burn, and a shield-first entry and chute landing at
## home, under 10 g with no part past 80% of its heat headroom.
extends RefCounted


func run(_runner: Node) -> Dictionary:
	var api := ControlAPI.new()
	var r := api.handle({"cmd": "load_craft", "path": "krill_lander"})
	if not r.ok:
		return {"status": "FAIL", "log": r.error}
	api.handle({"cmd": "launch", "body": "floe"})
	var m := MoonMission.new(api)
	var ok := m.fly()
	m.log.append("SOI path: " + " -> ".join(m.soi_path) + ("   landed on Krill" if m.landed_on_krill else "   (no Krill landing)")
		+ ("   landed back on Floe" if m.landed_home else "   (did not land back on Floe)"))
	# a survivable ride home: shield-first entry, no burnt-off parts
	var gentle := m.peak_g < 10.0 and m.peak_heat < 0.8
	m.log.append("ride home: peak %.1f g (limit 10), peak heat %.0f%% of headroom (limit 80%%)" % [m.peak_g, 100.0 * m.peak_heat])
	ok = ok and m.landed_home and gentle
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(m.log)}
