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
## Dev: the edges of a script's life: its ship crashing under it; the
## player's own time warp through its sleep; a new flight while it runs.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var gate := preload("res://scripts/gates/test_by_hand.gd").new()
	var log := PackedStringArray()
	var comp: ShipComputer = ShipComputer.get_shared()

	# 1. crash: full power to 1.5 km, engine off, no chute
	var scene: Node3D = await gate._scene(tree, "hopper", "pad")
	var s: FlightSession = scene.session
	comp.machine.write_file("/root/crash.py", "e = ship.engines[0]\ne.throttle = 100\naim('up')\nwhile ship.height < 1500:\n    sleep(0.2)\ne.throttle = 0\nwhile True:\n    print(f'{ship.height:.0f} m, {ship.falling_speed:.0f} m/s down')\n    sleep(2)\n")
	comp.log.clear()
	comp.machine.start("/root/crash.py")
	for i in 6000:
		s.step_ticks(4)
		comp.update(scene, s.telemetry())
		if not s.telemetry().get("alive", false) and i % 50 == 0 and i > 0:
			pass
	var tail := comp.log.slice(maxi(0, comp.log.size() - 5))
	log.append("crash: alive %s, situation %s; the script said last: %s" % [s.telemetry().get("alive", "?"), s.telemetry().get("situation", "?"), " / ".join(tail)])
	var running := 0
	for row in str(comp.machine.scripts()).split("\n", false):
		if row.split("\t")[1] == "1":
			running += 1
	log.append("crash: scripts still running %d" % running)

	# 2. a new flight while a script runs
	scene.queue_free()
	await tree.process_frame
	scene = await gate._scene(tree, "orbiter", "pad")
	running = 0
	for row in str(comp.machine.scripts()).split("\n", false):
		if row.split("\t")[1] == "1":
			running += 1
	log.append("new flight: scripts still running %d (toast: %s)" % [running, scene._pending_toast])

	# 3. the player's time warp through a sleep: in orbit, sleep 600 s, warp 1000x
	scene.queue_free()
	await tree.process_frame
	scene = await gate._scene(tree, "orbiter", "orbit")
	s = scene.session
	comp.machine.write_file("/root/nap.py", "t0 = ship.time\nsleep(600)\nprint(f'woke {ship.time - t0 - 600:.1f} s late')\n")
	comp.log.clear()
	comp.machine.start("/root/nap.py")
	for i in 40:
		await tree.process_frame
	s.set_warp("rails", 5)    # 1000x, as the player would
	var wall := Time.get_ticks_msec()
	while Time.get_ticks_msec() - wall < 60000:
		await tree.process_frame
		if comp.log.size() > 0 and str(comp.log[-1]).begins_with("woke"):
			break
	log.append("warp: %s; warp now %s %d" % [" / ".join(comp.log), s.warp_kind, s.warp_index])
	scene.queue_free()
	await tree.process_frame
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	return {"status": "PASS", "log": "\n".join(log)}
