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
## --test-craft-roundtrip: every craft file loads and saves byte-identically
## through the hangar's CraftModel, and the reloaded craft gives identical
## trs_core readouts. Also rebuilds the stock craft with the editor API
## (stack/surface attach, symmetry, auto-staging) and checks the result
## matches the shipped files.
extends RefCounted

var _log := PackedStringArray()
var _failed := 0


func _check(ok: bool, msg: String) -> void:
	_log.append(("ok   " if ok else "FAIL ") + msg)
	if not ok:
		_failed += 1


func run(_runner: Node) -> Dictionary:
	var files := Sim.craft_files()
	_check(files.size() >= 3, "%d craft files found" % files.size())
	for path in files:
		var text0 := FileAccess.get_file_as_string(path)
		var m := CraftModel.new()
		if not m.load_text(text0):
			_check(false, "%s: load failed" % path)
			continue
		var text1 := m.save_text()
		var m2 := CraftModel.new()
		m2.load_text(text1)
		var text2 := m2.save_text()
		var s0 := Sim.stats_text(text0)
		var s2 := m2.stats_raw()
		var stock := path.begins_with(Sim.STOCK_CRAFT_DIR)
		_check(text1 == text2 and (text1 == text0 or not stock) and s0 == s2,
			"%s: %d bytes, save==load->save %s, file canonical %s, readouts identical %s" % [
				path.get_file(), text1.length(), text1 == text2, text1 == text0, s0 == s2])
		# stage readouts for the report
		var st := m2.stats()
		var line := "     %s: %.0f kg, dv vac %.0f / SL %.0f m/s," % [st.name, st.mass, st.dv_vac, st.dv_sl]
		for s in st.stages:
			if s.dv_vac > 0:
				line += " S%d %.0f" % [s.stage, s.dv_vac]
		_log.append(line)

	# Rebuild stock craft through the editor API.
	_compare_rebuild("hopper", _build_hopper())
	_compare_rebuild("orbiter", _build_orbiter())
	_compare_rebuild("krill_lander", _build_lander())
	_editor_ops()
	_insert_ops()
	_staging_ops()
	return {"status": "FAIL" if _failed else "PASS", "log": "\n".join(_log)}


func _stack_below(m: CraftModel, target: int, part_id: String) -> int:
	return m.attach_stack(CraftModel.held_single(part_id), target, "bottom")[0]


func _build_hopper() -> CraftModel:
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	m.attach_stack(CraftModel.held_single("parachute"), 0, "top")
	var dec := _stack_below(m, 0, "decoupler_stack")
	var tank := _stack_below(m, dec, "tank_m")
	_stack_below(m, tank, "engine_lifter")
	m.attach_surface(CraftModel.held_single("fin"), tank, 0.0, -0.6, 3)
	return m


func _build_orbiter() -> CraftModel:
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	m.attach_stack(CraftModel.held_single("parachute"), 0, "top")
	var rw := _stack_below(m, 0, "reaction_wheel")
	var hs := _stack_below(m, rw, "heat_shield")
	var d1 := _stack_below(m, hs, "decoupler_stack")
	var t2 := _stack_below(m, d1, "tank_s")
	var e2 := _stack_below(m, t2, "engine_vac")
	var d2 := _stack_below(m, e2, "decoupler_stack")
	var t1a := _stack_below(m, d2, "tank_l")
	var t1b := _stack_below(m, t1a, "tank_m")
	_stack_below(m, t1b, "engine_lifter")
	m.attach_surface(CraftModel.held_single("fin"), t1b, 0.0, -0.6, 3)
	return m


