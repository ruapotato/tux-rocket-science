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
## --test-discovery: a landmark is found by going there. Set down on Mount
## Shiver's flank, the first look says so ("Discovered: Mount Shiver",
## with its words) and pays research, once; a script there reads
## ship.landmark as its name; a pod far from any reads None; and the
## landmark stays found across a save.
extends RefCounted

var _log := PackedStringArray()
var _failed := 0
var _gate := preload("res://scripts/gates/test_by_hand.gd").new()


func _check(ok: bool, msg: String) -> void:
	_log.append(("ok   " if ok else "FAIL ") + msg)
	if not ok:
		_failed += 1


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	DirAccess.remove_absolute(ProjectSettings.globalize_path(Sim.save_dir + "career.json"))
	Career.load_from(Sim.save_dir + "career.json", "sandbox")
	for m in Career.missions_def.missions:
		Career.done.append(m.id)   # (no contract paying out over the landmark's toast)
	var pod := CraftModel.new()
	pod.place_root(CraftModel.held_single("pod_1"))
	pod.attach_stack(CraftModel.held_single("tank_s"), 0, "bottom")
	var scene: Node3D = await _gate._scene(tree, "orbiter", "pad")
	var s: FlightSession = scene.session
	var core: TrsSim = s.core
	var lm: Dictionary = {}
	for l in Sim.landmarks("floe"):
		if l.id == "mount_shiver":
			lm = l
	_check(not lm.is_empty() and str(lm.get("text", "")) != "", "Floe has Mount Shiver, with words about it")

	# far from any landmark: nothing below
	var far: int = core.flight_place_landed(pod.save_text(), "floe", -60.0, -150.0)
	s.switch_to(far)
	await _frames(tree, 3)
	scene._check_missions()
	var t := s.telemetry()
	_check(Career.landmark_below(t).is_empty() and Career.sighted.is_empty(), "far from them, no landmark below (%s)" % str(Career.landmark_below(t).get("name", "none")))

	# on its flank
	var said := []
	var catch := func(e: Dictionary): said.append(e)
	Career.landmark_sighted.connect(catch)
	var r0 := Career.research
	var near: int = core.flight_place_landed(pod.save_text(), "floe", float(lm.lat_deg) - 0.8, float(lm.lon_deg))
	s.switch_to(near)
	await _frames(tree, 3)
	scene._check_missions()
	t = s.telemetry()
	_check(said.size() == 1 and str(said[0].name) == "Mount Shiver" and "floe/mount_shiver" in Career.sighted
		and absf(Career.research - r0 - Career.LANDMARK_RESEARCH) < 1e-6,
		"set down on its flank (%.0f m up): discovered, +%.0f research" % [float(t.get("altitude", 0.0)), Career.research - r0])
	_check(str(scene.hud._toast.text).begins_with("Discovered: Mount Shiver.") and str(scene.hud._toast.text).contains(str(lm.text).left(20)),
		"the flight screen says so: %s" % str(scene.hud._toast.text).left(90))
	# once: looking again finds nothing new
	scene._check_missions()
	await _frames(tree, 2)
	scene._check_missions()
	_check(said.size() == 1 and Career.sighted.size() == 1, "found once, not again")
	Career.landmark_sighted.disconnect(catch)

	# a script there knows where it is
	var comp: ShipComputer = ShipComputer.get_shared()
	scene.computer = comp
	comp.update(scene, s.telemetry())
	comp.machine.write_file("/root/where.py", "print('over', ship.landmark, 'in', ship.biome)\n")
	var out := str(comp.sh("py /root/where.py"))
	_check(out.contains("over Mount Shiver in "), "a script reads ship.landmark: %s" % out.strip_edges())

	# kept across a save
	Career.load_from(Career.path, "sandbox")
	_check("floe/mount_shiver" in Career.sighted, "still found after the career is read back from its file")

	DirAccess.remove_absolute(ProjectSettings.globalize_path(Sim.save_dir + "career.json"))
	Career.load_from(Sim.save_dir + "career.json", "sandbox")
	scene.queue_free()
	await tree.process_frame
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	Sim.save_game()
	return {"status": "FAIL" if _failed else "PASS", "log": "\n".join(_log)}


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame
