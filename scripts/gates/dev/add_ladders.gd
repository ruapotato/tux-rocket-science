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
## Dev: fits a Waddle Ladder to the stock craft whose pods stand out of a
## penguin's reach from the ground: on the first tank under the pod, its
## top level with the tank's, half-way between two fins. Rewrites the
## craft files (canonical text) and logs what it did.
extends RefCounted

const CRAFT := ["krill_lander"]


func run(_runner: Node) -> Dictionary:
	var log := PackedStringArray()
	for n in CRAFT:
		var path := ProjectSettings.globalize_path(Sim.STOCK_CRAFT_DIR + n + ".craft.json")
		var m := CraftModel.new()
		m.load_text(FileAccess.get_file_as_string(path))
		var has := false
		for p in m.parts:
			has = has or p.part == "ladder"
		if has:
			log.append("%s: already has a ladder" % n)
			continue
		if n == "krill_lander":
			# between two legs on the landing tank, its foot just clear of the ground
			var lt := -1
			for i in m.parts.size():
				if m.parts[i].part == "tank_m":
					lt = i
			var lh: float = Sim.part("tank_m").shape.height
			m.attach_surface(CraftModel.held_single("ladder"), lt, 0.0, lh * 0.5 - Sim.part("ladder").shape.height * 0.5 + 0.8, 1)
			var lf := FileAccess.open(path, FileAccess.WRITE)
			lf.store_string(m.save_text())
			lf.close()
			log.append("%s: ladder on the landing tank" % n)
			continue
		# the first tank on the chain down from the pod
		var pod := -1
		for i in m.parts.size():
			if int(Sim.part(m.parts[i].part).get("crew", 0)) > 0:
				pod = i
				break
		var tank := -1
		var cur := pod
		for step in 8:
			var next := -1
			for i in m.parts.size():
				if int(m.parts[i].parent) == cur and m.parts[i].node == "top":
					next = i
			if next < 0:
				for i in m.parts.size():
					if i == int(m.parts[cur].parent):
						next = i
			if next < 0:
				break
			cur = next
			if Sim.part(m.parts[cur].part).has("fuel_liquid"):
				tank = cur
				break
		if tank < 0:
			log.append("%s: no tank under the pod" % n)
			continue
		var h: float = Sim.part(m.parts[tank].part).shape.height
		var lad: float = Sim.part("ladder").shape.height
		var placed := m.attach_surface(CraftModel.held_single("ladder"), tank, PI / 3, h * 0.5 - lad * 0.5, 1)
		if placed.is_empty():
			log.append("%s: ladder didn't fit" % n)
			continue
		var f := FileAccess.open(path, FileAccess.WRITE)
		f.store_string(m.save_text())
		f.close()
		log.append("%s: ladder on %s (%d)" % [n, m.parts[tank].part, tank])
	return {"status": "PASS", "log": "\n".join(log)}
