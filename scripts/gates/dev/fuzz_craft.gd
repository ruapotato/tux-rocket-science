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
## Dev: builds random craft from every part the way a player could (free
## stack nodes, surface attach with symmetry, pick-up and put-back, undo),
## then checks the save round-trips, the hangar stats are finite, and a
## flight staged through every stage keeps finite, parseable telemetry.
## SEED / N pick the run.
extends RefCounted

var log := PackedStringArray()
var rng := RandomNumberGenerator.new()
var ids: Array = []
var fails := 0
var ops := PackedStringArray()
var first_bad := ""
var plume_tally := {}


func run(_runner: Node) -> Dictionary:
	var seed_env := OS.get_environment("SEED")
	rng.seed = int(seed_env) if seed_env != "" else 1
	var n := int(OS.get_environment("N")) if OS.get_environment("N") != "" else 60
	ids = Sim.catalog_order.duplicate()
	for k in n:
		_one(k)
	log.append("%d craft, %d failures" % [n, fails])
	if not plume_tally.is_empty():
		log.append("exhaust warning vs flight: %s" % [plume_tally])
	return {"status": "PASS" if fails == 0 else "FAIL", "log": "\n".join(log)}


func _fail(k: int, m: CraftModel, why: String) -> void:
	fails += 1
	log.append("craft %d FAIL: %s" % [k, why])
	_save(k, m)


func _save(k: int, m: CraftModel) -> void:
	var path := "user://fuzz_%d.craft.json" % k
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f:
		f.store_string(m.save_text())
	log.append("  saved %s" % ProjectSettings.globalize_path(path))


func _pick() -> String:
	return ids[rng.randi() % ids.size()]


## A rocket a sensible player might build: stacks hang down from free
## bottom nodes, boosters go on radial decouplers (fed or not), fins and
## legs on the flanks, engines always pointing down.
func _build_sane() -> CraftModel:
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single(["pod_1", "pod_3", "probe_core"][rng.randi() % 3]))
	if rng.randf() < 0.3:
		m.manual_staging = true
	var stack := ["tank_s", "tank_m", "tank_l", "tank_xm", "tank_xl", "decoupler_stack", "decoupler_xl", "adapter_21",
		"reaction_wheel", "heat_shield", "mount_tri", "engine_lifter", "engine_vac", "engine_small", "engine_big", "srb"]
	for step in rng.randi_range(2, 14):
		var down := []
		for fn in m.free_nodes():
			if fn.dir[1] < -0.5:
				down.append(fn)
		var r := rng.randf()
		if r < 0.7 and not down.is_empty():
			var fn: Dictionary = down[rng.randi() % down.size()]
			m.attach_stack(CraftModel.held_single(stack[rng.randi() % stack.size()]), fn.index, fn.node)
		elif r < 0.85:
			# a booster: radial decoupler, tank or SRB, engine under a tank
			var tanks := []
			for i in m.parts.size():
				if Sim.part(m.parts[i].part).has("fuel_liquid"):
					tanks.append(i)
			if tanks.is_empty():
				continue
			var t: int = tanks[rng.randi() % tanks.size()]
			var dr := m.attach_surface(CraftModel.held_single("decoupler_radial"), t, rng.randf() * TAU, 0.0, [2, 3, 4][rng.randi() % 3])
			if dr.is_empty():
				continue
			var body: String = ["tank_m", "tank_l", "srb"][rng.randi() % 3]
			var b := m.attach_stack(CraftModel.held_single(body), dr[0], "outer")
			if b.is_empty():
				continue
			if body != "srb":
				m.attach_stack(CraftModel.held_single(["engine_lifter", "engine_small"][rng.randi() % 2]), b[0], "bottom")
				if rng.randf() < 0.5:
					m.attach_surface(CraftModel.held_single("feed_line"), b[0], PI, 0.0, 1)
			if rng.randf() < 0.5:
				m.attach_stack(CraftModel.held_single("nosecone"), b[0], "top")
		elif m.parts.size() > 1:
			var t := rng.randi_range(1, m.parts.size() - 1)
			var h := CraftModel.part_height(m.parts[t].part)
			m.attach_surface(CraftModel.held_single(["fin", "landing_leg", "parachute"][rng.randi() % 3]), t, rng.randf() * TAU, -0.3 * h, [2, 3, 4][rng.randi() % 3])
	return m


