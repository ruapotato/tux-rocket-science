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
## --test-templates: the Script Editor's "New" scripts each do what they
## say: the first script hops to 2 km and back on a parachute; hover.py
## holds its height to a couple of metres; the co-pilot warns a pilot who
## is falling fast; a script made from the ship being flown flies its
## stages; and the career pays for hands-off flying (the first script earns
## "The computer flies", a hover at 50 m "Steady as she goes") -- but not
## once the pilot has touched the keys.
extends RefCounted

const Templates := preload("res://scripts/os/templates.gd")
const ScriptMaker := preload("res://scripts/os/script_maker.gd")

var _log := PackedStringArray()
var _failed := 0
var _gate := preload("res://scripts/gates/test_by_hand.gd").new()


func _check(ok: bool, msg: String) -> void:
	_log.append(("ok   " if ok else "FAIL ") + msg)
	if not ok:
		_failed += 1


## Steps the flight with a script running (and the scene's mission checks);
## pilot(t) is called each step as a pilot at the keys would act.
func _fly(scene: Node3D, path: String, text: String, max_s: float, pilot := Callable(), presses := 0) -> Dictionary:
	var s: FlightSession = scene.session
	var comp: ShipComputer = ShipComputer.get_shared()
	scene.computer = comp
	for pid in _gate._scripts(comp):
		comp.sh("kill " + str(pid))
	comp.machine.write_file(path, text)
	var seen := comp.log_count
	var why := str(comp.machine.start(path))
	var t0 := s.time()
	var heard := []
	var guard := 0
	var pressed := 0
	var heights := []
	while s.time() - t0 < max_s and guard < 200000:
		var tw := s.telemetry()
		s.step_ticks(1 if float(tw.get("altitude_terrain", 1e9)) < 50.0 and not tw.get("landed", false) else 4)
		guard += 1
		if pilot.is_valid():
			pilot.call(s.time() - t0)
		comp.update(scene, s.telemetry())
		heights.append([s.time() - t0, float(s.telemetry().get("altitude_terrain", 0.0))])
		if comp.wants_space() and pressed < presses:
			comp.take_space()
			pressed += 1
		if guard % 10 == 0:
			scene._check_missions()
		var fresh := mini(comp.log_count - seen, comp.log.size())
		for k in range(comp.log.size() - fresh, comp.log.size()):
			if not str(comp.log[k]).begins_with("> "):
				heard.append(str(comp.log[k]))
		seen = comp.log_count
		if guard % 25 == 0 and _gate._scripts(comp).is_empty():
			break
	for pid in _gate._scripts(comp):
		comp.sh("kill " + str(pid))
	return {"started": why == "", "said": heard, "t": s.time() - t0, "heights": heights, "pressed": pressed}


