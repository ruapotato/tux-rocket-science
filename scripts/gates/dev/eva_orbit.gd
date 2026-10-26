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
## Dev: a spacewalk in orbit. F out of the Orbiter at 120 km; the penguin
## floats beside it (no ground, so SAS holds steady); R jetpack and a push
## away, then back toward the pod with the jetpack until within reach;
## F back in. Logs distances and mono; shots with a screen.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var gpu := DisplayServer.get_name() != "headless"
	var g := Sim.game_session()
	for v in Sim.vessels():
		g.core.vessel_remove(int(v.id))
	var R := 720000.0
	var id: int = g.core.flight_spawn_state(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json"), "floe",
		PackedFloat64Array([R, 0, 0, 0, sqrt(3.528e12 / R), 0]))
	Sim.seat_crew(id, ["Commander Tux"])
	Sim.pending_vessel = id
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var ev := InputEventKey.new()
	ev.keycode = KEY_F
	ev.pressed = true
	scene._key(ev)
	var pen := s.active
	var t := s.telemetry()
	log.append("out: active %d eva %s sas %s, %.2f m from the orbiter, hatch %s" % [pen, t.get("eva", false), t.sas,
		Frame.vec(s.core.vessel_render(pen, id), 1).length(), s.core.eva_hatch(pen, id)])
	s.step_ticks(100)
	t = s.telemetry()
	var rel := Frame.vec(s.core.vessel_render(pen, id), 1)
	log.append("after 2 s: %.2f m apart, sas %s, spin %.4f, situation %s" % [rel.length(), t.sas, t.spin, t.situation])
	if gpu:
		for i in 20:
			await tree.process_frame
		await RenderingServer.frame_post_draw
		scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/eva_orbit.png")
	# jetpack: push away for 2 s, then home on the pod
	scene._toggle_rcs()
	s._telemetry_tick = -1
	var away := rel.normalized()  # from the pod out to the penguin (rel = penguin relative to the orbiter)
	for k in 250:
		_push(s, away)
		s.step_ticks(1)
	_push(s, Vector3.ZERO)
	s.step_ticks(250)  # coast out
	var far := Frame.vec(s.core.vessel_render(pen, id), 1).length()
	var mono0: float = s.telemetry().mono
	var steps := 0
	while steps < 2000 and (s.core.eva_hatch(pen, id) as PackedFloat64Array)[0] < 0:
		var to := -Frame.vec(s.core.vessel_render(pen, id), 1)  # pod from the penguin
		var vrel := _rel_vel(s, pen, id)
		var want := to.normalized() * minf(1.0, to.length() * 0.3) - vrel  # close at under 1 m/s
		_push(s, want.normalized() if want.length() > 0.05 else Vector3.ZERO)
		s.step_ticks(1)
		steps += 1
	_push(s, Vector3.ZERO)
	t = s.telemetry()
	var h: PackedFloat64Array = s.core.eva_hatch(pen, id)
	log.append("pushed out to %.2f m; flew back in %.1f s using %.2f kg mono; hatch %s gap %.2f" % [far, steps / 50.0, mono0 - float(t.mono), h[0], h[1]])
	scene._key(ev)
	log.append("F: active %d (orbiter %d), crew %s" % [s.active, id, Sim.crew_of(id)])
	var ok: bool = s.active == id and Sim.crew_of(id) == ["Commander Tux"]
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(log)}


func _push(s: FlightSession, world_dir: Vector3) -> void:
	var t := s.telemetry()
	var n := Frame.vec_arr(t.nose).normalized()
	var x := Frame.vec_arr(t.side).normalized()
	var z := x.cross(n)
	s.set_translate(clampf(world_dir.dot(x), -1, 1), clampf(world_dir.dot(n), -1, 1), clampf(world_dir.dot(z), -1, 1))


## velocity of vessel a relative to b, render frame
func _rel_vel(s: FlightSession, a: int, b: int) -> Vector3:
	var ta: Dictionary = JSON.parse_string(s.core.telemetry(a))
	var tb: Dictionary = JSON.parse_string(s.core.telemetry(b))
	return Frame.vec_arr(ta.velocity) - Frame.vec_arr(tb.velocity) if ta.has("velocity") else Vector3.ZERO
