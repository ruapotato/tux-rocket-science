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
## liftoff, and return to Floe's SOI.
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
	ok = ok and m.landed_home
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(m.log)}