func _build() -> CraftModel:
	var m := CraftModel.new()
	var roots := ["pod_1", "pod_3", "probe_core"]
	m.place_root(CraftModel.held_single(roots[rng.randi() % roots.size()]))
	if rng.randf() < 0.3:
		m.manual_staging = true
	var steps := rng.randi_range(3, 22)
	ops.clear()
	first_bad = ""
	for s in steps:
		if m.parts.is_empty():
			m.place_root(CraftModel.held_single(roots[rng.randi() % roots.size()]))
		var r := rng.randf()
		var held := CraftModel.held_single(_pick())
		if r < 0.6:
			var free := m.free_nodes()
			if free.is_empty():
				continue
			var fn: Dictionary = free[rng.randi() % free.size()]
			m.attach_stack(held, fn.index, fn.node, [0.0, PI / 2, PI][rng.randi() % 3])
			_note(m, "stack %s on %d.%s" % [held.parts[0].part, fn.index, fn.node])
		elif r < 0.85:
			var target := rng.randi() % m.parts.size()
			var h := CraftModel.part_height(m.parts[target].part)
			m.attach_surface(held, target, rng.randf() * TAU, rng.randf_range(-0.5, 0.5) * h, [1, 2, 3, 4][rng.randi() % 4])
			_note(m, "surface %s on %d" % [held.parts[0].part, target])
		elif r < 0.92 and m.parts.size() > 2:
			# pick a subtree up and drop it on a free node elsewhere
			var i := rng.randi_range(1, m.parts.size() - 1)
			var got := m.pick_up(i)
			var free := m.free_nodes()
			if not got.is_empty() and not free.is_empty():
				var fn: Dictionary = free[rng.randi() % free.size()]
				m.attach_stack(got, fn.index, fn.node)
			_note(m, "move %d (%d parts)" % [i, got.get("parts", []).size()])
		elif r < 0.96 and m.can_undo():
			m.undo()
			_note(m, "undo")
		elif m.parts.size() > 2:
			var ri := rng.randi_range(1, m.parts.size() - 1)
			m.remove(ri)
			_note(m, "remove %d" % ri)
	return m


func _note(m: CraftModel, op: String) -> void:
	ops.append(op)
	if first_bad == "":
		var why := _staging_problem(m)
		if why != "":
			first_bad = "after op %d '%s' (manual %s): %s" % [ops.size(), op, m.manual_staging, why]


func _staging_problem(m: CraftModel) -> String:
	var used := {}
	for i in m.parts.size():
		var sg: int = m.parts[i].stage
		if m.activatable(i) and sg < 0 and not (Sim.part(m.parts[i].part).has("decoupler") and m.detach_set(i).is_empty()):
			return "activatable %d %s has no stage" % [i, m.parts[i].part]
		if sg >= 0:
			used[sg] = true
	for sg in used.size():
		if not used.has(sg):
			return "stage gap: %s" % [used.keys()]
	return ""


func _finite(v) -> bool:
	if v is float:
		return not (is_nan(v) or is_inf(v))
	if v is Dictionary:
		for x in v.values():
			if not _finite(x):
				return false
	if v is Array:
		for x in v:
			if not _finite(x):
				return false
	return true


func _one(k: int) -> void:
	var m := _build_sane() if OS.get_environment("SANE") != "" else _build()
	if m.parts.is_empty():
		return  # undone back to an empty hangar
	var text := m.save_text()
	if text == "":
		log.append("craft %d FAIL: save refused (%d parts): %s" % [k, m.parts.size(), Sim.core.last_error()])
		fails += 1
		return
	var m2 := CraftModel.new()
	if not m2.load_text(text):
		_fail(k, m, "saved craft does not load: %s" % [Sim.decode(text).get("error")])
		return
	if m2.save_text() != text:
		_fail(k, m, "save -> load -> save differs")
		return
	var st: Dictionary = m.stats()
	if st.is_empty() or not _finite(st):
		_fail(k, m, "hangar stats empty or not finite: %s" % [st])
		return
	# stage numbers: every activatable part has one, stages are contiguous
	var used := {}
	for i in m.parts.size():
		var sg: int = m.parts[i].stage
		if m.activatable(i) and sg < 0 and not (Sim.part(m.parts[i].part).has("decoupler") and m.detach_set(i).is_empty()):
			_fail(k, m, "activatable %s has no stage; first %s" % [m.parts[i].part, first_bad])
			return
		if sg >= 0:
			used[sg] = true
	for sg in used.size():
		if not used.has(sg):
			_fail(k, m, "stage gap: %s" % [used.keys()])
			return
	if OS.get_environment("DV") != "":
		_dv_check(k, m, text, st)
		return
	var s := FlightSession.new()
	if not s.launch(text):
		_fail(k, m, "launch refused")
		return
	s.set_throttle(1.0)
	var stages := m.stage_count()
	var line := "craft %d: %d parts, %d stages, %.1f t, dv %.0f" % [k, m.parts.size(), stages, st.get("mass", 0.0) / 1000.0, st.get("dv_vac", 0.0)]
	for p in stages + 1:
		s.stage()
		for q in 6:
			s.step_ticks(50)
			var t = JSON.parse_string(s.core.telemetry(s.active)) if s.active >= 0 else {"alive": false}
			if t == null:
				_fail(k, m, "telemetry not JSON after %.1f s" % s.time())
				return
			if not _finite(t):
				_fail(k, m, "telemetry not finite after %.1f s" % s.time())
				return
			if not t.get("alive", false):
				break
	var tf := s.telemetry()
	var vl = JSON.parse_string(s.core.vessel_list())
	if vl == null:
		_fail(k, m, "vessel list not JSON")
		return
	var gone := {}
	for e in s.events:
		if e.kind == "part_destroyed":
			var sp := float(e.speed)
			var key := "%s %s" % [e.get("part", "?"), "heat" if sp <= -2.0 else "blast" if sp < 0.0 else "impact"]
			if gone.is_empty():
				log.append("  craft %d first loss: %s at T+%.2f, %.1f m/s" % [k, e.part, e.t, sp])
			gone[key] = gone.get(key, 0) + 1
	if not tf.get("alive", false) and s.time() < 4.0:
		_save(k, m)
	log.append(line + " -> T+%.0f alive %s alt %.0f, %d vessels, destroyed %s" % [s.time(), tf.get("alive", false), tf.get("altitude", 0.0), (vl as Array).size(), gone])


