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
## Dev: what a very big rocket costs per frame (editor stats, telemetry,
## render feed, physics on the pad). Its 60 side engines fire down the core,
## so it cooks itself in a couple of seconds and goes up all at once: every
## part must be accounted for in the events.
extends RefCounted


func run(_runner: Node) -> Dictionary:
	var log := PackedStringArray()
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	var last := 0
	var tanks := []
	for k in 10:
		last = m.attach_stack(CraftModel.held_single("tank_l"), last, "bottom")[0]
		tanks.append(last)
	m.attach_stack(CraftModel.held_single("engine_lifter"), last, "bottom")
	for t in tanks:
		var decs := m.attach_surface(CraftModel.held_single("decoupler_radial"), t, 0.0, 0.0, 6)
		# symmetry: one attach puts a copy on each of the six decouplers
		var side: int = m.attach_stack(CraftModel.held_single("tank_m"), decs[0], "outer")[0]
		m.attach_stack(CraftModel.held_single("engine_small"), side, "bottom")
		m.attach_surface(CraftModel.held_single("feed_line"), side, PI, 0.6, 1)
	var t0 := Time.get_ticks_usec()
	var st := m.stats()
	var t_stats := (Time.get_ticks_usec() - t0) / 1000.0
	log.append("%d parts: editor stats %.1f ms (%d stages)" % [m.parts.size(), t_stats, (st.get("stages", []) as Array).size()])
	var f := FileAccess.open("user://perf.craft.json", FileAccess.WRITE)
	f.store_string(m.save_text())
	f.close()
	var s := FlightSession.new()
	s.launch(m.save_text(), "floe")
	s.set_throttle(1.0)
	var tp := Time.get_ticks_usec()
	s.step_ticks(50)
	log.append("on the pad, clamped: 50 ticks %.1f ms" % ((Time.get_ticks_usec() - tp) / 1000.0))
	s.stage()
	t0 = Time.get_ticks_usec()
	for i in 10:
		s.telemetry()
	log.append("telemetry %.2f ms" % ((Time.get_ticks_usec() - t0) / 10000.0))
	t0 = Time.get_ticks_usec()
	for i in 10:
		s.core.vessel_render(s.active, s.active)
	log.append("render feed %.2f ms" % ((Time.get_ticks_usec() - t0) / 10000.0))
	for sec in 6:
		t0 = Time.get_ticks_usec()
		s.step_ticks(50)
		var tt := s.telemetry()
		log.append("second %d: 50 ticks %.1f ms, %d vessels, alive %s, alt %.0f m, parts %d" % [sec + 1, (Time.get_ticks_usec() - t0) / 1000.0,
			(JSON.parse_string(s.core.vessel_list()) as Array).size(), tt.get("alive", false), tt.get("altitude", -1.0), int(tt.get("parts", 0))])
	var kinds := {}
	for e in s.events:
		var k := "%s/%s/%s" % [e.get("kind", "?"), e.get("cause", e.get("how", "")), e.get("part", "")]
		kinds[k] = kinds.get(k, 0) + 1
	log.append("events: %s" % [kinds])
	if not s.events.is_empty():
		log.append("first: %s" % [s.events.slice(0, 4)])
	return {"status": "PASS", "log": "\n".join(log)}
