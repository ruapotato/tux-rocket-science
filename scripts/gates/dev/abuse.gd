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
## Dev: what a player's worst scripts do to the game: how long a frame of
## the computer takes with each running, and what each one says.
extends RefCounted

const SCRIPTS := [
	["spin", "while True:\n    pass\n"],
	["spam", "i = 0\nwhile True:\n    i += 1\n    print('line', i)\n"],
	["hog", "l = []\nwhile True:\n    l.append([1, 2, 3, 4, 5, 6, 7, 8])\n"],
	["deep", "def f(n):\n    return f(n + 1)\nf(0)\n"],
	["readloop", "while True:\n    x = ship.altitude + ship.orbit.top + ship.part('Walrus').fuel\n"],
	["typo", "throtle(100)\n"],
	["badtype", "x = 'height' + 5\n"],
	["divzero", "speed = 0\nprint(100 / speed)\n"],
	["nopart", "e = ship.part('Warp Drive')\n"],
	["fuelset", "ship.part('Walrus').fuel = 100\n"],
]


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	var gate := preload("res://scripts/gates/test_by_hand.gd").new()
	var scene: Node3D = await gate._scene(tree, "orbiter", "pad")
	var s: FlightSession = scene.session
	var comp: ShipComputer = ShipComputer.get_shared()
	var log := PackedStringArray()
	for sc in SCRIPTS:
		comp.machine.write_file("/root/%s.py" % sc[0], sc[1])
		var seen := comp.log_count
		comp.machine.start("/root/%s.py" % sc[0])
		var worst := 0
		var total := 0
		for i in 120:
			s.step_ticks(1)
			var t0 := Time.get_ticks_usec()
			comp.update(scene, s.telemetry())
			var dt := Time.get_ticks_usec() - t0
			worst = maxi(worst, dt)
			total += dt
		var said := PackedStringArray()
		var fresh := mini(comp.log_count - seen, comp.log.size())
		for j in range(comp.log.size() - fresh, comp.log.size()):
			said.append(str(comp.log[j]))
		var running := false
		for row in str(comp.sh("ps")).split("\n", false):
			if row.contains("/bin/py") and row.contains(" running "):
				running = true
				var f := row.strip_edges().split(" ", false)
				comp.sh("kill " + f[0])
		log.append("%-9s frame avg %.2f ms, worst %.2f ms, %s; said %d lines: %s" % [sc[0], total / 120000.0, worst / 1000.0,
			"still running (killed)" if running else "ended", said.size(), " / ".join(said.slice(maxi(0, said.size() - 3)))])
	return {"status": "PASS", "log": "\n".join(log)}
