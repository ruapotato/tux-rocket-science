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
## Dev (GPU): a player's hover controller on the Hopper, in real time: the
## flight screen's COMPUTER panel with its live charts. Saves hover_<k>.png
## and says how well it held the height.
extends RefCounted

const HOVER := """# hover.py -- hold 100 m over the pad, then come down to 20 m
lifter = ship.part("Walrus")
target = 100
print("lift off: holding", target, "m")
lifter.throttle = 60                 # lights it: the clamps let go
aim("up")
t0 = ship.time
while ship.time - t0 < 45:
    if ship.time - t0 > 28 and target != 20:
        target = 20
        print("down to", target, "m")
    push = lifter.max_thrust * 1000 / ship.mass     # m/s2, flat out
    hover = 100 * ship.body.gravity / push          # % that just holds it
    err = target - ship.height
    lifter.throttle = clamp(hover + err * 1.5 - ship.vertical_speed * 3, 0, 100)
    chart("height", ship.height)
    chart("throttle", lifter.throttle)
    sleep(0.1)
lifter.throttle = 0
print(f"done: {ship.height:.1f} m up, {ship.fuel:.0f}% fuel left")
"""


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var gate := preload("res://scripts/gates/test_by_hand.gd").new()
	var scene: Node3D = await gate._scene(tree, "hopper", "pad")
	var comp: ShipComputer = ShipComputer.get_shared()
	var s: FlightSession = scene.session
	comp.machine.write_file("/root/scripts/hover.py", HOVER)
	comp.machine.start("/root/scripts/hover.py")
	var log := PackedStringArray()
	var t0 := s.time()
	var hold := []
	var shot := 0
	var wall := Time.get_ticks_msec()
	while s.time() - t0 < 50.0 and Time.get_ticks_msec() - wall < 150000:
		await tree.process_frame
		var tt := s.time() - t0
		var h := float(s.telemetry().get("altitude_terrain", 0.0))
		if tt > 12.0 and tt < 27.0:
			hold.append(h)
		if (shot == 0 and tt > 20.0) or (shot == 1 and tt > 40.0):
			shot += 1
			await RenderingServer.frame_post_draw
			scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/hover_%d.png" % shot)
			log.append("saved hover_%d at %.0f s, %.1f m up" % [shot, tt, h])
	log.append("game time %.1f s in %.1f s" % [s.time() - t0, (Time.get_ticks_msec() - wall) / 1000.0])
	if not hold.is_empty():
		var lo: float = hold.min()
		var hi: float = hold.max()
		log.append("held between %.1f and %.1f m (target 100) from 12 to 27 s" % [lo, hi])
	for l in comp.log.slice(maxi(0, comp.log.size() - 6)):
		log.append("  said: " + str(l))
	return {"status": "PASS", "log": "\n".join(log)}
