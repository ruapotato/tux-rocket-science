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
## --test-world: the persistent game through the real flight scene. A
## launch joins the saved world with its crew seated; leaving keeps it
## flying and saves; a restart loads it back identically; the next launch
## clears the pad (crew home); reverting to the hangar undoes a launch;
## flying a craft left in orbit resumes it; a crewed wreck dropped from the
## world loses its crew; and you can't walk away from a craft in the air.
extends RefCounted

var _log := PackedStringArray()
var _failed := 0


func _check(ok: bool, msg: String) -> void:
	_log.append(("ok   " if ok else "FAIL ") + msg)
	if not ok:
		_failed += 1


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var orbiter := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	var hopper := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "hopper.craft.json")

	# 1. launch the Orbiter into the (empty, sandboxed) game world
	var scene := await _flight(tree, orbiter)
	var id: int = scene.session.active
	var crew: Array = Sim.crew_of(id)
	var flying: bool = crew.size() == 1 and Crew.find(crew[0]).status == "flying"
	_check(scene.persistent and Sim.vessels().size() == 1 and flying and scene.leave_refusal() == "",
		"launch: 1 vessel in the world, %s seated and flying, may leave from the pad" % [crew])

	# 2. leave it on the pad: saved; a "restart" loads the same world back
	scene._leave()
	await _frames(tree, 3)
	var saved := FileAccess.get_file_as_string(Sim.save_dir + "world.json")
	Sim.game = null
	Sim.seats = {}
	var back := Sim.game_session()
	_check(saved != "" and back.core.world_save() == saved and Sim.crew_of(id) == crew,
		"left and reloaded: world identical (%d bytes), crew still %s" % [saved.length(), Sim.crew_of(id)])

	# 3. the next launch finds the pad taken: the Orbiter is recovered first
	scene = await _flight(tree, hopper)
	var hop: int = scene.session.active
	var names := []
	for v in Sim.vessels():
		names.append(v.name)
	_check(names == ["Hopper"] and int(Crew.find(crew[0]).flights) == 1 and scene.hud._toast.text.contains("Cleared the pad"),
		"pad cleared: world holds %s, %s recovered (flights %d) and free to fly again, toast '%s'" % [names, crew[0], int(Crew.find(crew[0]).flights), scene.hud._toast.text.left(60)])

	# 4. off the pad and climbing: can't leave; revert to hangar undoes it
	scene._throttle = 1.0
	scene._stage()
	scene.session.step_ticks(150)
	var why: String = scene.leave_refusal()
	var hop_crew: Array = Sim.crew_of(hop)
	scene._on_menu("revert_hangar")
	await _frames(tree, 3)
	_check(why != "" and Sim.vessels().is_empty() and Sim.seats.is_empty() and Crew.find(hop_crew[0]).status == "available",
		"in the air: refused ('%s'); reverted to hangar: world empty, %s available" % [why, hop_crew])

	# 5. a craft left in orbit, flown again later: same vessel, same moment
	var g := Sim.game_session()
	var oid: int = g.core.flight_spawn_orbit(orbiter, "floe", 150000.0, 0.0)
	Sim.seat_crew(oid, Crew.assign(1))
	g.step_ticks(50)
	var t_left: float = g.time()
	Sim.save_game()
	Sim.pending_vessel = oid
	scene = await _flight(tree, "")
	var tel: Dictionary = scene.session.telemetry()
	_check(scene._resumed and scene.session.active == oid and tel.situation == "orbiting" and absf(scene.session.time() - t_left) < 0.5
		and scene.crew_names == Sim.crew_of(oid) and scene.leave_refusal() == "",
		"resumed in orbit: vessel %d %s at %.0f km, clock %.2f s on from leaving (the frames since), crew %s" % [oid, tel.situation, tel.altitude / 1000.0,
			scene.session.time() - t_left, scene.crew_names])

	# 6. a crewed wreck dropped from the world takes its crew with it
	var lost_names: Array = Sim.crew_of(oid)
	g.core.vessel_remove(oid)
	var lost := Sim.sync_crew()
	_check(lost == lost_names and Crew.find(lost_names[0]).status == "lost",
		"vessel gone: %s marked lost" % [lost])
	return {"status": "PASS" if _failed == 0 else "FAIL", "log": "\n".join(_log)}


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _flight(tree: SceneTree, text: String) -> Node3D:
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	await _frames(tree, 2)
	Sim.tutorial = false
	Sim.pending_craft_text = text
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	return scene
