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
## Dev: the playthrough's craft designs on the drawing board: each one's
## stages (delta-v, thrust to weight on the worlds it's for), mass, cost
## and the tech it needs. No flight.
extends RefCounted


func run(_runner: Node) -> Dictionary:
	var log := PackedStringArray()
	var Play: GDScript = load("res://scripts/gates/dev/playthrough.gd")
	for d in Play.DESIGNS:
		var m: CraftModel = Play.design(d)
		for body in ["floe", Play.DESIGNS[d]]:
			var st := m.stats(body)
			var rows := []
			for sg in st.get("stages", []):
				rows.append("[%s]" % ", ".join(PackedStringArray(sg.keys().map(func(k): return "%s %s" % [k, str(sg[k]) if not (sg[k] is float) else "%.2f" % sg[k]]))))
			log.append("%s on %s: %d parts, %.1f t (dry %.1f t), dv vac %.0f / sl %.0f m/s; stages %s" % [d, body, m.parts.size(),
				float(st.get("mass", 0)) / 1000.0, float(st.get("dry_mass", 0)) / 1000.0, float(st.get("dv_vac", 0)), float(st.get("dv_sl", 0)), " ".join(rows)])
		var ids := Career.craft_parts(m.save_text())
		var techs := {}
		for p in ids:
			techs[Career.node_of_part(p)] = true
		log.append("   costs %s, needs %s" % [Career.money(Career.craft_cost(ids)), techs.keys()])
	return {"status": "PASS", "log": "\n".join(log)}