func _build_lander() -> CraftModel:
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_1"))
	m.attach_stack(CraftModel.held_single("parachute"), 0, "top")
	var rw := _stack_below(m, 0, "reaction_wheel")
	var hs := _stack_below(m, rw, "heat_shield")
	var d0 := _stack_below(m, hs, "decoupler_stack")
	var ls := _stack_below(m, d0, "tank_s")
	var lt := _stack_below(m, ls, "tank_m")
	m.attach_surface(CraftModel.held_single("landing_leg"), lt, deg_to_rad(45), -0.5, 4)
	var le := _stack_below(m, lt, "engine_small")
	var d1 := _stack_below(m, le, "decoupler_stack")
	var tt := _stack_below(m, d1, "tank_l")
	var te := _stack_below(m, tt, "engine_vac")
	var d2 := _stack_below(m, te, "decoupler_stack")
	var ca := _stack_below(m, d2, "tank_l")
	var cb := _stack_below(m, ca, "tank_l")
	_stack_below(m, cb, "engine_lifter")
	m.attach_surface(CraftModel.held_single("fin"), cb, 0.0, -1.4, 3)
	var decs := m.attach_surface(CraftModel.held_single("decoupler_radial"), cb, deg_to_rad(90), 0.0, 2)
	# booster on the first decoupler; symmetry puts one on its counterpart
	var srb := CraftModel.held_single("srb")
	srb.parts.append({"part": "nosecone", "parent": 0, "node": "bottom", "parent_node": "top",
		"pos": [0.0, 2.5 + 0.55, 0.0], "rot": DMath.q_identity(), "stage": -1, "sym": 0})
	_attach_booster(m, decs[0], srb)
	return m


## Boosters hang off the radial decoupler's outer face (surface attach with
## the booster's side touching it).
func _attach_booster(m: CraftModel, dec: int, held: Dictionary) -> void:
	m.checkpoint()
	var sym := m._next_sym()
	var nose_sym := sym + 1
	for c in m.counterparts(dec):
		var d: Dictionary = m.parts[c]
		var outer := m.node_world(c, "outer")
		var off: float = Sim.part("srb").shape.radius
		var pos := DMath.add(outer.pos, DMath.scale(outer.dir, off))
		var root := m._append_held(held, c, "", "outer", pos, d.rot, sym)
		m.parts[root + 1].sym = nose_sym
	m._after_structure_change()


func _compare_rebuild(name: String, m: CraftModel) -> void:
	var stock := CraftModel.new()
	stock.load_file(Sim.STOCK_CRAFT_DIR + name + ".craft.json")
	var a := m.stats()
	var b := stock.stats()
	var same_parts := m.parts.size() == stock.parts.size()
	var max_pos := 0.0
	var same_stages := true
	var same_tree := true
	if same_parts:
		for k in m.parts.size():
			var p: Dictionary = m.parts[k]
			var q: Dictionary = stock.parts[k]
			max_pos = maxf(max_pos, DMath.length(DMath.sub(p.pos, q.pos)))
			same_stages = same_stages and p.stage == q.stage
			same_tree = same_tree and p.part == q.part and p.parent == q.parent
	var dv_err: float = absf(a.dv_vac - b.dv_vac) / b.dv_vac
	_check(same_parts and same_tree and same_stages and max_pos < 1e-9 and dv_err < 1e-12,
		"editor rebuild of %s: %d parts, tree %s, auto-staging %s, max pos diff %s m, dv diff %s" % [
			name, m.parts.size(), same_tree, same_stages, String.num_scientific(max_pos), String.num_scientific(dv_err)])


