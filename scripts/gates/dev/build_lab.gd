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
## Dev: builds craft the way a player would, from every kind of part, flies
## each and reports what happened.
extends RefCounted

var log := PackedStringArray()
var s: FlightSession


func run(_runner: Node) -> Dictionary:
	s = FlightSession.new()
	var only := OS.get_environment("LAB")
	if only != "":
		call(only)
		return {"status": "PASS", "log": "\n".join(log)}
	_liquid_boosters(false)
	_liquid_boosters(true)
	_srb_boosters()
	_radial_engines()
	_engine_on_engine()
	_side_chute()
	_nested_boosters()
	_two_stage()
	_legs()
	_reattach_booster()
	return {"status": "PASS", "log": "\n".join(log)}


func _core(m: CraftModel, tank := "tank_l") -> int:
	m.place_root(CraftModel.held_single("pod_1"))
	m.attach_stack(CraftModel.held_single("parachute"), 0, "top")
	var t := m.attach_stack(CraftModel.held_single(tank), 0, "bottom")[0]
	m.attach_stack(CraftModel.held_single("engine_lifter"), t, "bottom")
	return t


func _stages(m: CraftModel) -> void:
	for st in range(m.stage_count() - 1, -1, -1):
		var names := []
		for i in m.parts.size():
			if m.parts[i].stage == st:
				names.append(m.parts[i].part)
		log.append("    stage %d: %s" % [st, ", ".join(names)])


func _launch(m: CraftModel) -> Dictionary:
	var st: Dictionary = m.stats()
	var twr := 0.0
	for x in st.get("stages", []):
		if x.thrust_sl > 0:
			twr = x.twr_sl
			break
	log.append("  hangar: %d parts, %.2f t, TWR %.2f, dv %.0f m/s, %s" % [m.parts.size(), st.get("mass", 0.0) / 1000.0,
		twr, st.get("dv_vac", 0.0), _warn(st)])
	_stages(m)
	s.launch(m.save_text())
	s.set_throttle(1.0)
	s.stage()
	s.step_ticks(5)
	return s.telemetry()


func _warn(st: Dictionary) -> String:
	return "unfed %s" % [st.get("unfed_engines", [])]


func _vessels() -> Array:
	return JSON.parse_string(s.core.vessel_list())


func _destroyed() -> Array:
	var out := []
	for e in s.events:
		if e.kind == "part_destroyed":
			out.append("%s@%.1f %s" % [e.get("part", "?"), e.get("t", 0.0), e.get("cause", e.get("speed", ""))])
	return out


func _run_until(cond: Callable, max_s: float) -> float:
	var t0 := s.time()
	while s.time() - t0 < max_s and not cond.call(s.telemetry()):
		s.step_ticks(5)
	return s.time() - t0


func _report(tag: String) -> void:
	var t := s.telemetry()
	var vs := _vessels()
	var far := []
	for v in vs:
		if int(v.id) != s.active:
			far.append("%s %dp %.0fm" % [v.situation, v.parts, s._distance_to_active(int(v.id))])
	log.append("  %s: T+%.1f pitch %.0f hdg %.0f, alt %.0f m, %.0f m/s up, %d engines lit, thrust %.0f kN, %d parts, others [%s], destroyed %s" % [
		tag, s.time(), t.pitch_deg, t.heading_deg, t.altitude, t.vertical_speed, t.engines_lit, t.thrust / 1000.0, t.parts.size() if t.parts is Array else int(t.get("parts", 0)),
		", ".join(far), _destroyed()])


func _liquid_boosters(manual: bool) -> void:
	log.append("liquid side boosters on radial decouplers%s" % (" (manual staging)" if manual else ""))
	var m := CraftModel.new()
	var t := _core(m)
	m.attach_surface(CraftModel.held_single("fin"), t, 0.0, -1.4, 3)
	if manual:
		m.manual_staging = true
	var dr := m.attach_surface(CraftModel.held_single("decoupler_radial"), t, PI / 2, 0.0, 2)
	var side := m.attach_stack(CraftModel.held_single("tank_m"), dr[0], "outer")
	m.attach_stack(CraftModel.held_single("engine_lifter"), side[0], "bottom")
	m.attach_stack(CraftModel.held_single("nosecone"), side[0], "top")
	var t0 := _launch(m)
	log.append("  liftoff: %d lit, thrust %.0f kN" % [t0.engines_lit, t0.thrust / 1000.0])
	for k in 6:
		s.step_ticks(250)
		_report("climb")
	var dt := _run_until(func(x): return int(x.engines_lit) < 3, 120.0)
	_report("after %.1f s side tanks dry" % dt)
	s.stage()
	s.step_ticks(150)
	_report("3 s after SPACE")


