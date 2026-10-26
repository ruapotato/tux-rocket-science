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
## Dev: builds the stock Gull (a jet plane) and writes data/craft/gull.craft.json.
## Built nose up as everything in the hangar is: the belly is local -Z
## (wheels at angle PI), wings out to the sides, tail at the jet.
extends RefCounted


static func gull() -> CraftModel:
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	var tank := m.attach_stack(CraftModel.held_single("tank_l"), 0, "bottom")[0]
	var jet := m.attach_stack(CraftModel.held_single("jet_engine"), tank, "bottom")[0]
	m.attach_surface(CraftModel.held_single("wing"), tank, PI / 2, -0.55, 2)
	m.attach_surface(CraftModel.held_single("fin"), jet, PI / 2, 0.0, 2)
	m.attach_surface(CraftModel.held_single("fin"), jet, 0.0, 0.0, 1)
	m.attach_surface(CraftModel.held_single("wheel_nose"), tank, PI, 1.6, 1)
	# main gear splayed wide (a narrow track rocks), longer legs to match
	m.attach_surface(CraftModel.held_single("wheel_gear"), tank, PI - 0.8, -1.2, 1)
	m.attach_surface(CraftModel.held_single("wheel_gear"), tank, PI + 0.8, -1.2, 1)
	m.attach_stack(CraftModel.held_single("parachute"), 0, "top")
	m.craft_name = "Gull"
	return m


func run(_runner: Node) -> Dictionary:
	var m := gull()
	var text := m.save_text()
	var path := ProjectSettings.globalize_path("res://data/craft/gull.craft.json")
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_string(text)
	f.close()
	var st: Dictionary = m.stats()
	return {"status": "PASS", "log": "wrote %s: %d parts, %.2f t, jets %d, %.0f kN, TWR %.2f, %.0f s of fuel at sea level; stages %s" % [
		path, m.parts.size(), st.mass / 1000.0, st.jets, st.jet_thrust_sl / 1000.0, st.jet_twr_sl, st.jet_time_sl,
		(st.stages as Array).size()]}