func _between(heights: Array, a: float, b: float) -> Array:
	var lo := INF
	var hi := -INF
	for h in heights:
		if float(h[0]) >= a and float(h[0]) <= b:
			lo = minf(lo, float(h[1]))
			hi = maxf(hi, float(h[1]))
	return [lo, hi]


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false

	# 1. the first script
	var scene: Node3D = await _gate._scene(tree, "orbiter", "pad")
	var r := _fly(scene, "/root/scripts/first_script.py", Templates.FIRST, 400.0)
	var said := " / ".join(r.said)
	var top: float = _between(r.heights, 0.0, 1e9)[1]
	_check(r.started and said.contains("The top:") and not said.contains("py: ") and top > 2000.0,
		"first_script.py: climbed to %.0f m and said what it did (%s)" % [top, said])
	scene.queue_free()
	await tree.process_frame

	# 2. hover.py
	scene = await _gate._scene(tree, "hopper", "pad")
	r = _fly(scene, "/root/scripts/hover.py", Templates.HOVER, 60.0)
	said = " / ".join(r.said)
	var held := _between(r.heights, 12.0, 27.0)
	var low := _between(r.heights, 38.0, 44.0)
	_check(r.started and held[0] > 98.0 and held[1] < 102.0 and low[0] > 18.0 and low[1] < 22.0 and said.contains("done:"),
		"hover.py: held %.1f-%.1f m for a 100 m target, then %.1f-%.1f m for 20 m (%s)" % [held[0], held[1], low[0], low[1], said])
	scene.queue_free()
	await tree.process_frame

	# 2b. the suicide burn: down on the pad on its engine
	scene = await _gate._scene(tree, "hopper", "pad")
	r = _fly(scene, "/root/scripts/suicide_burn.py", Templates.SUICIDE, 200.0)
	said = " / ".join(r.said)
	var tl: Dictionary = scene.session.telemetry()
	_check(r.started and tl.get("alive", false) and tl.get("landed", false) and float(tl.get("max_impact", 99.0)) < 5.0 and said.contains("down!"),
		"suicide_burn.py: up to 3 km and down on its engine, worst contact %.1f m/s (%s)" % [float(tl.get("max_impact", -1.0)), said])
	scene.queue_free()
	await tree.process_frame

	# 3. the co-pilot, while a pilot burns a few seconds and lets it fall
	scene = await _gate._scene(tree, "hopper", "pad")
	var sc: Node3D = scene
	var pilot := func(tt: float) -> void:
		if tt > 1.0 and tt < 1.2 and int(sc.session.telemetry().get("next_stage", -1)) == 1:
			sc.session.set_throttle(1.0)
			sc.session.stage()
		if tt > 9.0:
			sc.session.set_throttle(0.0)
	r = _fly(scene, "/root/scripts/watch.py", Templates.WATCH, 130.0, pilot)
	said = " / ".join(r.said)
	_check(r.started and said.contains("Falling") and not said.contains("py: "),
		"watch.py while a pilot flies: it warned of the fall (%s) [top %.0f m, last %.0f m, %.0f s, running %s]" % [said, _between(r.heights, 0, 1e9)[1], float(r.heights[-1][1]) if not r.heights.is_empty() else -1.0, r.t, str(r.started)])
	scene.queue_free()
	await tree.process_frame

	# 4. a script made from the ship being flown
	scene = await _gate._scene(tree, "krill_lander", "pad")
	var comp: ShipComputer = ShipComputer.get_shared()
	comp.update(scene, scene.session.telemetry())
	var t: Dictionary = scene.session.telemetry()
	var made: String = ScriptMaker.make(comp._parts, str(t.get("name", "")), int(t.get("next_stage", 99)))
	var presses := made.count("\nwait_user_trigger()\n")
	r = _fly(scene, "/root/scripts/from_ship.py", made, 300.0, Callable(), presses)
	said = " / ".join(r.said)
	_check(r.started and r.pressed == presses and said.contains("every stage done") and int(scene.session.telemetry().get("next_stage", 9)) <= 0,
		"New > From this ship's stages (in flight, the Krill lander): %d stages, each on a space (%s)" % [presses, said])
	scene.queue_free()
	await tree.process_frame

	# 5. the career pays for hands-off flying
	DirAccess.remove_absolute(ProjectSettings.globalize_path(Sim.save_dir + "career.json"))
	Career.load_from(Sim.save_dir + "career.json", "career")
	Career.done = ["hop"]
	scene = await _gate._scene(tree, "orbiter", "pad")
	r = _fly(scene, "/root/scripts/first_script.py", Templates.FIRST, 120.0)
	var auto_hop: bool = "auto_hop" in Career.done
	scene.queue_free()
	await tree.process_frame
	scene = await _gate._scene(tree, "hopper", "pad")
	var hover50 := Templates.HOVER.replace("target = 100", "target = 50").replace("t0 > 28", "t0 > 40")
	r = _fly(scene, "/root/scripts/hover50.py", hover50, 45.0)
	var steady: bool = "hover_50" in Career.done
	scene.queue_free()
	await tree.process_frame
	# the same hover, a pilot's hand on the throttle once: no pay
	Career.done = ["hop", "auto_hop"]
	scene = await _gate._scene(tree, "hopper", "pad")
	var sc2: Node3D = scene
	var touch := func(tt: float) -> void:
		if tt > 5.0 and tt < 5.1:
			sc2.hands_used = true   # (as a press of Z, X, a SAS button or a stage would)
	r = _fly(scene, "/root/scripts/hover50.py", hover50, 45.0, touch)
	var not_paid: bool = not "hover_50" in Career.done
	scene.queue_free()
	await tree.process_frame
	_check(auto_hop and steady and not_paid,
		"hands-off contracts: first_script.py earned 'The computer flies' (%s); a hover at 50 m earned 'Steady as she goes' (%s); with a hand on the keys it didn't (%s)" % [auto_hop, steady, not_paid])
	DirAccess.remove_absolute(ProjectSettings.globalize_path(Sim.save_dir + "career.json"))
	Career.load_from(Sim.save_dir + "career.json", "sandbox")

	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	Sim.save_game()
	return {"status": "FAIL" if _failed else "PASS", "log": "\n".join(_log)}