func _srb_boosters() -> void:
	log.append("solid boosters on radial decouplers")
	var m := CraftModel.new()
	var t := _core(m)
	var dr := m.attach_surface(CraftModel.held_single("decoupler_radial"), t, PI / 2, 0.0, 3)
	var b := m.attach_stack(CraftModel.held_single("srb"), dr[0], "outer")
	m.attach_stack(CraftModel.held_single("nosecone"), b[0], "top")
	var t0 := _launch(m)
	log.append("  liftoff: %d lit, thrust %.0f kN" % [t0.engines_lit, t0.thrust / 1000.0])
	for k in 6:
		s.step_ticks(250)
		_report("climb")
	var dt := _run_until(func(x): return int(x.engines_lit) < 4, 120.0)
	_report("after %.1f s SRBs out" % dt)
	s.stage()
	s.step_ticks(150)
	_report("3 s after SPACE")


func _radial_engines() -> void:
	log.append("engines surface-attached straight onto the tank's flank")
	var m := CraftModel.new()
	var t := _core(m, "tank_m")
	m.attach_surface(CraftModel.held_single("engine_small"), t, 0.0, -0.6, 4)
	_launch(m)
	s.step_ticks(250)
	_report("5 s")


func _engine_on_engine() -> void:
	log.append("an engine stacked under an engine")
	var m := CraftModel.new()
	var t := _core(m)
	var e := m.children_of(t)[0]
	m.attach_stack(CraftModel.held_single("engine_small"), e, "bottom")
	_launch(m)
	s.step_ticks(250)
	_report("5 s")


func _side_chute() -> void:
	log.append("two chutes on the pod's sides, none on top: pop up and come down")
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	var t := m.attach_stack(CraftModel.held_single("tank_s"), 0, "bottom")[0]
	m.attach_stack(CraftModel.held_single("engine_lifter"), t, "bottom")
	m.attach_surface(CraftModel.held_single("parachute"), 0, 0.0, 0.0, 2)
	_launch(m)
	_run_until(func(x): return int(x.engines_lit) == 0, 60.0)
	_run_until(func(x): return x.vertical_speed < 0.0, 120.0)
	_report("apex")
	s.deploy_chutes()
	_run_until(func(x): return x.landed or not x.alive, 600.0)
	var tt := s.telemetry()
	_report("down (chutes open %d, impact %.1f m/s, crew %d)" % [tt.chutes_open, tt.get("max_impact", -1.0), tt.get("crew", 0)])


func _nested_boosters() -> void:
	log.append("boosters on boosters (radial decouplers on a radially decoupled tank)")
	var m := CraftModel.new()
	var t := _core(m)
	var dr := m.attach_surface(CraftModel.held_single("decoupler_radial"), t, PI / 2, 0.0, 2)
	var side := m.attach_stack(CraftModel.held_single("tank_l"), dr[0], "outer")
	m.attach_stack(CraftModel.held_single("engine_lifter"), side[0], "bottom")
	var dr2 := m.attach_surface(CraftModel.held_single("decoupler_radial"), side[0], PI / 2, 0.0, 1)
	log.append("  outer decouplers placed: %d" % dr2.size())
	if dr2.size() > 0:
		m.attach_stack(CraftModel.held_single("srb"), dr2[0], "outer")
	_launch(m)
	s.step_ticks(250)
	_report("5 s")


func _two_stage() -> void:
	log.append("two-stage stack: Huddle-3, wheel, upper tank+vacuum engine, decoupler, lower stage")
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_3"))
	m.attach_stack(CraftModel.held_single("parachute"), 0, "top")
	var hs := m.attach_stack(CraftModel.held_single("heat_shield"), 0, "bottom")[0]
	var rw := m.attach_stack(CraftModel.held_single("reaction_wheel"), hs, "bottom")[0]
	var ut := m.attach_stack(CraftModel.held_single("tank_m"), rw, "bottom")[0]
	var ue := m.attach_stack(CraftModel.held_single("engine_vac"), ut, "bottom")[0]
	var d := m.attach_stack(CraftModel.held_single("decoupler_stack"), ue, "bottom")[0]
	var lt := m.attach_stack(CraftModel.held_single("tank_l"), d, "bottom")[0]
	var lt2 := m.attach_stack(CraftModel.held_single("tank_l"), lt, "bottom")[0]
	m.attach_stack(CraftModel.held_single("engine_lifter"), lt2, "bottom")
	m.attach_surface(CraftModel.held_single("fin"), lt2, 0.0, -1.4, 4)
	var t0 := _launch(m)
	log.append("  liftoff: %d lit, TWR %.2f" % [t0.engines_lit, t0.twr])
	_run_until(func(x): return int(x.engines_lit) == 0, 300.0)
	_report("lower stage dry")
	s.stage()
	s.step_ticks(50)
	_report("1 s after the decoupler")
	s.stage()
	s.step_ticks(250)
	_report("5 s after lighting the upper stage")


