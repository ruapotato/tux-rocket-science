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
## --test-signals: the alien signal chain through the real flight scene. A
## Signal Scanner in orbit of Krill hears the Chime exactly when it is in
## sight and in range, at the distance and heading the round-world formulas
## give; hearing it completes "The Hum" and charts it; a penguin on foot
## 60 m from it hasn't investigated it, one 15 m from it has ("The Chime");
## the Kelp Hearth opens its R&D node for free in a career, and nothing
## else does.
extends RefCounted

var _log := PackedStringArray()
var _failed := 0


func _check(ok: bool, msg: String) -> void:
	_log.append(("ok   " if ok else "FAIL ") + msg)
	if not ok:
		_failed += 1


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	Sim.save_game()
	DirAccess.remove_absolute(ProjectSettings.globalize_path(Sim.save_dir + "career.json"))
	Career.load_from(Sim.save_dir + "career.json", "career")
	Career.done = ["hop", "space", "orbit"]
	Career.unlocked.append_array(["rocketry", "orbits", "power", "control"])
	Career.research = 50.0
	var active := Career.active_missions().map(func(m): return m.id)
	var why_hearth := Career.tech_refusal("hearth")
	_check("hum" in active and not "chime" in active and why_hearth.contains("found") and Career.unlock("hearth") != "",
		"after orbit: 'The Hum' on offer, 'The Chime' not yet; The Hearth can't be bought ('%s')" % why_hearth)
	_check(Career.unlock("listening") == "" and Career.part_unlocked("signal_scanner") and not Career.part_unlocked("hearth_core"),
		"Listening Post researched: the scanner is unlocked, the Hearth Core isn't")

	# 1. a scanner probe in a 100 km orbit of Krill, tipped to pass over the Chime
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("probe_core"))
	m.attach_surface(CraftModel.held_single("battery"), 0, PI, 0.0, 1)
	m.attach_surface(CraftModel.held_single("signal_scanner"), 0, 0.0, 0.0, 1)
	var scene := await _flight(tree, m.save_text())
	var s: FlightSession = scene.session
	var core: TrsSim = s.core
	var kb := core.body_find("krill")
	var kinfo: Dictionary = JSON.parse_string(core.body_info(kb))
	var relic: Dictionary = kinfo.relics[0]
	var probe: int = core.flight_spawn_orbit(m.save_text(), "krill", 100000.0, 12.0)
	s.switch_to(probe)
	await _frames(tree, 3)
	var t := s.telemetry()
	_check(m.parts.size() == 3 and is_equal_approx(float(t.get("scanner_range", 0.0)), 3.0e6) and t.get("scanner_powered", false) and (t.get("relics", []) as Array).size() == 1,
		"probe core, battery and scanner: powered, hears %s km; Krill has %d relic (%s)" % [float(t.get("scanner_range", 0.0)) / 1000.0, t.get("relics", []).size(), relic.name])
	var r0 := Career.relic_in(t, "krill_chime")
	var period: float = t.orbit.period
	var heard_at := -1.0
	var mismatch := 0
	var worst_d := 0.0
	var worst_b := 0.0
	var n_heard := 0
	var unpowered := 0
	var n_hidden := 0
	var t0 := s.time()
	while s.time() - t0 < period * 1.05:
		var before := s.time()
		var why := s.warp_to(before + 20.0)
		await tree.process_frame
		if s.time() <= before:
			_check(false, "warp refused at T+%.0f: '%s'" % [before - t0, why])
			break
		t = s.telemetry()
		var r := Career.relic_in(t, "krill_chime")
		# the same geometry from telemetry alone, in GDScript doubles
		var R: float = kinfo.radius
		var la := deg_to_rad(float(t.lat_deg))
		var lo := deg_to_rad(float(t.lon_deg))
		var rv: float = R + float(t.altitude)
		var p := [rv * cos(la) * cos(lo), rv * cos(la) * sin(lo), rv * sin(la)]
		var rl := deg_to_rad(float(relic.lat_deg))
		var ro := deg_to_rad(float(relic.lon_deg))
		var rr: float = R + float(relic.height)
		var q := [rr * cos(rl) * cos(ro), rr * cos(rl) * sin(ro), rr * sin(rl)]
		var dist: float = sqrt(pow(p[0] - q[0], 2) + pow(p[1] - q[1], 2) + pow(p[2] - q[2], 2))
		var dl := ro - lo
		var brg := fposmod(atan2(sin(dl) * cos(rl), cos(la) * sin(rl) - sin(la) * cos(rl) * cos(dl)), TAU)
		var up_dot: float = ((p[0] - q[0]) * q[0] + (p[1] - q[1]) * q[1] + (p[2] - q[2]) * q[2]) / rr  # height over the relic's horizon
		worst_d = maxf(worst_d, absf(float(r.distance) - dist) / dist)
		worst_b = maxf(worst_b, absf(angle_difference(deg_to_rad(float(r.bearing_deg)), brg)))
		var want: bool = up_dot > 0.0 and dist <= 3.0e6 and t.scanner_powered
		unpowered += 0 if t.scanner_powered else 1
		if r.heard != want and absf(up_dot) > 1.0:
			mismatch += 1
		if r.heard:
			n_heard += 1
		else:
			n_hidden += 1
		if r.heard and heard_at < 0.0:
			heard_at = s.time() - t0
			scene._check_missions()
			await _frames(tree, 2)
	_check(r0.has("distance") and n_hidden > 10 and n_heard > 10 and heard_at >= 0.0 and unpowered == 0 and mismatch == 0 and worst_d < 1e-9 and worst_b < 1e-6,
		"one orbit, every 20 s: heard %d times, hidden behind Krill %d; powered throughout (%d samples not); heard exactly when above its horizon and in range (%d mismatches); distance and heading against the formulas: worst %s, %s rad" % [
			n_heard, n_hidden, unpowered, mismatch, str(worst_d), str(worst_b)])
	# the HUD while the Chime is in sight (the orbit may end with Krill
	# between them, wherever the world's clock has got to)
	for k in 60:
		if Career.relic_in(s.telemetry(), "krill_chime").get("heard", false):
			break
		s.warp_to(s.time() + 20.0)
		await tree.process_frame
	await _frames(tree, 2)
	var sig: String = scene.hud._signal.text
	_check("krill_chime" in Career.heard and "hum" in Career.done and scene.hud.story_showing() and sig.contains("Krill Chime") and sig.contains("signal!"),
		"'The Hum' done, the Chime charted, its story told; the HUD reads '%s'" % sig.replace("\n", " | "))

	# 2. a penguin by the Chime: 60 m away it hasn't investigated, 15 m it has
	var pod := CraftModel.new()
	pod.place_root(CraftModel.held_single("pod_1"))
	pod.attach_stack(CraftModel.held_single("tank_s"), 0, "bottom")
	for d in [60.0, 15.0]:
		var dlat := rad_to_deg(d / float(kinfo.radius))
		var lid: int = core.flight_place_landed(pod.save_text(), "krill", float(relic.lat_deg) + dlat + 4.0 / float(kinfo.radius), float(relic.lon_deg))
		s.switch_to(lid)
		await _frames(tree, 3)
		var eva: int = core.vessel_eva_exit(lid, 0)
		s.switch_to(eva)
		for k in 30:
			s.step_ticks(5)
			await tree.process_frame
		t = s.telemetry()
		scene._check_missions()
		var rr := Career.relic_in(t, "krill_chime")
		var did: bool = "chime" in Career.done
		_check(t.eva and t.landed and did == (float(rr.surface_distance) <= Career.INVESTIGATE_M) and did == (d < 30.0),
			"penguin standing %.1f m from the Chime (along the ground): %s" % [float(rr.surface_distance), "investigated: 'The Chime' done" if did else "not close enough yet"])
	var hud_txt: String = scene.hud._signal.text
	_check(hud_txt.contains("charted") and not hud_txt.contains("SCANNER"),
		"on foot with no scanner: the charted Chime still shows ('%s')" % hud_txt.replace("\n", " | "))

	# 3. the Hearth on Kelp opens its R&D node, free
	active = Career.active_missions().map(func(x): return x.id)
	_check("hearth_hear" in active and "keel_hear" in active, "the Chime answers: Kelp's and Narwhal's tones on offer")
	Career.heard.append("kelp_hearth")
	scene._check_missions()
	var kelp: Dictionary = JSON.parse_string(core.body_info(core.body_find("kelp")))
	var hr: Dictionary = kelp.relics[0]
	var research_before := Career.research
	var done_before := Career.done.duplicate()
	var lid2: int = core.flight_place_landed(pod.save_text(), "kelp", float(hr.lat_deg) + rad_to_deg(12.0 / float(kelp.radius)), float(hr.lon_deg))
	s.switch_to(lid2)
	await _frames(tree, 3)
	var eva2: int = core.vessel_eva_exit(lid2, 0)
	s.switch_to(eva2)
	for k in 30:
		s.step_ticks(5)
		await tree.process_frame
	scene._check_missions()
	var gained := 0.0  # every mission this landing completed ("To Kelp" too)
	for id in Career.done:
		if not id in done_before:
			gained += float(Career.mission(id).research)
	_check("hearth" in Career.done and "hearth" in Career.unlocked and Career.part_unlocked("hearth_core") and not Career.part_unlocked("stillwater_drive") and
		is_equal_approx(Career.research - research_before, gained),
		"a penguin at the Kelp Hearth: 'The Hearth' done, +%.0f research (the missions' sum), R&D 'The Hearth' open (Hearth Core %s, Stillwater Drive %s)" % [
			Career.research - research_before, "unlocked" if Career.part_unlocked("hearth_core") else "LOCKED",
			"unlocked" if Career.part_unlocked("stillwater_drive") else "locked"])
	DirAccess.remove_absolute(ProjectSettings.globalize_path(Sim.save_dir + "career.json"))
	Career.load_from(Sim.save_dir + "career.json", "sandbox")
	return {"status": "PASS" if _failed == 0 else "FAIL", "milestone": "M24", "log": "\n".join(_log)}


func _flight(tree: SceneTree, text: String) -> Node3D:
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	await _frames(tree, 2)
	Sim.tutorial = false
	Sim.pending_craft_text = text
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	return scene


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame
