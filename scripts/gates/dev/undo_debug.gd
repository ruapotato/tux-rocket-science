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
	var g = load("res://scripts/gates/test_craft_roundtrip.gd").new()
	var m: CraftModel = g._build_orbiter()
	var before := m.save_text()
	m.attach_surface(CraftModel.held_single("fin"), 7, 0.5, 1.0, 6)
	m.undo()
	var after := m.save_text()
	var a := before.split("\n")
	var b := after.split("\n")
	var out := PackedStringArray(["lines %d vs %d" % [a.size(), b.size()]])
	for i in mini(a.size(), b.size()):
		if a[i] != b[i]:
			out.append("%d: %s | %s" % [i, a[i], b[i]])
			if out.size() > 8:
				break
	return {"status": "PASS", "log": "\n".join(out)}