func _legs() -> void:
	log.append("lander on four legs, set down on flat ice")
	for deployed in [true, false]:
		var m := CraftModel.new()
		m.place_root(CraftModel.held_single("pod_1"))
		var t := m.attach_stack(CraftModel.held_single("tank_m"), 0, "bottom")[0]
		m.attach_stack(CraftModel.held_single("engine_small"), t, "bottom")
		m.attach_surface(CraftModel.held_single("landing_leg"), t, PI / 4, -0.5, 4)
		s.launch(m.save_text())
		s.active = s.core.flight_place_landed(m.save_text(), "floe", 0.0, 20.0)
		s.set_legs(deployed)
		s.step_ticks(500)
		var tt := s.telemetry()
		log.append("  legs %s: after 10 s pitch %.1f, landed %s, speed %.2f m/s, worst contact %.1f, destroyed %s" % [
			"down" if deployed else "up", tt.pitch_deg, tt.landed, tt.surface_speed, tt.get("max_impact", 0.0), _destroyed()])


func _reattach_booster() -> void:
	log.append("picking a side booster up and putting it back on its decoupler")
	var m := CraftModel.new()
	var t := _core(m)
	var dr := m.attach_surface(CraftModel.held_single("decoupler_radial"), t, PI / 2, 0.0, 3)
	var side := m.attach_stack(CraftModel.held_single("tank_m"), dr[0], "outer")
	m.attach_stack(CraftModel.held_single("engine_lifter"), side[0], "bottom")
	var before := m.save_text()
	var held := m.pick_up(side[0])
	log.append("  picked up %d parts, %d left" % [held.parts.size(), m.parts.size()])
	var back := m.attach_stack(held, dr[0], "outer")
	var same := m.save_text() == before
	log.append("  put back on %d decouplers, craft identical: %s" % [back.size(), same])
	if not same:
		var a: Array = JSON.parse_string(before).parts
		var b: Array = JSON.parse_string(m.save_text()).parts
		for i in mini(a.size(), b.size()):
			if JSON.stringify(a[i]) != JSON.stringify(b[i]):
				log.append("    %d: %s\n     -> %s" % [i, JSON.stringify(a[i]), JSON.stringify(b[i])])
				break
	# spun a quarter turn about its own axis
	m.pick_up(back[0])
	var spun := m.stack_placements(held, dr[0], "outer", PI / 2)
	log.append("  spun 90 deg: up %s" % [DMath.q_rotate(spun[0].rot, [0.0, 1.0, 0.0])])


func _probe() -> void:
	log.append("probe core, no crew: holds attitude and steers")
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("probe_core"))
	var t := m.attach_stack(CraftModel.held_single("tank_m"), 0, "bottom")[0]
	m.attach_stack(CraftModel.held_single("engine_lifter"), t, "bottom")
	m.attach_surface(CraftModel.held_single("fin"), t, 0.0, -0.6, 3)
	var t0 := _launch(m)
	log.append("  crew %d / seats %d" % [t0.get("crew", 0), t0.get("crew_capacity", 0)])
	s.step_ticks(250)
	_report("5 s")
	s.set_input(0.5, 0.0, 0.0)
	s.step_ticks(100)
	s.set_input(0.0, 0.0, 0.0)
	s.step_ticks(100)
	_report("after 2 s of pitch input")


func _tri_mount() -> void:
	log.append("wide tank on a tri-mount with three Walrus engines")
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_3"))
	m.attach_stack(CraftModel.held_single("parachute"), 0, "top")
	var ad := m.attach_stack(CraftModel.held_single("adapter_21"), 0, "bottom")
	log.append("  adapter under the pod: %s (up %s)" % [ad, DMath.q_rotate(m.parts[ad[0]].rot, [0.0, 1.0, 0.0]) if ad.size() > 0 else "-"])
	var t := m.attach_stack(CraftModel.held_single("tank_xl"), ad[0], "bottom")[0]
	var mt := m.attach_stack(CraftModel.held_single("mount_tri"), t, "bottom")[0]
	for nn in ["bottom", "bottom_2", "bottom_3"]:
		var e := m.attach_stack(CraftModel.held_single("engine_lifter"), mt, nn)
		log.append("  engine on %s: %s" % [nn, e])
	m.attach_surface(CraftModel.held_single("fin"), t, PI / 3, -1.4, 3)
	var t0 := _launch(m)
	log.append("  liftoff: %d lit, thrust %.0f kN, TWR %.2f" % [t0.engines_lit, t0.thrust / 1000.0, t0.twr])
	s.step_ticks(500)
	_report("10 s")