## In orbit (vacuum), staged the way the hangar plans it (each stage burns
## for its listed burn time): the speed the engines actually gave must be
## the hangar's vacuum delta-v.
func _dv_check(k: int, m: CraftModel, text: String, st: Dictionary) -> void:
	var s := FlightSession.new()
	if not s.spawn_in_orbit(text, "floe", 400000.0):
		_fail(k, m, "spawn refused")
		return
	s.set_sas("off")
	s.set_throttle(1.0)
	var plan: Array = st.get("stages", [])
	var t_burn := s.time()
	for x in plan:
		t_burn += float(x.get("burn_time", 0.0)) + 0.1
		s.stage()
		var ticks := int(ceil(float(x.get("burn_time", 0.0)) * 50.0)) + 2
		while ticks > 0:
			var n := mini(ticks, 500)
			s.step_ticks(n)
			ticks -= n
	s.step_ticks(50)
	var t := s.telemetry()
	var want: float = st.get("dv_vac", 0.0)
	var got: float = t.get("dv_expended", 0.0)
	var lost := 0
	for e in s.events:
		if e.kind == "part_destroyed":
			lost += 1
			if lost <= 3 and OS.get_environment("VERBOSE") != "":
				log.append("    lost %s at T+%.2f speed %.1f" % [e.part, e.t, e.speed])
	var heat_lost := 0
	for e in s.events:
		if e.kind == "part_destroyed" and float(e.speed) <= -2.0 and float(e.t) < t_burn + 2.0:
			heat_lost += 1  # burnt while engines ran (not on a later re-entry)
	var warned := (st.get("plume_hits", []) as Array).size()
	if heat_lost > 0 and warned == 0:
		_fail(k, m, "%d parts burnt up with no exhaust warning in the hangar" % heat_lost)
	plume_tally[("warned " if warned > 0 else "clear ") + ("burnt" if heat_lost > 0 else "fine")] = plume_tally.get(("warned " if warned > 0 else "clear ") + ("burnt" if heat_lost > 0 else "fine"), 0) + 1
	if warned > 0 and heat_lost == 0 and OS.get_environment("VERBOSE") != "":
		var h: Array = st.plume_hits[0]
		log.append("  craft %d warned, none burnt: %s into %s from stage %d, soak %.0f K; %d lost, stages %s" % [k, m.parts[int(h[1])].part, m.parts[int(h[0])].part, int(h[2]), float(h[3]), lost,
			(st.stages as Array).map(func(x): return "%d:%.0fs" % [int(x.stage), float(x.burn_time)])])
	var err := absf(got - want) / maxf(want, 1.0)
	if lost > 0 and OS.get_environment("VERBOSE") != "":
		_save(k, m)
	var line := "craft %d: %d parts, %d stages, hangar %.1f m/s, flown %.1f m/s (%.3f%%), %d parts lost" % [k, m.parts.size(), plan.size(), want, got, err * 100.0, lost]
	log.append(line)
	if err > 0.005 and lost == 0 and want > 1.0:
		_fail(k, m, "delta-v off: " + line)
