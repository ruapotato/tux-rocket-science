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
## Dev: builds the stock Waddle Ark and writes data/craft/waddle_ark.craft.json:
## a ship to live in on the way to the stars. Nose to tail: the command
## capsule, a hab, a junction with a hab out each side, the spin ring, the
## wide hab, a crew spine, the Hearth Core and the Stillwater Drive, and a
## tank and heavy engine for pushing (and weight) in the ordinary way.
extends RefCounted


static func ark() -> CraftModel:
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_3"))
	m.attach_stack(CraftModel.held_single("dock_port"), 0, "top")
	var ad := m.attach_stack(CraftModel.held_single("adapter_21"), 0, "bottom")[0]
	var h1 := m.attach_stack(CraftModel.held_single("hab_burrow"), ad, "bottom")[0]
	var j := m.attach_stack(CraftModel.held_single("junction"), h1, "bottom")[0]
	m.attach_stack(CraftModel.held_single("hab_burrow"), j, "east")
	m.attach_stack(CraftModel.held_single("hab_burrow"), j, "west")
	var ring := m.attach_stack(CraftModel.held_single("spin_ring"), j, "bottom")[0]
	var wide := m.attach_stack(CraftModel.held_single("hab_rookery"), ring, "bottom")[0]
	var sp := m.attach_stack(CraftModel.held_single("spine"), wide, "bottom")[0]
	var core := m.attach_stack(CraftModel.held_single("hearth_core"), sp, "bottom")[0]
	var drive := m.attach_stack(CraftModel.held_single("stillwater_drive"), core, "bottom")[0]
	var tank := m.attach_stack(CraftModel.held_single("tank_xl"), drive, "bottom")[0]
	m.attach_stack(CraftModel.held_single("engine_big"), tank, "bottom")
	m.craft_name = "Waddle Ark"
	m.description = "A ship to live in: habs to float about in (Enter), a 32 m spin ring that weighs you like home once spun up (Home), and the Stillwater Drive for the stars. Launch it from the Orbital Yard."
	return m


func run(_runner: Node) -> Dictionary:
	var m := ark()
	var text := m.save_text()
	var path := ProjectSettings.globalize_path("res://data/craft/waddle_ark.craft.json")
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_string(text)
	f.close()
	var st: Dictionary = m.stats()
	return {"status": "PASS", "log": "wrote %s: %d parts, %.2f t, stages %d" % [path, m.parts.size(), st.mass / 1000.0, (st.stages as Array).size()]}
