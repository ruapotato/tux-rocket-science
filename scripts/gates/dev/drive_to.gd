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
## Dev: `drive_to <body>` on its own. The playthrough's Star Skua in a
## 200 km orbit of FROM (floe), then the program to TO (narwhal), logged
## as it goes: how long it took in game time and in real time, and the
## orbit it ended in.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var env := func(k: String, d: String) -> String: return OS.get_environment(k) if OS.get_environment(k) != "" else d
	var Play: GDScript = load("res://scripts/gates/dev/playthrough.gd")
	var text: String = Play.design("star_skua").save_text()
	if tree.current_scene:
		tree.current_scene.queue_free()
	for i in 2:
		await tree.process_frame
	Sim.tutorial = false
	Sim.pending_craft_text = text
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var pad := s.active
	var id: int = -1
	if env.call("FROM", "floe") == "lantern":
		# just inside Lantern's heliopause, falling in at 50 km/s (as a
		# course ends and the drift in starts)
		id = s.core.flight_spawn_state(text, "lantern", PackedFloat64Array([2.99e10, 0, 0, -50000.0, 0, 0]))
	else:
		id = s.core.flight_spawn_orbit(text, env.call("FROM", "floe"), 200000.0, 0.0)
	s.switch_to(id)
	s.core.vessel_remove(pad)
	for i in 3:
		await tree.process_frame
	var comp: ShipComputer = scene.computer if scene.computer else ShipComputer.get_shared()
	scene.computer = comp
	comp.programs.start(scene, ("drive_to " + env.call("TO", "narwhal")).split(" "), s.telemetry())
	var p := comp.programs
	var t0 := s.time()
	var w0 := Time.get_ticks_msec()
	var said_t := -1e12
	var last := ""
	while p.running() and Time.get_ticks_msec() - w0 < 600000:
		await tree.process_frame
		if p.phase != last or s.time() - said_t > 20000.0:
			last = p.phase
			said_t = s.time()
			var t := s.telemetry()
			log.append("T+%9.0f %s | %s (%s, drive %.2f g, %.0f fps, %.2f Gm from the star, interstellar %s)" % [s.time() - t0, p.phase, p.note, t.get("body", ""), float(t.get("drive_accel", 0)) / 9.80665, Engine.get_frames_per_second(),
				Vector3(t.position[0], t.position[1], t.position[2]).length() / 1e9 if str(t.get("body", "")) == "lantern" else -1.0, t.get("interstellar", false)])
	var t := s.telemetry()
	for line in comp.log:
		log.append("    computer: " + str(line))
	log.append("ended %s: %s; at %s, %s; %.1f days of game time, %.0f s real" % [p.status, p.note, t.get("body", ""), t.get("situation", ""),
		(s.time() - t0) / 86400.0, (Time.get_ticks_msec() - w0) / 1000.0])
	return {"status": "PASS" if p.status == "done" else "FAIL", "log": "\n".join(log)}
