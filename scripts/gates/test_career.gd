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
## --test-career: the space program through the real flight scene. A new
## career starts with PASA's funds and Basic Rocketry; a craft with parts
## not yet researched can't launch; launching pays the craft's cost; a crew
## report is worth base x body x situation and can't be taken twice aboard;
## climbing past 1 km completes "First hop"; recovering banks the results
## (repeats then worth half) and refunds the parts; R&D spends research;
## a revert undoes the flight's results and payment; a penguin's surface
## sample comes aboard with it.
extends RefCounted

var _log := PackedStringArray()
var _failed := 0


func _check(ok: bool, msg: String) -> void:
	_log.append(("ok   " if ok else "FAIL ") + msg)
	if not ok:
		_failed += 1


func _near(a: float, b: float) -> bool:
	return absf(a - b) < 1e-6 * maxf(1.0, absf(b))


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	# an empty world (earlier gates leave craft about, which a launch would
	# recover and refund) and a brand new career
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	Sim.save_game()
	DirAccess.remove_absolute(ProjectSettings.globalize_path(Sim.save_dir + "career.json"))
	Career.load_from(Sim.save_dir + "career.json", "career")
	var sci: Dictionary = Career.science
	var start: float = Career.missions_def.start_funds
	_check(Career.is_career() and Career.funds == start and Career.unlocked == ["basics"] and Career.research == 0.0,
		"new career: %s, unlocked %s" % [Career.money(Career.funds), Career.unlocked])

	# 1. what can launch
	var hopper := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "hopper.craft.json")
	var orbiter := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	var hop_ids := Career.craft_parts(hopper)
	var hop_cost := 0.0
	for p in hop_ids:
		hop_cost += float(Sim.part(p).cost)
	var why_orb := Career.launch_refusal(orbiter)
	_check(Career.launch_refusal(hopper) == "" and why_orb.contains("isn't researched") and _near(Career.craft_cost(hop_ids), hop_cost),
		"Hopper may launch (costs %s, the sum of its %d parts); Orbiter refused ('%s')" % [Career.money(hop_cost), hop_ids.size(), why_orb])

	# 2. launch the Hopper: paid for
	var scene := await _flight(tree, hopper)
	var s: FlightSession = scene.session
	_check(_near(Career.funds, start - hop_cost), "launched: PASA has %s (%s - %s)" % [Career.money(Career.funds), Career.money(start), Career.money(hop_cost)])

	# 3. a crew report on the pad
	var t := s.telemetry()
	var pod_uid := ""
	for p in JSON.parse_string(s.core.vessel_parts(s.active)):
		if p.part == "pod_1":
			pod_uid = str(int(p.uid))
	var want_pad: float = sci.experiments.crew_report.base * sci.bodies.floe.value * sci.situations.landed.value
	var ex: Array = Career.experiments_on(s.active, t)
	var cr: Dictionary = ex.filter(func(x): return x.exp == "crew_report")[0]
	var r1 := Career.run(s.active, pod_uid, "crew_report", t)
	var r2 := Career.run(s.active, pod_uid, "crew_report", t)
	_check(Career.situation(t) == "landed" and t.get("biome", "") == "shelf" and _near(cr.value, want_pad) and r1 == "" and r2.contains("already")
		and Career.results_subjects(s.active) == [Career.subject("crew_report", "floe", "landed", "shelf")],
		"crew report on the pad: %s in %s, worth %.2f (%.0f x %.1f x %.1f), a second refused ('%s'); aboard %s" % [Career.situation(t), t.get("biome_name", "?"), cr.value,
			sci.experiments.crew_report.base, sci.bodies.floe.value, sci.situations.landed.value, r2, Career.results_subjects(s.active)])

	# 4. up past 1 km: First hop; a report flying low
	scene._throttle = 1.0
	scene._stage()
	var guard := 0
	while s.telemetry().altitude < 1100.0 and guard < 50 * 60:
		s.step_ticks(10)
		guard += 10
	scene._check_missions()
	t = s.telemetry()
	var r3 := Career.run(s.active, pod_uid, "crew_report", t)
	var want_low: float = sci.experiments.crew_report.base * sci.bodies.floe.value * sci.situations.flying_low.value
	var hop_m := Career.mission("hop")
	_check("hop" in Career.done and _near(Career.research, float(hop_m.research)) and _near(Career.funds, start - hop_cost + float(hop_m.funds)) and r3 == "",
		"at %.0f m: '%s' done (+%d research, +%s), crew report while %s recorded" % [t.altitude, hop_m.title, int(hop_m.research),
			Career.money(float(hop_m.funds)), Career.situation(t)])

	# 5. revert to launch: the flight's results and mission are undone, and
	#    the same rocket is paid for again
	scene._on_menu("revert_launch")
	await _frames(tree, 3)
	_check(Career.done.is_empty() and Career.results_on(s.active).is_empty() and _near(Career.funds, start - hop_cost) and Career.research == 0.0,
		"reverted: no missions done, nothing aboard, PASA has %s again" % Career.money(Career.funds))

	# 6. again: report on the pad and flying, then recover (straight from
	#    the world, as the Recover button does)
	for p in JSON.parse_string(s.core.vessel_parts(s.active)):
		if p.part == "pod_1":
			pod_uid = str(int(p.uid))
	Career.run(s.active, pod_uid, "crew_report", s.telemetry())
	scene._throttle = 1.0
	scene._stage()
	guard = 0
	while s.telemetry().altitude < 1100.0 and guard < 50 * 60:
		s.step_ticks(10)
		guard += 10
	scene._check_missions()
	Career.run(s.active, pod_uid, "crew_report", s.telemetry())
	var research_before := Career.research
	var funds_before := Career.funds
	# surveys wanting these results pay on top
	var aboard_now := Career.results_subjects(s.active)
	var bonus_r := 0.0
	var bonus_f := 0.0
	for sv0 in Career.surveys:
		if sv0.subject in aboard_now:
			bonus_r += float(sv0.research)
			bonus_f += float(sv0.funds)
	var parts_cost := 0.0
	for p in JSON.parse_string(s.core.vessel_parts(s.active)):
		parts_cost += float(Sim.part(p.part).cost)
	Sim.recover_vessel(s.active, {})
	var got: Dictionary = Sim.last_recovery
	var refund: float = parts_cost * float(Career.missions_def.recovery_refund)
	_check(int(got.results) == 2 and _near(float(got.research), want_pad + want_low) and _near(Career.research, research_before + want_pad + want_low + bonus_r)
		and _near(float(got.funds), refund) and _near(Career.funds, funds_before + refund + bonus_f),
		"recovered: 2 results +%.2f research (%.2f + %.2f), parts refunded %s (%.0f%% of %s on the craft)%s" % [got.research, want_pad, want_low,
			Career.money(refund), 100.0 * Career.missions_def.recovery_refund, Career.money(parts_cost),
			"; surveys paid +%.1f research, +%s" % [bonus_r, Career.money(bonus_f)] if bonus_r > 0 else ""])
	var again := Career.value("crew_report", "floe", "landed", "shelf")
	var elsewhere := Career.value("crew_report", "floe", "landed", "highlands")
	var high := Career.value("crew_report", "floe", "flying_high")
	_check(_near(again, want_pad * sci.repeat_factor) and _near(elsewhere, want_pad) and Career.describe("landed", "floe", "highlands") == "Landed on Floe (Highlands)",
		"the same report again would be worth %.3f (half of %.2f); landed in Floe's Highlands still %.2f; flying high is one subject over all Floe (%.2f)" % [again, want_pad, elsewhere, high])

	# 7. R&D
	var before := Career.research
	var cost: float = Career.tech_node("rocketry").cost
	var u1 := Career.unlock("rocketry")
	var u2 := Career.unlock("heavy")
	var u3 := Career.unlock("orbits")
	_check(u1 == "" and "rocketry" in Career.unlocked and _near(Career.research, before - cost) and Career.part_unlocked("tank_l")
		and u2.contains("first") and (u3.contains("research") or u3 == ""),
		"R&D: Bigger Rockets for %d (research %.2f -> %.2f), tank_l unlocked; Heavy Lifting refused ('%s'); Getting to Orbit: '%s'" % [
			int(cost), before, Career.research, u2, u3 if u3 != "" else "researched"])

	# 8. a penguin's surface sample comes aboard with it
	scene = await _flight(tree, hopper)
	s = scene.session
	var hop := s.active
	scene._key(_ev(KEY_F))
	var pen := s.active
	s.step_ticks(60)
	var pen_uid := str(int(JSON.parse_string(s.core.vessel_parts(pen))[0].uid))
	t = s.telemetry()
	var samp: Array = Career.experiments_on(pen, t).filter(func(x): return x.exp == "surface_sample")
	var want_s: float = sci.experiments.surface_sample.base * sci.bodies.floe.value * sci.situations.landed.value
	var rs := Career.run(pen, pen_uid, "surface_sample", t)
	scene._key(_ev(KEY_F))
	var aboard := Career.results_on(hop)
	_check(not samp.is_empty() and _near(samp[0].value, want_s) and rs == "" and s.active == hop and aboard.size() == 1 and aboard[0].exp == "surface_sample",
		"EVA surface sample worth %.1f, carried back in: Hopper holds %s" % [want_s, aboard.map(func(r): return r.subject)])
	# 8b. surveys: three standing contracts; bringing one's result home
	#     pays its funds and research on top of the result's worth, and a
	#     new one takes its place
	var sv: Array = Career.surveys.duplicate(true)
	var done0 := Career.surveys_done
	var want_s0: Dictionary = sv[0]
	var all_floe := sv.all(func(x): return x.body == "floe")
	scene = await _flight(tree, hopper)
	s = scene.session
	for p in JSON.parse_string(s.core.vessel_parts(s.active)):
		if p.part == "pod_1":
			pod_uid = str(int(p.uid))
	Career.aboard[pod_uid] = [{"subject": want_s0.subject, "exp": want_s0.exp, "body": want_s0.body, "situation": want_s0.situation, "biome": want_s0.get("biome", "")}]
	var f0 := Career.funds
	var r0s := Career.research
	var worth_s := Career.value_of(want_s0)
	var refund_s := 0.0
	for p in JSON.parse_string(s.core.vessel_parts(s.active)):
		refund_s += float(Sim.part(p.part).cost) * float(Career.missions_def.recovery_refund)
	Sim.recover_vessel(s.active, {})
	var subjects := Career.surveys.map(func(x): return x.subject)
	_check(sv.size() == 3 and all_floe and _near(Career.funds - f0, float(want_s0.funds) + refund_s) and _near(Career.research - r0s, worth_s + float(want_s0.research))
		and Career.surveys.size() == 3 and not want_s0.subject in subjects and Career.surveys_done == done0 + 1,
		"surveys: %s; brought home '%s': +%s (and the parts' refund) and %.2f + %.1f research; a new one offered (%d standing)" % [
			sv.map(func(x): return Career.describe_survey(x)), Career.describe_survey(want_s0), Career.money(float(want_s0.funds)),
			worth_s, want_s0.research, Career.surveys.size()])

	# 9. an antenna sends a result home: half its worth, for kJ per point
	var m := CraftModel.new()
	m.load_text(hopper)
	var tank := -1
	for i in m.parts.size():
		if m.parts[i].part == "tank_m":
			tank = i
	m.attach_surface(CraftModel.held_single("antenna"), tank, 0.0, 0.0, 1)
	Career.unlocked.append_array(["orbits", "power"])
	scene = await _flight(tree, m.save_text())
	s = scene.session
	for p in JSON.parse_string(s.core.vessel_parts(s.active)):
		if p.part == "pod_1":
			pod_uid = str(int(p.uid))
	t = s.telemetry()
	Career.run(s.active, pod_uid, "crew_report", t)
	var subj := Career.subject("crew_report", "floe", "landed", "shelf")
	var worth_now := Career.value("crew_report", "floe", "landed", "shelf")
	var r0 := Career.research
	var e0: float = t.ec
	var sent := Career.transmit(s.active, subj, s.telemetry())
	var t2: Dictionary = JSON.parse_string(s.core.telemetry(s.active))  # fresh (no tick has passed)
	var want_pts: float = worth_now * float(Sim.part("antenna").power.antenna_fraction)
	var want_kj: float = want_pts * float(Sim.part("antenna").power.antenna_kj_per_point)
	_check(sent.ok and _near(Career.research - r0, want_pts) and _near(e0 - float(t2.ec), want_kj) and Career.results_on(s.active).is_empty(),
		"antenna: sent the pad crew report for +%.3f research (half of %.3f) and %.2f kJ (%.0f kJ per point); nothing left aboard" % [
			Career.research - r0, worth_now, e0 - float(t2.ec), Sim.part("antenna").power.antenna_kj_per_point])
	# (the file keeps the mode it was saved in: a sandbox again needs a new one)
	DirAccess.remove_absolute(ProjectSettings.globalize_path(Sim.save_dir + "career.json"))
	Career.load_from(Sim.save_dir + "career.json", "sandbox")
	return {"status": "PASS" if _failed == 0 else "FAIL", "log": "\n".join(_log)}


func _ev(code: Key, down := true) -> InputEventKey:
	var e := InputEventKey.new()
	e.keycode = code
	e.physical_keycode = code
	e.pressed = down
	return e


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
