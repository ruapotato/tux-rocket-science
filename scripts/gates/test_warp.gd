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
## --test-warp: on-rails time warp never runs a craft into the ground and
## never pretends to burn. Falling on Krill it eases off level by level,
## always keeping WARP_LEAD seconds of real time before the ground, and is
## back to normal time while the craft is still high and whole (a flyby
## that clears the moon is never slowed); throttling
## up during warp hands back to normal time so the engine really burns;
## slowing down is always allowed, engines or not; and a skip ahead stops
## a minute short of an impact on the way; a craft resting on another
## world doesn't hold warp up.
extends RefCounted

var _log := PackedStringArray()
var _failed := 0


func _check(ok: bool, msg: String) -> void:
	_log.append(("ok   " if ok else "FAIL ") + msg)
	if not ok:
		_failed += 1


func run(_runner: Node) -> Dictionary:
	var dt := 1.0 / 60.0
	# 1. dropping onto Krill from 150 km at 200 m/s, asking for 100000x
	var s := FlightSession.new()
	var lander := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "krill_lander.craft.json")
	s.spawn_in_orbit(lander, "krill", 150000.0)
	var R: float = JSON.parse_string(s.core.body_info(2)).radius
	s.core.vessel_remove(s.active)
	s.switch_to(s.core.flight_spawn_state(lander, "krill", PackedFloat64Array([R + 150000.0, 0, 0, -200.0, 0, 60.0])))
	var asked := s.set_warp("rails", FlightSession.RAILS_WARP.size() - 1)
	var first := s.warp_index
	var worst := INF
	var levels := [FlightSession.RAILS_WARP[first]]
	var frames := 0
	while s.warp_index > 0 and frames < 60 * 600:
		var t := s.telemetry()
		if t.vertical_speed < 0.0:
			worst = minf(worst, t.altitude_terrain / -t.vertical_speed / FlightSession.RAILS_WARP[s.warp_index])
		s.advance(dt)
		frames += 1
		if s.warp_index > 0 and FlightSession.RAILS_WARP[s.warp_index] != levels[-1]:
			levels.append(FlightSession.RAILS_WARP[s.warp_index])
	var tf := s.telemetry()
	var eased := s.events.filter(func(e): return e.kind == "warp_eased").size()
	_check(asked == "" and first < FlightSession.RAILS_WARP.size() - 1,
		"asked for %dx falling at Krill: given %dx" % [FlightSession.RAILS_WARP[-1], FlightSession.RAILS_WARP[first]])
	_check(s.warp_index == 0 and tf.alive and not tf.landed and tf.altitude_terrain > 1000.0 and worst >= FlightSession.WARP_LEAD - 0.5 and eased >= 2,
		"eased %s x then normal time at %.1f km up, falling %.0f m/s (%d eases; never under %.1f s of real time from the ground, keeps %.0f)" % [
			"x, ".join(levels.map(func(x): return str(int(x)))), tf.altitude_terrain / 1000.0, -tf.vertical_speed, eased, worst, FlightSession.WARP_LEAD])
	var again := s.set_warp("rails", 5)
	_check(again != "" or s.warp_index <= 1, "warping again that close: '%s' (level %d)" % [again, s.warp_index])

	# a flyby whose low point clears Krill by 40 km is never slowed
	s.core.vessel_remove(s.active)
	var mu: float = JSON.parse_string(s.core.body_info(2)).mu
	var rp := R + 40000.0
	var vp := sqrt(mu * (2.0 / rp) + 400.0 * 400.0)  # hyperbolic, 400 m/s excess
	var r0 := R + 400000.0
	var v0 := sqrt(vp * vp - 2.0 * mu * (1.0 / rp - 1.0 / r0))
	var vt := vp * rp / r0
	s.switch_to(s.core.flight_spawn_state(lander, "krill", PackedFloat64Array([r0, 0, 0, -sqrt(v0 * v0 - vt * vt), 0, vt])))
	var fl := s.set_warp("rails", 4)
	var lowest := INF
	for i in 60 * 20:
		s.advance(dt)
		lowest = minf(lowest, s.telemetry().altitude)
	_check(fl == "" and s.warp_kind == "rails" and s.warp_index == 4 and absf(lowest - 40000.0) < 2000.0,
		"flyby 40 km over Krill at 100x: never slowed (level %d), lowest %.1f km" % [s.warp_index, lowest / 1000.0])
	s.set_warp("rails", 0)

	# skipping ahead (the map's "skip to burn") past where the path hits
	# Krill stops a minute short of the ground instead
	s.core.vessel_remove(s.active)
	s.switch_to(s.core.flight_spawn_state(lander, "krill", PackedFloat64Array([R + 60000.0, 0, 0, -50.0, 0, 300.0])))
	var t_hit := -1.0
	for p in s.trajectory([], 4, 1.0e5):
		if p.end == "surface":
			t_hit = float(p.t_end)
	var skip_why := s.warp_to(s.time() + 1.0e5)
	var ts := s.telemetry()
	_check(t_hit > 0.0 and ts.alive and not ts.landed and absf(t_hit - s.time() - FlightSession.WARP_TO_LEAD) < 1.0 and skip_why.contains("ground"),
		"skip ahead past an impact: stopped %.1f s before it, %.1f km up ('%s')" % [t_hit - s.time(), ts.altitude_terrain / 1000.0, skip_why])

	# 2. the Orbiter in orbit, engine lit at zero throttle, warping
	s = FlightSession.new()
	var orbiter := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	s.spawn_in_orbit(orbiter, "floe", 100000.0)
	s.set_throttle(0.0)
	s.stage()
	var w := s.set_warp("rails", 4)
	_check(w == "" and s.warp_kind == "rails" and s.warp_index == 4, "engine lit at zero throttle: warp 100x allowed ('%s')" % w)
	# slowing down is allowed even with the throttle up
	s.set_throttle(1.0)
	var slow := s.set_warp("rails", 3)
	_check(slow == "" and s.warp_index == 3, "throttle up mid-warp: slowing to 50x still allowed ('%s', level %d)" % [slow, s.warp_index])
	var fuel0: float = s.telemetry().liquid
	var t0 := s.time()
	s.events.clear()
	s.advance(dt)
	var stopped := s.events.filter(func(e): return e.kind == "warp_stopped" and e.reason == "engines firing").size()
	_check(s.warp_kind == "physics" and s.warp_index == 0 and stopped == 1 and s.time() - t0 < 0.05,
		"next frame: back to normal time (%s %d), told why (%d), no warped time passed (%.3f s)" % [s.warp_kind, s.warp_index, stopped, s.time() - t0])
	for i in 60:
		s.advance(dt)
	var tb := s.telemetry()
	_check(tb.liquid < fuel0 and tb.thrust > 0.0, "and the engine really burns: %.1f -> %.1f kg in %.2f s, thrust %.0f kN" % [fuel0, tb.liquid, s.time() - t0, tb.thrust / 1000.0])
	var refuse := s.set_warp("rails", 2)
	_check(refuse != "" and s.warp_index == 0, "asking to warp under thrust: refused ('%s')" % refuse)
	# 3. stepping up the ladder: under thrust it tops out at 4x; with the
	#    engine idle the next step up from 4x goes on to the rails rates
	var climb := []
	var why := ""
	for i in 6:
		why = s.step_warp(1)
		climb.append("%s %d" % [s.warp_kind, s.warp_index])
	_check(s.warp_kind == "physics" and s.warp_index == FlightSession.PHYSICS_WARP.size() - 1 and why.contains("thrust"),
		"stepping up under thrust: %s, stops at 4x ('%s')" % [", ".join(climb), why])
	s.set_throttle(0.0)
	for i in 3:
		s.advance(dt)
	why = s.step_warp(1)
	_check(why == "" and s.warp_kind == "rails" and s.warp_index == 1,
		"engine idle, one more step up from 4x: %s %dx ('%s')" % [s.warp_kind, FlightSession.RAILS_WARP[s.warp_index], why])
	s.step_warp(1)
	_check(s.warp_kind == "rails" and s.warp_index == 2, "and on up: %dx" % FlightSession.RAILS_WARP[s.warp_index])
	# 4. a planned burn moved by whole orbits: later by exactly the period
	#    of the orbit it sits on, back again, and not into the past
	s = FlightSession.new()
	s.spawn_in_orbit(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json"), "floe", 120000.0)
	var period: float = s.telemetry().orbit.period
	var t_node := s.time() + 300.0
	s.nodes = [[t_node, 40.0, 0.0, 0.0]]
	var i1 := s.shift_node(0, 1)
	var later: float = s.nodes[0][0]
	var i2 := s.shift_node(0, -1)
	var back: float = s.nodes[0][0]
	var i3 := s.shift_node(0, -1)
	_check(i1 == 0 and i2 == 0 and absf(later - t_node - period) < 1e-6 and absf(back - t_node) < 1e-6 and i3 == -1 and s.shift_why.contains("behind"),
		"node 300 s ahead: an orbit later at +%.3f s (period %.3f), back again (%+.6f s), an orbit earlier refused ('%s')" % [
			later - t_node, period, back - t_node, s.shift_why])
	# 5. something left landed elsewhere (a lander, a flag, a mining base)
	#    doesn't stop warp: it rests where it is while the rest moves on
	var pod := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "hopper.craft.json")
	var lid: int = s.core.flight_place_landed(pod, "krill", 5.0, 40.0)
	var tl0: Dictionary = JSON.parse_string(s.core.telemetry(lid))
	var w0 := s.time()
	var why_r := s.set_warp("rails", 3)
	for i in 30:
		s.advance(dt)
	var skipped := s.warp_to(s.time() + 3600.0)
	var tl1: Dictionary = JSON.parse_string(s.core.telemetry(lid))
	var gone := s.time() - w0
	s.switch_to(lid)
	var w1 := s.time()
	var skipped2 := s.warp_to(s.time() + 7200.0)
	var tl2: Dictionary = JSON.parse_string(s.core.telemetry(lid))
	_check(skipped2 == "" and absf(s.time() - w1 - 7200.0) < 1e-6 and tl2.landed and absf(float(tl2.lat_deg) - float(tl0.lat_deg)) < 1e-9,
		"flying the resting Hopper: a two-hour skip runs the full two hours ('%s', %.1f s), still landed" % [skipped2, s.time() - w1])
	_check(why_r == "" and gone > 3600.0 + 30 * dt * 10.0 and skipped == "" and tl1.landed and
		absf(float(tl1.lat_deg) - float(tl0.lat_deg)) < 1e-9 and absf(float(tl1.lon_deg) - float(tl0.lon_deg)) < 1e-9,
		"with a Hopper resting on Krill: rails warp and an hour's skip run on (%.0f s gone, '%s'); the Hopper still landed at %.6f, %.6f" % [
			gone, skipped, float(tl1.lat_deg), float(tl1.lon_deg)])
	return {"status": "PASS" if _failed == 0 else "FAIL", "log": "\n".join(_log)}