func _heavy() -> void:
	log.append("heavy lifter: wide first stage under an adapter, flown to orbit by the ascent autopilot")
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	m.attach_stack(CraftModel.held_single("parachute"), 0, "top")
	var hs := m.attach_stack(CraftModel.held_single("heat_shield"), 0, "bottom")[0]
	var d0 := m.attach_stack(CraftModel.held_single("decoupler_stack"), hs, "bottom")[0]
	var ut := m.attach_stack(CraftModel.held_single("tank_l"), d0, "bottom")[0]
	var ue := m.attach_stack(CraftModel.held_single("engine_vac"), ut, "bottom")[0]
	var d1 := m.attach_stack(CraftModel.held_single("decoupler_stack"), ue, "bottom")[0]
	var ad := m.attach_stack(CraftModel.held_single("adapter_21"), d1, "bottom")
	log.append("  adapter flipped under a 1.25 m decoupler: %s (up %s)" % [ad, DMath.q_rotate(m.parts[ad[0]].rot, [0.0, 1.0, 0.0]) if ad.size() > 0 else "-"])
	var t := m.attach_stack(CraftModel.held_single("tank_xl"), ad[0], "bottom")[0]
	m.attach_stack(CraftModel.held_single("engine_big"), t, "bottom")
	m.attach_surface(CraftModel.held_single("fin"), t, PI / 3, -1.4, 4)
	m.craft_name = "Heavy"
	var st: Dictionary = m.stats()
	log.append("  hangar: %.1f t, dv %.0f m/s, %d stages" % [st.mass / 1000.0, st.dv_vac, m.stage_count()])
	_stages(m)
	var api := ControlAPI.new(s)
	s.launch(m.save_text())
	var ap := AscentAutopilot.new(api)
	var guard := 0
	while guard < 50 * 60 * 30:
		if ap.step(0.1):
			break
		var tel: Dictionary = s.telemetry()
		if ap.phase == "coast" and not tel.in_atmosphere and tel.orbit.t_to_ap != null and tel.orbit.t_to_ap > 60.0:
			api.handle({"cmd": "warp_to", "t": tel.t + tel.orbit.t_to_ap - 50.0})
			continue
		s.step_ticks(5)
		guard += 5
	for line in ap.log:
		log.append("    " + line)
	var tf := s.telemetry()
	log.append("  end: %s, Pe %.1f km, %.0f m/s left, peak heat %.0f%% (%s), destroyed %s" % [tf.situation, tf.orbit.pe_alt / 1000.0, tf.dv_total, ap.max_heat * 100.0, ap.hot_part, _destroyed()])


func _canted() -> void:
	for tilt in [0.0, 30.0, -30.0]:
		log.append("two side engines on a long tank's flank, tilted %.0f deg: hangar vs flight" % tilt)
		var m := CraftModel.new()
		m.place_root(CraftModel.held_single("probe_core"))
		var t := m.attach_stack(CraftModel.held_single("tank_l"), 0, "bottom")[0]
		var e := m.attach_surface(CraftModel.held_single("engine_small"), t, 0.0, -0.8, 2)
		if tilt != 0.0:
			m.rotate_part(e[0], [0.0, 0.0, 1.0], deg_to_rad(tilt))
		for i in e:
			log.append("  engine %d at %s up %s" % [i, m.parts[i].pos, DMath.q_rotate(m.parts[i].rot, [0.0, 1.0, 0.0])])
		var st: Dictionary = m.stats()
		s.spawn_in_orbit(m.save_text(), "floe", 400000.0)
		s.set_throttle(1.0)
		s.stage()
		s.step_ticks(5)
		var t0 := s.telemetry()
		log.append("  hangar: thrust vac %.1f kN, dv %.1f m/s, exhaust hits %s; flight: thrust %.1f kN" % [st.stages[0].thrust_vac / 1000.0, st.dv_vac, st.plume_hits, t0.thrust / 1000.0])
		s.step_ticks(int(st.stages[0].burn_time * 50.0) + 50)
		var tf := s.telemetry()
		log.append("  flown %.1f m/s, alive %s, destroyed %s" % [tf.get("dv_expended", -1.0), tf.get("alive", false), _destroyed()])
