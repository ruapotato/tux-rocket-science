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
## --test-computer-flight: the ship's computer flies. The Orbiter on the pad;
## the computer's readings are the HUD's (to the whole unit); `ship throttle
## 0.5` typed at its shell is the throttle; the desktop comes up with its
## apps (every one opens a window, no script errors); and the two example
## scripts on its disk, running on its CPU, fly the Orbiter off the pad to
## an 80 km apoapsis (ascent.py) and round it into an orbit clear of the air
## (circularize.py), with nobody touching the keys.
extends RefCounted

var _log := PackedStringArray()
var _failed := 0


func _check(ok: bool, msg: String) -> void:
	_log.append(("ok   " if ok else "FAIL ") + msg)
	if not ok:
		_failed += 1


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			c.queue_free()
	await _frames(tree, 2)
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	Sim.pending_site = "pad"
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var comp: ShipComputer = scene.computer
	_check(comp != null and comp.machine != null, "the ship's computer is up")

	# 1. what it reads is what the HUD says
	var t := s.telemetry()
	comp.update(scene, t)
	var r := ShipComputer.readings(t, comp.programs, s)
	var got := comp.sh("cat /dev/ship/altitude").strip_edges()
	var st := comp.sh("ship status")
	_check(got == str(r.altitude) and got == "%.2f" % float(t.altitude) and st.contains("prelaunch") and st.contains(str(r.mass)),
		"readings: /dev/ship/altitude %s m = the HUD's %.1f m; ship status shows the situation and the mass" % [got, float(t.altitude)])

	# 2. a command at its shell is the throttle
	comp.sh("ship throttle 0.5")
	comp.update(scene, s.telemetry())
	_check(absf(float(s.telemetry().throttle) - 0.5) < 1e-9 and is_equal_approx(scene._throttle, 0.5),
		"`ship throttle 0.5` at its shell: the throttle is %.2f" % float(s.telemetry().throttle))
	comp.sh("ship throttle 0")
	comp.update(scene, s.telemetry())

	# 3. the desktop and every app on it
	scene._toggle_desk()
	await _frames(tree, 3)
	var de: Control = scene._de
	var opened := []
	var missed := []
	for app in de.LAUNCHERS:
		var key: String = app[1]
		var before: int = de.windows.size()
		de._launch(key)
		await _frames(tree, 2)
		if de.windows.size() > before or de._find(str(de.TITLES.get(de.EXEC_MAP.get(key, key), "?"))) != null:
			opened.append(key)
		else:
			missed.append(key)
	_check(opened.size() >= 20 and "flight" in opened and ("term" in opened or "terminal" in opened) and "g2048" in opened and "gsnake" in opened,
		"the desktop: %d apps open a window (%s)%s" % [opened.size(), ", ".join(opened),
			"" if missed.is_empty() else "; none for " + ", ".join(missed)])
	scene._toggle_desk()
	await _frames(tree, 2)

	# 4. the examples fly it to orbit
	comp.update(scene, s.telemetry())
	var why := str(comp.machine.start("/root/examples/ascent.py"))
	var ticks := 0
	var apo := 0.0
	while ticks < 50 * 400:
		s.step_ticks(10)
		ticks += 10
		comp.update(scene, s.telemetry())
		t = s.telemetry()
		var o: Dictionary = t.get("orbit", {})
		apo = float(o.get("ap_alt", 0.0)) if o.get("ap_alt") != null else 0.0
		if apo >= 80000.0 and float(t.throttle) == 0.0:
			break
		if not t.get("alive", false):
			break
	var said := "\n".join(comp.log.slice(maxi(0, comp.log.size() - 4)))
	_check(why == "" and apo >= 80000.0 and float(t.get("throttle", 1.0)) == 0.0 and float(t.get("altitude", 0.0)) > 20000.0,
		"ascent.py on the computer's CPU: off the pad to a %.1f km apoapsis in %.0f s, engine off, %.1f km up (it said: %s)" % [
			apo / 1000.0, ticks / 50.0, float(t.get("altitude", 0.0)) / 1000.0, said.replace("\n", " / ")])
	why = str(comp.machine.start("/root/examples/circularize.py"))
	var t0 := ticks
	var pe := -1e9
	while ticks < t0 + 50 * 600:
		s.step_ticks(10)
		ticks += 10
		comp.update(scene, s.telemetry())
		t = s.telemetry()
		pe = float((t.get("orbit", {}) as Dictionary).get("pe_alt", -1e9))
		if pe >= 72000.0 and float(t.get("throttle", 1.0)) == 0.0:
			break
		if not t.get("alive", false):
			break
	_check(why == "" and pe >= 72000.0 and t.get("situation", "") == "orbiting",
		"circularize.py: periapsis %.1f km after %.0f s more: %s" % [pe / 1000.0, (ticks - t0) / 50.0, t.get("situation", "lost")])

	scene.queue_free()
	await _frames(tree, 2)
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	Sim.save_game()
	return {"status": "FAIL" if _failed else "PASS", "log": "\n".join(_log)}
