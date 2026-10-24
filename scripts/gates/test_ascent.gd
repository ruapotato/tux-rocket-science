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
## --test-ascent: the stock Orbiter and Sidecar each fly from the launch pad to a stable
## orbit (periapsis > 75 km) under a scripted autopilot, blind, through the
## control interface.
extends RefCounted


func run(_runner: Node) -> Dictionary:
	var logs := PackedStringArray()
	var ok := true
	# the Sidecar flies its side tanks dry through feed lines on the way up;
	# the Rookery Heavy lifts on a wide tank and three engines on a mount
	for craft in ["orbiter", "sidecar", "rookery_heavy"]:
		var r := _fly(craft)
		ok = ok and r.status == "PASS"
		logs.append("%s %s\n%s" % [craft, r.status, r.log])
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(logs)}


func _fly(craft: String) -> Dictionary:
	var api := ControlAPI.new()
	var r := api.handle({"cmd": "load_craft", "path": craft})
	if not r.ok:
		return {"status": "FAIL", "log": r.error}
	r = api.handle({"cmd": "launch", "body": "floe"})
	if not r.ok:
		return {"status": "FAIL", "log": r.error}
	var t0: Dictionary = api.handle({"cmd": "query"}).telemetry
	var ap := AscentAutopilot.new(api)
	ap.log.append("launch: %s, %.2f t on the pad at %.0f m, %s" % [t0.name, t0.mass / 1000.0, t0.altitude, t0.situation])
	var step := 5  # control every 0.1 s
	var done := false
	var guard := 0
	while not done and guard < 50 * 60 * 40:
		done = ap.step(step * FlightSession.TICK)
		var tel: Dictionary = api.session.telemetry()
		# coast phases above the air are warped on rails, as a player would
		if ap.phase == "coast" and not tel.in_atmosphere and tel.orbit.t_to_ap != null and tel.orbit.t_to_ap > 60.0:
			api.handle({"cmd": "warp", "kind": "rails", "index": 3})
			api.handle({"cmd": "advance", "seconds": 0.5})
			api.handle({"cmd": "warp", "kind": "rails", "index": 0})
			guard += 1
			continue
		api.handle({"cmd": "step", "ticks": step})
		guard += step
	# let it coast an orbit to prove it is stable
	var tf: Dictionary = api.handle({"cmd": "query"}).telemetry
	var orb: Dictionary = tf.orbit
	var ok: bool = ap.phase == "done" and tf.situation == "orbiting" and orb.pe_alt > 75000.0
	var w := api.handle({"cmd": "warp", "kind": "rails", "index": 5})
	var tw0: float = tf.t
	for i in 40:
		api.handle({"cmd": "advance", "seconds": 0.1})
	api.handle({"cmd": "warp", "kind": "rails", "index": 0})
	var t2: Dictionary = api.handle({"cmd": "query"}).telemetry
	ok = ok and w.ok and t2.situation == "orbiting" and absf(t2.orbit.pe_alt - orb.pe_alt) < 1.0
	ok = ok and ap.max_heat < 0.8  # a clean ascent never comes near a heat limit
	ap.log.append("final: %s, Pe %.2f km, Ap %.2f km, inc %.2f deg, %.0f m/s dv left, max Q %.1f kPa, peak heat %.0f%% of a part's heat headroom (%s)" % [
		tf.situation, orb.pe_alt / 1000.0, orb.ap_alt / 1000.0, orb.inc_deg, tf.dv_total, ap.max_q / 1000.0, 100.0 * ap.max_heat, ap.hot_part])
	ap.log.append("after %.0f s on rails: Pe %.2f km (%s)" % [t2.t - tw0, t2.orbit.pe_alt / 1000.0, t2.situation])
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(ap.log)}
