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
## Dev: which booster layouts hold straight up on stability SAS.
extends RefCounted


func run(_runner: Node) -> Dictionary:
	var log := PackedStringArray()
	var s := FlightSession.new()
	for v: String in ["core", "core+fins", "boost", "boost+nose", "boost+fins", "boost+nose+fins", "boost-noeng", "direct", "srb", "srb+nose"]:
		var m := CraftModel.new()
		m.place_root(CraftModel.held_single("pod_1"))
		var t := m.attach_stack(CraftModel.held_single("tank_l"), 0, "bottom")[0]
		m.attach_stack(CraftModel.held_single("engine_lifter"), t, "bottom")
		if v.contains("fins"):
			m.attach_surface(CraftModel.held_single("fin"), t, 0.0, -1.4, 3)
		var side: Array[int] = []
		if v.begins_with("boost"):
			var dr := m.attach_surface(CraftModel.held_single("decoupler_radial"), t, PI / 2, 0.0, 2)
			side = m.attach_stack(CraftModel.held_single("tank_m"), dr[0], "outer")
		elif v == "direct":
			side = m.attach_surface(CraftModel.held_single("tank_m"), t, PI / 2, 0.0, 2)
		elif v.begins_with("srb"):
			var dr := m.attach_surface(CraftModel.held_single("decoupler_radial"), t, PI / 2, 0.0, 2)
			side = m.attach_stack(CraftModel.held_single("srb"), dr[0], "outer")
		if not side.is_empty() and not v.contains("noeng") and not v.begins_with("srb"):
			m.attach_stack(CraftModel.held_single("engine_lifter"), side[0], "bottom")
		if v.contains("nose"):
			m.attach_stack(CraftModel.held_single("nosecone"), side[0], "top")
		s.launch(m.save_text())
		s.set_throttle(1.0)
		s.stage()
		var line: String = v + ":"
		for k in 12:
			s.step_ticks(50)
			var tt := s.telemetry()
			line += " %.0f" % tt.pitch_deg
		log.append(line)
	return {"status": "PASS", "log": "\n".join(log)}