func _editor_ops() -> void:
	var m := _build_orbiter()
	var before := m.save_text()
	var n0 := m.parts.size()
	# symmetry: 6 fins, rotate + offset them all, then undo everything
	var fins := m.attach_surface(CraftModel.held_single("fin"), 7, 0.5, 1.0, 6)
	m.rotate_part(fins[0], [0.0, 0.0, 1.0], deg_to_rad(15))
	m.offset_part(fins[0], [0.0, 0.3, 0.0])
	var ok_sym := m.parts.size() == n0 + 6 and m.counterparts(fins[0]).size() == 6
	var spread := 0.0
	for c in m.counterparts(fins[0]):
		spread = maxf(spread, absf(m.parts[c].pos[1] - m.parts[fins[0]].pos[1]))
	m.undo()
	m.undo()
	m.undo()
	var after_undo := m.save_text()
	m.redo()
	var redo_ok := m.parts.size() == n0 + 6
	m.undo()
	# pick up the upper stage (decoupler subtree) and put it back
	var held := m.pick_up(3)
	var removed_ok := m.parts.size() < n0
	m.attach_stack(held, 2, "bottom")
	var ref := CraftModel.new()
	ref.load_text(before)
	var replaced := _same_craft(m, ref)
	# deleting a symmetric part removes all its counterparts
	var fin_idx := m.parts.size() - 1
	m.remove(fin_idx)
	var removed_sym := m.parts.size() == n0 - 3
	# what hangs on a radial decoupler rides beside the core, upright, and a
	# decoupler added under hand-made staging fires after the engines it carries
	for manual in [false, true]:
		var mb := CraftModel.new()
		mb.place_root(CraftModel.held_single("pod_1"))
		var ct := mb.attach_stack(CraftModel.held_single("tank_l"), 0, "bottom")[0]
		var ce := mb.attach_stack(CraftModel.held_single("engine_lifter"), ct, "bottom")[0]
		mb.manual_staging = manual
		var rd := mb.attach_surface(CraftModel.held_single("decoupler_radial"), ct, PI / 2, 0.0, 2)
		var bt := mb.attach_stack(CraftModel.held_single("tank_m"), rd[0], "outer")
		var be := mb.attach_stack(CraftModel.held_single("engine_lifter"), bt[0], "bottom")
		var upright := true
		for k in bt + be:
			var up := DMath.q_rotate(mb.parts[k].rot, [0.0, 1.0, 0.0])
			var off: Array = DMath.sub(mb.parts[k].pos, mb.parts[ct].pos)
			upright = upright and up[1] > 0.9999 and absf(sqrt(off[0] * off[0] + off[2] * off[2]) - 1.55) < 1e-9
		var order: bool = bt.size() == 2 and mb.parts[be[0]].stage == mb.parts[ce].stage and mb.parts[be[1]].stage == mb.parts[ce].stage \
			and mb.parts[rd[0]].stage == mb.parts[rd[1]].stage and mb.parts[rd[0]].stage == mb.parts[ce].stage - 1
		_check(upright and order, "%s staging: boosters on radial decouplers stand upright 1.55 m off the axis (%s), light with the core and drop on the next press (%s)" % [
			"hand-made" if manual else "automatic", upright, order])
	# a liquid engine on a radial decoupler with no tank of its own gets no fuel
	var mu := _build_orbiter()
	var dr := mu.attach_surface(CraftModel.held_single("decoupler_radial"), 8, 0.0, 0.0, 1)
	var eg := mu.attach_stack(CraftModel.held_single("engine_small"), dr[0], "outer")
	var unfed: Array = mu.stats().get("unfed_engines", [])
	_check(unfed.size() == 1 and int(unfed[0]) == eg[0] and (_build_orbiter().stats().get("unfed_engines", []) as Array).is_empty(),
		"engine on a bare radial decoupler flagged as unfed (%s), stock Orbiter has none" % [unfed])
	# a feed line from the core tank to it feeds it; one facing open space reaches nothing
	var fl := mu.attach_surface(CraftModel.held_single("feed_line"), 8, 0.0, 0.1, 1)
	var loose := mu.attach_surface(CraftModel.held_single("feed_line"), 8, PI, 0.1, 1)
	var st_fl := mu.stats()
	var lines: Array = st_fl.get("feed_lines", [])
	var targets := {}
	for l in lines:
		targets[int(l[0])] = int(l[1])
	var piped: bool = (st_fl.get("unfed_engines", []) as Array).is_empty() and targets.get(fl[0], -2) == eg[0] and targets.get(loose[0], -2) == -1
	_check(piped, "feed line from the core tank feeds that engine (lines %s, unfed %s), one facing away reaches nothing" % [lines, st_fl.get("unfed_engines", [])])
	# exhaust: no stock craft fires an engine into itself; a tank hung under
	# a solid booster is in its jet from the first SPACE and burns up
	var hot := []
	for path in Sim.craft_files():
		if path.begins_with(Sim.STOCK_CRAFT_DIR):
			var ms := CraftModel.new()
			ms.load_file(path)
			if not (ms.stats().get("plume_hits", []) as Array).is_empty():
				hot.append(path.get_file())
	var mj := CraftModel.new()
	mj.place_root(CraftModel.held_single("probe_core"))
	var sb := mj.attach_stack(CraftModel.held_single("srb"), 0, "bottom")[0]
	var tk := mj.attach_stack(CraftModel.held_single("tank_m"), sb, "bottom")[0]
	var ph: Array = mj.stats().get("plume_hits", [])
	var jet_ok := ph.size() == 1 and int(ph[0][0]) == tk and int(ph[0][1]) == sb and float(ph[0][3]) > float(Sim.part("tank_m").thermal.max_temp)
	var fs := FlightSession.new()
	fs.spawn_in_orbit(mj.save_text(), "floe", 400000.0)
	fs.stage()
	fs.step_ticks(250)
	var burnt := false
	for e in fs.events:
		burnt = burnt or (e.kind == "part_destroyed" and e.part == "tank_m" and float(e.speed) <= -2.0)
	_check(hot.is_empty() and jet_ok and burnt, "exhaust: stock craft fire into nothing (%s); tank under an SRB flagged (%s) and burns up in flight %s" % [hot, ph, burnt])
	# engines tilted 30 deg about the tank's tangent: the hangar counts only
	# the net thrust (cos 30 of it), the flight agrees, and the jets that run
	# alongside the tank do not touch it
	var straight := 0.0
	var tilted := 0.0
	var flown := 0.0
	var cant_hits := []
	for tilt in [0.0, 30.0]:
		var mc := CraftModel.new()
		mc.place_root(CraftModel.held_single("probe_core"))
		var ct := mc.attach_stack(CraftModel.held_single("tank_l"), 0, "bottom")[0]
		var ce := mc.attach_surface(CraftModel.held_single("engine_small"), ct, 0.0, -0.8, 2)
		mc.rotate_part(ce[0], [0.0, 0.0, 1.0], deg_to_rad(tilt))
		var cst: Dictionary = mc.stats()
		cant_hits.append_array(cst.get("plume_hits", []))
		if tilt == 0.0:
			straight = cst.dv_vac
		else:
			tilted = cst.dv_vac
			var cs := FlightSession.new()
			cs.spawn_in_orbit(mc.save_text(), "floe", 400000.0)
			cs.set_throttle(1.0)
			cs.stage()
			cs.step_ticks(int(cst.stages[0].burn_time * 50.0) + 50)
			flown = cs.telemetry().get("dv_expended", -1.0)
	var ratio := tilted / straight
	_check(absf(ratio - cos(deg_to_rad(30.0))) < 1e-9 and absf(flown - tilted) / tilted < 1e-4 and cant_hits.is_empty(),
		"tilted engines: hangar dv %.1f -> %.1f m/s (x%.9f, cos 30 = %.9f), flown %.1f m/s, exhaust hits %s" % [straight, tilted, ratio, cos(deg_to_rad(30.0)), flown, cant_hits])
	# RCS: four blocks on a mono drum (one placement, 4x symmetry); the
	# hangar's sliding delta-v is what a slide along the nose gets from all
	# the mono
	var mr := CraftModel.new()
	mr.place_root(CraftModel.held_single("probe_core"))
	var md := mr.attach_stack(CraftModel.held_single("mono_tank"), 0, "bottom")[0]
	mr.attach_stack(CraftModel.held_single("tank_s"), md, "bottom")
	var blocks := mr.attach_surface(CraftModel.held_single("rcs_block"), md, 0.0, 0.0, 4)
	var rst: Dictionary = mr.stats()
	var rs := FlightSession.new()
	rs.spawn_in_orbit(mr.save_text(), "floe", 400000.0)
	rs.set_sas("off")
	rs.set_rcs(true)
	rs.set_translate(0.0, 1.0, 0.0)
	var guard := 0
	while rs.telemetry().get("mono", 0.0) > 0.0 and guard < 50 * 400:
		rs.step_ticks(50)
		guard += 50
	rs.step_ticks(5)
	var rt := rs.telemetry()
	var slid: float = rt.get("rcs_dv_expended", -1.0)
	var mb := CraftModel.new()
	mb.place_root(CraftModel.held_single("probe_core"))
	var mbt := mb.attach_stack(CraftModel.held_single("tank_s"), 0, "bottom")[0]
	mb.attach_surface(CraftModel.held_single("rcs_block"), mbt, 0.0, 0.0, 2)
	var mono_left: float = mb.stats().get("mono", -1.0)  # the probe core's own 10 kg
	_check(blocks.size() == 4 and absf(slid - float(rst.rcs_dv)) / float(rst.rcs_dv) < 1e-6 and rt.get("mono", 1.0) == 0.0 and mono_left == 10.0,
		"RCS: hangar %d blocks, mono %.0f kg, sliding dv %.4f m/s; flown dry %.4f m/s; spin %s rad/s; probe core alone carries %.0f kg" % [
			int(rst.rcs_blocks), rst.mono, rst.rcs_dv, slid, String.num_scientific(float(rt.get("spin", 0.0))), mono_left])
	_check(ok_sym and spread < 1e-9 and after_undo == before and redo_ok and removed_ok and replaced and removed_sym,
		"editor ops: 6x symmetry, rotate/offset counterparts (spread %s), undo x3 == original %s, redo %s, pick-up/re-attach identical %s, symmetric delete %s" % [
			String.num_scientific(spread), after_undo == before, redo_ok, replaced, removed_sym])
	_log.append("     sym %s n=%d->%d, removed_ok %s" % [ok_sym, n0, m.parts.size(), removed_ok])


