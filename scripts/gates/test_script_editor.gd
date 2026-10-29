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
## --test-script-editor: the Script Editor on the ship's desktop. It
## suggests what is really there (every call the language has; after
## "ship." the ship's; after a part's name that kind of part's; inside
## ship.part(" the parts aboard); every call it suggests is one /bin/py
## answers; a script run from it shows its variables live, stops at a
## breakpoint with the line marked, steps, goes on and stops; and the
## console runs Python on the ship, keeping its variables, blocks and all.
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


## The suggestions the editor offers with the caret at the end of `text`.
func _suggest(app: Control, text: String) -> Array:
	var ed: CodeEdit = app.edit
	ed.text = text
	ed.set_caret_line(ed.get_line_count() - 1)
	ed.set_caret_column(ed.get_line(ed.get_line_count() - 1).length())
	app._complete()
	var out := []
	for o in ed.get_code_completion_options():
		out.append(str(o.insert_text))
	ed.cancel_code_completion()
	return out


## Steps the flight (and the computer, and the editor's watch) n frames.
func _fly(scene: Node3D, app: Control, n: int) -> void:
	for i in n:
		scene.session.step_ticks(3)
		scene.computer.update(scene, scene.session.telemetry())
		app._process(0.25)


func _watch(app: Control) -> Dictionary:
	var d := {}
	var root: TreeItem = app.watch.get_root()
	if root == null:
		return d
	for it in root.get_children():
		d[it.get_text(0)] = it.get_text(1)
	return d


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
	comp.update(scene, s.telemetry())
	scene._toggle_desk()
	await _frames(tree, 3)
	var de: Control = scene._de
	de._launch("code")
	await _frames(tree, 3)
	var win: Control = de._find("script editor")
	var app: Control = win.get_meta("content") if win else null
	_check(app != null and app.edit != null, "the Script Editor opens on the ship's desktop")
	if app == null:
		return {"status": "FAIL", "log": "\n".join(_log)}

	# 1. suggestions: what is really there
	var plain := _suggest(app, "aim")
	var ship_s := _suggest(app, "ship.")
	var orbit_s := _suggest(app, "x = ship.orbit.")
	var part_s := _suggest(app, "engine = ship.part(\"Walrus\")\nengine.")
	var drop_s := _suggest(app, "drop = ship.part(\"Crack-1 Stack Decoupler 1\")\ndrop.")
	var names := _suggest(app, "e = ship.part(\"")
	var mine := _suggest(app, "lean_angle = 3\nlean")
	_check("aim(" in plain and "aim_at(" in plain, "typing aim suggests %s" % ", ".join(plain))
	_check("orbit" in ship_s and "engines" in ship_s and "part(" in ship_s and "height" in ship_s,
		"after ship. it suggests the ship's: %s..." % ", ".join(ship_s.slice(0, 8)))
	_check("top" in orbit_s and "time_to_top" in orbit_s, "after ship.orbit. its orbit's: %s" % ", ".join(orbit_s))
	_check("throttle" in part_s and "ignite()" in part_s and not "decouple()" in part_s,
		"after an engine's name, an engine's: %s" % ", ".join(part_s))
	_check("decouple()" in drop_s and not "throttle" in drop_s, "after a decoupler's, a decoupler's: %s" % ", ".join(drop_s))
	_check("\"Walrus LE-215 Lifter Engine\"" in names and "\"Crack-1 Stack Decoupler 1\"" in names,
		"inside ship.part(\" the parts aboard: %s" % ", ".join(names))
	_check("lean_angle" in mine, "and the script's own names (lean_angle)")

	# 2. every call it suggests is one py answers (a name it doesn't know
	# is a compile error, so one line naming them all says)
	var all := PackedStringArray()
	for c in app.PyLib.CALLS:
		all.append(str(c[0]))
	comp.machine.write_file("/root/names.py", "x = [%s]\nprint(len(x))\n" % ", ".join(all))
	var said := str(comp.sh("py /root/names.py"))
	_check(said.strip_edges() == str(all.size()), "all %d calls it suggests are ones /bin/py has (%s)" % [all.size(), said.strip_edges()])

	# 3. run it, and watch it think
	app._open("/root/scripts/watch_me.py")
	app.edit.text = "n = 0\nwhile n < 100000:\n    n = n + 1\n    up = round(ship.altitude)\n    if n % 100 == 0:\n        print(\"hundred\", n)\n    sleep(0.05)\n"
	app._run()
	_fly(scene, app, 60)
	var w := _watch(app)
	_check(w.has("n") and w.has("up") and int(w.get("n", "0")) > 1 and app._pid > 0,
		"Run: the watch shows the script's variables live: %s" % str(w))
	# the Flight app marks the script running (RUN <pid>)
	de._launch("flight")
	await _frames(tree, 2)
	var fw: Control = de._find("flight")
	var fl: Control = fw.get_meta("content") if fw else null
	if fl:
		fl._refresh()
	_check(fl != null and fl._pid_of("/root/scripts/watch_me.py") == app._pid,
		"the Flight app shows the script running (RUN %d)" % (fl._pid_of("/root/scripts/watch_me.py") if fl else -1))
	de._raise(win)
	app.edit.set_line_as_breakpoint(5, true)  # print("hundred", n): line 6
	app._write_ctl()
	var paused := false
	for k in 200:
		_fly(scene, app, 1)
		if str(app.status.text).contains("paused at line 6"):
			paused = true
			break
	w = _watch(app)
	_check(paused and int(w.get("n", "1")) % 100 == 0 and app._exec_line == 5,
		"a breakpoint on line 6 stopped it there (n = %s, line %d marked): %s" % [w.get("n", "?"), app._exec_line + 1, app.status.text])
	app._step_line()
	var stepped := false
	for k in 100:
		_fly(scene, app, 1)
		if str(app.status.text).contains("(a step)"):
			stepped = true
			break
	_check(stepped and app._exec_line != 5, "Step ran line 6 (it printed) and stopped at line %d: %s" % [app._exec_line + 1, app.status.text])
	app.edit.set_line_as_breakpoint(5, false)
	app._pause()  # (Continue)
	_fly(scene, app, 30)
	var n_then := int(_watch(app).get("n", "0"))
	app._stop()
	_fly(scene, app, 10)
	# (alive by the computer's own list of scripts: a process an earlier
	# gate's kill left in ps, never to run again, is not this one)
	var running := false
	for row in str(comp.machine.scripts()).split("\n", false):
		var g := row.split("\t")
		running = running or (g.size() >= 4 and g[1] == "1" and g[3] != "-i")
	var hundreds := 0
	for line in comp.log:
		hundreds += 1 if str(line).begins_with("hundred ") else 0
	_check(not running and hundreds >= 1,
		"Continue went on (n = %d); Stop stopped it; and what it printed came out (%d lines of hundred)" % [n_then, hundreds])

	# 4. the console: Python on the ship, now
	app._console_line("2 + 3")
	_fly(scene, app, 20)
	app._console_line("m = ship.mass")
	_fly(scene, app, 20)
	app._console_line("round(m / 1000, 1)")
	_fly(scene, app, 20)
	app._console_line("for i in [1, 2]:")
	app._console_line("    print(i * 10)")
	app._console_line("")
	_fly(scene, app, 30)
	var text: String = app.out.get_parsed_text()
	var tonnes := "%.1f" % (float(s.telemetry().mass) / 1000.0)
	_check(text.contains(">>> 2 + 3\n") and text.contains("\n5\n") and text.contains("console ready") and text.contains("\n" + tonnes + "\n") and text.contains("10\n20"),
		"the console: its very first line answers (2 + 3: 5); m = ship.mass, then round(m / 1000, 1) says %s; a for block ran whole (10, 20): %s" % [tonnes, text.replace("\n", " / ")])

	# 5. a career: the programs and the planner are research; the language,
	# the parts, aiming and burning are the computer's from the start
	var was_mode: String = Career.mode
	var was_unlocked: Array = Career.unlocked.duplicate()
	Career.mode = "career"
	Career.unlocked = ["basics", "rocketry", "orbits"]
	comp.log.clear()
	for src in ["launch(80000)\n", "x = when_to_leave(\"krill\")\n", "print(\"aimed\", aim_at(90, 90), ship.part(\"Walrus\").fuel)\n"]:
		comp.machine.write_file("/root/scripts/career.py", src)
		comp.machine.start("/root/scripts/career.py")
		_fly(scene, app, 40)
	var heard := " / ".join(comp.log)
	_check(heard.contains("needs the Guidance Computer") and heard.contains("needs the Flight Planner") and heard.contains("aimed True 100"),
		"in a career, launch() and when_to_leave() wait on R&D (and say so); aiming and the parts don't: %s" % heard)
	Career.unlocked.append("planner")
	comp.log.clear()
	comp.machine.write_file("/root/scripts/career.py", "x = when_to_leave(\"krill\")\nprint(\"planner\", round(x.dv))\n")
	comp.machine.start("/root/scripts/career.py")
	_fly(scene, app, 40)
	heard = " / ".join(comp.log)
	_check(heard.contains("closed orbit") and not heard.contains("needs the Flight Planner"),
		"with the Flight Planner researched, when_to_leave() works it out (from the pad: %s)" % heard)
	Career.mode = was_mode
	Career.unlocked = was_unlocked

	scene._toggle_desk()
	await _frames(tree, 2)
	for pid in str(comp.sh("ps")).split("\n", false):
		var f := pid.strip_edges().split(" ", false)
		if f.size() > 5 and f[f.size() - 1] == "/bin/py" and f[2] == "running":
			comp.sh("kill " + f[0])
	scene.queue_free()
	await _frames(tree, 2)
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	Sim.save_game()
	return {"status": "FAIL" if _failed else "PASS", "log": "\n".join(_log)}
