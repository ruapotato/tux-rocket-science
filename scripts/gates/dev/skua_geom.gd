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
## Dev: a playthrough design's parts, where each sits (craft axes, m) and
## its size, and the craft's centre of mass: CRAFT (star_skua).
extends RefCounted


func run(_runner: Node) -> Dictionary:
	var log := PackedStringArray()
	var Play: GDScript = load("res://scripts/gates/dev/playthrough.gd")
	var name := OS.get_environment("CRAFT") if OS.get_environment("CRAFT") != "" else "star_skua"
	var m: CraftModel = Play.design(name)
	var c: Dictionary = JSON.parse_string(m.save_text())
	for i in c.parts.size():
		var p: Dictionary = c.parts[i]
		log.append("%2d %-16s parent %2d pos %s size %s" % [i, p.part, int(p.get("parent", -1)), str(p.get("pos", "?")), str(p.get("size", "-"))])
	var st := m.stats("floe")
	log.append("mass %.1f t, keys %s" % [float(st.get("mass", 0)) / 1000.0, str(st.keys())])
	log.append("com %s, plume hits %s, unfed %s" % [str(st.get("com", st.get("center_of_mass", "?"))), str(st.get("plume_hits", [])), str(st.get("unfed_engines", []))])
	return {"status": "PASS", "log": "\n".join(log)}
