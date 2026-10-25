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
## Dev: the Orbiter's predicted path on the map during the climb (at 30,
## 50 and 72 km): should be a smooth arc, never a polygon.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var api := ControlAPI.new(s)
	var ap := AscentAutopilot.new(api)
	var log := PackedStringArray()
	var ok := true
	for alt in [30000.0, 50000.0, 72000.0]:
		var guard := 0
		while s.telemetry().altitude < alt and guard < 50 * 400:
			ap.step(5 * FlightSession.TICK)
			api.handle({"cmd": "step", "ticks": 5})
			guard += 5
		scene._map.visible = true
		scene._map._dist = 5.0
		scene.hud.visible = false
		for i in 5:
			await tree.process_frame
		# the sharpest turn between neighbouring segments of the drawn path
		var worst := 0.0
		var sm: Array = scene._map._samples
		for k in range(1, sm.size() - 1):
			if sm[k - 1].patch != sm[k].patch or sm[k + 1].patch != sm[k].patch:
				continue
			var a: Vector3 = sm[k].pos - sm[k - 1].pos
			var b: Vector3 = sm[k + 1].pos - sm[k].pos
			if a.length() > 0.0 and b.length() > 0.0:
				worst = maxf(worst, rad_to_deg(a.angle_to(b)))
		var t := s.telemetry()
		log.append("%.0f km: %d samples, %d patches (%s), sharpest bend %.1f deg, Ap %.0f km Pe %.0f km" % [
			alt / 1000.0, sm.size(), scene._map._patches.size(), ",".join(scene._map._patches.map(func(p): return p.end)), worst,
			t.orbit.ap_alt / 1000.0, t.orbit.pe_alt / 1000.0])
		ok = ok and worst < 10.0
		await RenderingServer.frame_post_draw
		scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/map_arc_%dkm.png" % int(alt / 1000.0))
		scene._map.visible = false
		scene.hud.visible = true
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(log)}
