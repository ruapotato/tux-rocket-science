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
## --test-flight-programs: the ship's computer flies a whole mission from a
## script a player could write: the Krill lander off the pad, to Krill and
## down onto it, by the flight library's calls (launch, transfer, warp_to,
## capture, land), every one a program the game flies frame by frame
## through the ship's own controls; and the user's way of saying it
## (wait_for_window, until_orbit_crosses) gets the Orbiter from the
## Orbital Yard into Krill's sphere.
extends RefCounted

var _log := PackedStringArray()
var _failed := 0

const MISSION := """# to Krill and down, by the book
countdown(3)
launch(80000)
transfer("krill")
warp_to("krill")
capture(30000)
land()
say("The penguin has landed on " + get("body") + "!")
print("down with", round(get("dv_left")), "m/s to spare")
"""

const SIMPLE := """# the way you would say it
wait_for_window("krill")
until_orbit_crosses("krill")
warp_to("krill")
print("at", get("body"), "passing", round(get("periapsis") / 1000, 1), "km up")
"""


func _check(ok: bool, msg: String) -> void:
	_log.append(("ok   " if ok else "FAIL ") + msg)
	if not ok:
		_failed += 1


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


const CLOCK := 0.0


func _scene(tree: SceneTree, craft: String, site: String, clock := -1.0) -> Node3D:
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	if clock >= 0.0:
		Sim.game.core.reset_time(clock)
	Sim.seats = {}
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			c.queue_free()
	await _frames(tree, 2)
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + craft + ".craft.json")
	Sim.pending_site = site
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	return scene


## The pids of scripts still running on the computer.
static func _scripts(comp: ShipComputer) -> Array:
	# (the computer's own list of scripts, alive: a process a kill has left
	# behind in ps, never to run again, is no script still going)
	var live := {}
	for row in str(comp.machine.scripts()).split("\n", false):
		var g := row.split("\t")
		if g.size() >= 4 and g[1] == "1" and g[3] != "-i":
			live[g[0]] = true
	var pids := []
	for row in str(comp.sh("ps")).split("\n", false):
		var f := row.strip_edges().split(" ", false)
		if f.size() >= 6 and f[2] == "running" and f[f.size() - 1] in ["/bin/py", "/bin/sh"] and f[1] == "1" and live.has(f[0]):
			pids.append(f[0])
	return pids


## Runs a script on the ship's computer, stepping the flight, until it ends.
func _fly(scene: Node3D, path: String, text: String, max_s: float, step := 5) -> Dictionary:
	var s: FlightSession = scene.session
	var comp: ShipComputer = scene.computer
	for pid in _scripts(comp):
		comp.sh("kill " + str(pid))
	comp.machine.write_file(path, text)
	comp.log.clear()  # (it keeps the last 200 lines: a slice from an old size would read nothing)
	var n0 := 0
	var said := []
	var why := str(comp.machine.start(path))
	var t0 := s.time()
	var running := true
	var guard := 0
	var wall := Time.get_ticks_msec()
	while running and s.time() - t0 < max_s and guard < 400000 and Time.get_ticks_msec() - wall < 600000:
		# near the ground, a tick at a time (the game's own frames are as fine)
		var tt := s.telemetry()
		s.step_ticks(1 if float(tt.get("altitude_terrain", 1e9)) < 300.0 and not tt.get("landed", false) else step)
		guard += 1
		comp.update(scene, s.telemetry())
		if guard % 25 == 0:
			running = not _scripts(comp).is_empty()
		if not s.telemetry().get("alive", false):
			break
	for line in comp.log.slice(mini(n0, comp.log.size())):
		if not str(line).begins_with(">"):
			said.append(str(line))
	return {"started": why == "", "why": why, "ended": not running, "said": said, "t": s.time() - t0}


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	# (from the clock it has when run alone: where the moons are depends on
	# how long the gates before it ran, and some of those clocks miss)
	var t_was: float = Sim.game.core.time()
	var scene := await _scene(tree, "krill_lander", "pad", CLOCK)
	var s: FlightSession = scene.session
	var t_from := s.time()
	var comp: ShipComputer = scene.computer
	_check(comp != null and comp.machine != null, "the ship's computer is up")
	comp.update(scene, s.telemetry())
	var r := _fly(scene, "/root/scripts/mission.py", MISSION, 3.0e6)
	var t := s.telemetry()
	var said := " / ".join(r.said)
	_check(r.started and r.ended and t.get("landed", false) and str(t.get("body", "")) == "krill" and float(t.get("pitch_deg", 0.0)) > 60.0
			and int(t.get("crew_capacity", 0)) > 0 and said.contains("land: down on Krill") and float(t.get("max_impact", 99.0)) < 4.0,
		"mission.py: pad to Krill's ground in %.0f s by launch, transfer, warp_to, capture, land: landed %s on %s, pitch %.0f deg, worst contact %.1f m/s (%s)" % [
			r.t, t.get("landed", false), t.get("body", "?"), float(t.get("pitch_deg", 0.0)), float(t.get("max_impact", 0.0)), said])
	scene.queue_free()
	await _frames(tree, 2)

	# 2. the user's own words, from a parking orbit
	scene = await _scene(tree, "orbiter", "orbit")
	s = scene.session
	comp = scene.computer
	comp.update(scene, s.telemetry())
	var t1 := s.telemetry()
	r = _fly(scene, "/root/scripts/simple.py", SIMPLE, 1.0e6)
	t = s.telemetry()
	said = " / ".join(r.said)
	_check(r.started and r.ended and str(t.get("body", "")) == "krill" and float(t.get("orbit", {}).get("pe_alt", -1.0)) > 0.0,
		"simple.py from a %s orbit: wait_for_window, until_orbit_crosses, warp_to: in %s's sphere, periapsis %.1f km (%s)" % [
			FlightPrograms.km(float(t1.get("altitude", 0.0))), t.get("body", "?"), float(t.get("orbit", {}).get("pe_alt", -1.0)) / 1000.0, said])

	var spent := s.time() - t_from
	scene.queue_free()
	await _frames(tree, 2)
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.game.core.reset_time(t_was + spent)   # (the clock goes on from where it was)
	Sim.seats = {}
	Sim.save_game()
	return {"status": "FAIL" if _failed else "PASS", "log": "\n".join(_log)}
