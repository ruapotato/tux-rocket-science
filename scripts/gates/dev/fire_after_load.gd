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
## Dev: do a landed ship's lasers fire on a lock, and after the world is
## saved and loaded again? The Warden Lander by the dune stalkers.
extends RefCounted

const Guard := preload("res://scripts/gates/test_guard.gd")
const Templates := preload("res://scripts/os/templates.gd")


func _fire(s: FlightSession, ship: int, log: PackedStringArray, label: String) -> void:
	var core := s.core
	var target := {}
	for th in JSON.parse_string(core.things_near(ship, 600.0)):
		if str(th.what) == "animal":
			target = th
			break
	if target.is_empty():
		log.append("%s: nothing to fire at" % label)
		return
	var n := core.lock_on(ship, int(target.kind), int(target.id), int(target.serial))
	var beams := 0
	var hits := {}
	for k in 150:
		s.step_ticks(1)
		for p in JSON.parse_string(core.vessel_part_states(ship)):
			if p.get("firing", false):
				beams += 1
				hits[str(p.get("beam_hit", "?"))] = int(hits.get(str(p.get("beam_hit", "?")), 0)) + 1
	var t := s.telemetry()
	log.append("%s: lock_on said %d lasers; %s %.0f m off (in reach %s): %d beam ticks in 3 s %s; ec %.0f of %.0f, lock %s" % [label, n, str(target.species),
		float(target.distance), str(target.get("in_reach")), beams, str(hits), float(t.get("ec", 0)), float(t.get("ec_capacity", 0)), str(t.get("lock", "-"))])
	core.lock_on(ship, 0, 0, 0)


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var gate = preload("res://scripts/gates/test_by_hand.gd").new()
	var scene: Node3D = await gate._scene(tree, "warden_lander", "pad")
	var s: FlightSession = scene.session
	var g = Guard.new()
	var ship: int = g._set_down(scene, "saffron", "dune_stalkers", 1, 200.0)
	_fire(s, ship, log, "fresh")
	var text: String = s.core.world_save()
	var serial := s.core.vessel_serial(ship)
	s.core.world_load(text)
	s.switch_to(ship)
	s.step_ticks(5)
	log.append("loaded: the ship %s (serial %d, was %d)" % [s.telemetry().get("name", "?"), s.core.vessel_serial(ship), serial])
	_fire(s, ship, log, "after a save and load")
	scene.queue_free()
	await tree.process_frame
	return {"status": "PASS", "log": "\n".join(log)}