## Slipping a tank into the middle of a stack moves everything below it
## down by exactly the tank's node spacing; fins added to one of two side
## boosters land on both. Both undo cleanly.
func _insert_ops() -> void:
	var m := _build_orbiter()
	var before := m.save_text()
	var n0 := m.parts.size()
	var t1a := -1
	var eng := -1
	for i in m.parts.size():
		if m.parts[i].part == "tank_l":
			t1a = i
		if m.parts[i].part == "engine_lifter":
			eng = i
	var y0: float = m.parts[eng].pos[1]
	var dv0: float = m.stats().dv_vac
	var got := m.insert_stack(CraftModel.held_single("tank_s"), t1a, "bottom")
	var eng1 := -1
	for i in m.parts.size():
		if m.parts[i].part == "engine_lifter":
			eng1 = i
	var nodes: Array = Sim.part("tank_s").nodes
	var span := absf(float(nodes[0].pos[1]) - float(nodes[1].pos[1]))
	var shift: float = y0 - m.parts[eng1].pos[1]
	var dv1: float = m.stats().dv_vac
	var ins_ok: bool = got.size() == 1 and m.parts.size() == n0 + 1 and absf(shift - span) < 1e-9 and dv1 > dv0
	m.undo()
	var undo_ok := m.save_text() == before
	# boosters on two radial decouplers, then one fin on one booster -> two fins
	var decs := m.attach_surface(CraftModel.held_single("decoupler_radial"), t1a, 0.0, 0.0, 2)
	var boosters := m.attach_stack(CraftModel.held_single("srb"), decs[0], "outer")
	var nb := m.parts.size()
	var fins := m.attach_surface(CraftModel.held_single("fin"), boosters[0], 0.3, -0.8, 1)
	var mirror_ok: bool = boosters.size() == 2 and fins.size() == 2 and m.parts.size() == nb + 2 \
		and m.parts[fins[0]].parent != m.parts[fins[1]].parent
	_check(ins_ok and undo_ok and mirror_ok,
		"insert/mirror: tank slipped into the stack moves the rest %.3f m (node span %.3f), dv %.0f -> %.0f m/s, undo %s; fin on one of 2 boosters -> %d fins %s" % [
			shift, span, dv0, dv1, undo_ok, fins.size(), mirror_ok])


