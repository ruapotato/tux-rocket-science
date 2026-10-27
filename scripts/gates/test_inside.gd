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
## --test-inside: walking about inside a ship through the real flight
## scene. A career can't use the Orbital Yard until Orbital Shipyard is
## researched; the Waddle Ark starts there in a circular orbit; Enter puts
## one of its crew in a hab, weightless (and the hint says so); under
## thrust the penguin falls aft and the hint's weight is thrust / mass;
## Home spins the ship up to its ring's design rate and the deck weighs
## w^2 r as the hint says, the ring's seats too (their crew no longer
## "weightless"); the Stillwater Drive pushing at 1 g in the heliopause
## changes nothing anyone feels; Enter goes back to the controls.
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
	Sim.save_game()
	DirAccess.remove_absolute(ProjectSettings.globalize_path(Sim.save_dir + "career.json"))
	Career.load_from(Sim.save_dir + "career.json", "career")
	var locked := not Career.site_unlocked("orbit")
	Career.unlocked.append_array(["rocketry", "orbits", "staging", "heavy", "habitation"])
	Career.research = 100.0
	var why := Career.unlock("yard")
	_check(locked and why == "" and Career.site_unlocked("orbit"), "the Orbital Yard is closed to a new career, open once Orbital Shipyard is researched ('%s')" % why)
	Career.load_from(Sim.save_dir + "career.json", "sandbox")

	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			c.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "waddle_ark.craft.json")
	Sim.pending_site = "orbit"
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var t := s.telemetry()
	_check(absf(float(t.altitude) - FlightSession.YARD_ALT) < 1.0 and t.situation == "orbiting" and float(t.orbit.ecc) < 1e-6,
		"the Ark starts in the Orbital Yard: %.3f km up, %s, e %s" % [float(t.altitude) / 1000.0, t.situation, str(float(t.orbit.ecc))])

	# 1. inside, weightless
	scene._toggle_inside()
	s.step_ticks(5)
	await _frames(tree, 3)
	t = s.telemetry()
	var wk: Dictionary = t.get("walker", {})
	_check(not wk.is_empty() and float(wk.felt_g) < 1e-9 and scene.hud._hint.text.contains("weightless"),
		"Enter: %s floats in the %s, feeling %s g; hint '%s'" % [scene._in_who, wk.get("room_name", "?"), str(float(wk.get("felt_g", 0.0))), scene.hud._hint.text])

	# 2. the engine lit: weight toward the stern, what thrust / mass says
	scene._throttle = 1.0
	scene._stage()
	for k in 40:
		s.step_ticks(5)
		await _frames(tree, 1)
	t = s.telemetry()
	wk = t.walker
	var want: float = float(t.thrust) / float(t.mass) / 9.80665
	var shown := "%.2f g" % want
	_check(wk.standing and absf(float(wk.felt_g) - want) < 1e-3 * want and float(wk.felt[1]) < 0.0 and scene.hud._hint.text.contains(shown),
		"under thrust: stands on an aft wall in the %s feeling %.4f g, thrust / mass %.4f g; hint shows %s" % [wk.room_name, float(wk.felt_g), want, shown])
	scene._throttle = 0.0
	s.set_throttle(0.0)
	await _frames(tree, 2)

	# 3. spin hold: up to the ring's rate, then the deck
	scene._toggle_spin()
	var ticks := 0
	while ticks < 50 * 150:
		s.step_ticks(50)
		ticks += 50
		t = s.telemetry()
		if absf(float(t.spin_rate) - float(t.spin_design)) < 1e-4 * float(t.spin_design):
			break
	var w: float = t.spin_rate
	_check(absf(w - float(t.spin_design)) < 1e-4 * w and float(t.spin_hold) == float(t.spin_design),
		"Home: spun up to %.4f rad/s (%.2f rpm, the ring's design) in %.1f s" % [w, w * 60.0 / TAU, ticks / 50.0])
	var parts: Array = JSON.parse_string(s.core.vessel_parts(s.active))
	var ring := -1
	for k in parts.size():
		if parts[k].part == "spin_ring":
			ring = k
	s.core.walker_leave(s.active)
	s.core.walker_enter(s.active, ring)
	for k in 30:
		s.step_ticks(5)
		await _frames(tree, 1)
	t = s.telemetry()
	wk = t.walker
	var x := DMath.to_v3(wk.x) - DMath.to_v3(wk.cm)
	var rho := Vector2(x.x, x.z).length()
	var wnow: float = t.spin_rate
	var deck: float = w * w * rho / 9.80665
	_check(wk.standing and absf(float(wk.felt_g) - wnow * wnow * rho / 9.80665) < 1e-6 and scene.hud._hint.text.contains("%.2f g" % float(wk.felt_g)) and scene.hud._hint.text.contains("rpm"),
		"on the deck %.3f m from the axis: feels %.4f g = w^2 r %.4f g; hint '%s'" % [rho, float(wk.felt_g), deck, scene.hud._hint.text])
	var seat_g: Dictionary = t.get("seat_g", {})
	var ring_uid := str(int(parts[ring].uid))
	var aboard: Array = Sim.seats.get(ring_uid, [])
	var moods := []
	for p in scene._portraits:
		if p.member.get("name", "") in aboard:
			moods.append(p.mood)
	_check(float(seat_g.get(ring_uid, 0.0)) > 0.9 and not aboard.is_empty() and not "weightless" in moods,
		"the ring's seats weigh %.3f g: its %d crew are %s, not weightless" % [float(seat_g.get(ring_uid, 0.0)), aboard.size(), moods])

	# the Stillwater Drive on: a 1 g push in the heliopause, and the deck
	# weighs just what the spin gives
	var before: float = wk.felt_g
	s.core.vessel_set_drive(s.active, 1.0)  # (the throttle drives it; no frames, so nothing resets it)
	s.step_ticks(50)
	t = s.telemetry()
	wk = t.walker
	_check(float(t.get("drive_accel", 0.0)) > 9.8 and absf(float(wk.felt_g) - before) < 1e-6,
		"Stillwater Drive at %.2f m/s2: the deck still weighs %.4f g (was %.4f)" % [float(t.get("drive_accel", 0.0)), float(wk.felt_g), before])
	s.core.vessel_set_drive(s.active, 0.0)

	# 4. Enter: back at the controls
	scene._toggle_inside()
	await _frames(tree, 3)
	t = s.telemetry()
	_check(scene._inside == null and not t.has("walker"), "Enter again: back at the controls, nobody walking about")
	# leave the sandbox world empty for the gates after this one
	scene.queue_free()
	await _frames(tree, 2)
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	Sim.save_game()
	return {"status": "FAIL" if _failed else "PASS", "log": "\n".join(_log)}