## Structural equality: same parts, tree, nodes, stages and symmetry
## partition, positions/rotations within 1e-12.
func _same_craft(a: CraftModel, b: CraftModel) -> bool:
	if a.parts.size() != b.parts.size():
		return false
	var sym_ab := {}
	for k in a.parts.size():
		var p: Dictionary = a.parts[k]
		var q: Dictionary = b.parts[k]
		if p.part != q.part or p.parent != q.parent or p.node != q.node or p.parent_node != q.parent_node:
			return false
		if p.stage != q.stage or (p.sym == 0) != (q.sym == 0):
			return false
		if p.sym != 0:
			if sym_ab.get(p.sym, q.sym) != q.sym:
				return false
			sym_ab[p.sym] = q.sym
		if DMath.length(DMath.sub(p.pos, q.pos)) > 1e-12:
			return false
		for i in 4:
			if absf(p.rot[i] - q.rot[i]) > 1e-12:
				return false
	return true


## Drag-and-drop staging edits, as the staging list performs them.
func _staging_ops() -> void:
	var m := _build_orbiter()
	var n0 := m.stage_count()
	var dv0: float = m.stats().dv_vac
	var alb := -1
	for i in m.parts.size():
		if m.parts[i].part == "engine_vac":
			alb = i
	# auto staging gives the decoupler and the vacuum engine a press each:
	# merge them into one stage, then split them again with an inserted stage
	m.set_stage(alb, m.parts[alb].stage + 1)
	var moved: bool = m.stage_count() == n0 - 1 and m.parts[alb].stage == 1
	m.insert_stage(alb, 1)
	var inserted: bool = m.stage_count() == n0 and m.parts[alb].stage == 1
	var st := m.stats()
	var dv_ok: bool = absf(st.dv_vac - dv0) < 1.0
	m.undo()
	m.undo()
	var back: bool = m.stage_count() == n0 and m.parts[alb].stage == 1
	m.reset_staging()
	var reset := m.stage_count() == n0
	_check(inserted and dv_ok and moved and back and reset,
		"staging edits: insert stage %s (dv %.1f m/s), move %s, undo %s, auto reset %s" % [inserted, st.dv_vac, moved, back, reset])
	# whole-stage moves: the vacuum engine's stage one press earlier swaps it
	# with the decoupler above it; dragging it back and undo restore the plan
	var s_alb: int = m.parts[alb].stage
	var dec_mates := []
	for i in m.parts.size():
		if m.parts[i].stage == s_alb + 1:
			dec_mates.append(i)
	m.move_stage(s_alb, s_alb + 2)
	var swapped: bool = m.parts[alb].stage == s_alb + 1 and m.stage_count() == n0
	for i in dec_mates:
		swapped = swapped and m.parts[i].stage == s_alb
	m.move_stage(s_alb + 1, s_alb)
	var restored: bool = m.parts[alb].stage == s_alb
	m.undo()
	m.undo()
	restored = restored and m.parts[alb].stage == s_alb
	m.merge_stage(s_alb, s_alb + 1)
	var merged: bool = m.stage_count() == n0 - 1 and m.parts[alb].stage == s_alb
	_check(swapped and restored and merged,
		"stage moves: swap earlier %s, back + undo %s, merge %s" % [swapped, restored, merged])
	# thrust limits: saved only when set, survive a reload, scale TWR and
	# burn time exactly, and leave delta-v alone (one engine per stage)
	var ml := _build_orbiter()
	var lifter := -1
	for i in ml.parts.size():
		if ml.parts[i].part == "engine_lifter":
			lifter = i
	var s_full := ml.stats()
	var plain := ml.save_text()
	var set_to := ml.set_limit(lifter, 0.6)
	var text := ml.save_text()
	var reloaded := CraftModel.new()
	reloaded.load_text(text)
	var s_lim := reloaded.stats()
	var st_full: Dictionary = s_full.stages[0]
	var st_lim: Dictionary = s_lim.stages[0]
	var twr_ratio: float = st_lim.twr_sl / st_full.twr_sl
	var burn_ratio: float = st_lim.burn_time / st_full.burn_time
	var lim_ok: bool = set_to == 0.6 and not plain.contains("\"limit\"") and text.contains("\"limit\": 0.6") \
		and reloaded.save_text() == text and absf(twr_ratio - 0.6) < 1e-12 and absf(burn_ratio - 1.0 / 0.6) < 1e-9 \
		and absf(st_lim.dv_vac - st_full.dv_vac) < 1e-9
	ml.undo()
	lim_ok = lim_ok and ml.save_text() == plain
	_check(lim_ok, "thrust limit 60%%: saved and reloaded identical %s, TWR x%.15f, burn time x%.12f, dv %.3f -> %.3f m/s, undo restores %s" % [
		reloaded.save_text() == text, twr_ratio, burn_ratio, st_full.dv_vac, st_lim.dv_vac, ml.save_text() == plain])
