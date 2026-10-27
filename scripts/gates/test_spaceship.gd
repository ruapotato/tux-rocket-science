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
## --test-spaceship: building past tubes, and flying it. In the hangar's
## model: stretching a tank in the middle of a stack keeps everything on
## its nodes; a part stuck on a box lands flat on the face it is aimed at;
## mirror symmetry puts a wing's twin on the other side, mirrored; sizes
## survive saving, and the readout's mass is the parts' at their sizes.
## Then the stock Skua from the pad: the Hearth Torch pushes what its Isp
## at the pad's pressure gives, draws its 40 MW, lifts the ship off, and
## the cabin weighs what the thrust gives.
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

	# 1. stretching a tank in the middle of a stack
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("pod_3"))
	var ad := m.attach_stack(CraftModel.held_single("adapter_21"), 0, "bottom")[0]
	var tank := m.attach_stack(CraftModel.held_single("tank_stretch"), ad, "bottom")[0]
	var eng := m.attach_stack(CraftModel.held_single("engine_big"), tank, "bottom")[0]
	var fin := m.attach_surface(CraftModel.held_single("fin"), tank, 0.0, -0.8, 1)[0]
	var fin_before: Array = m.parts[fin].pos.duplicate()
	m.set_size(tank, [1.5, 2.5, 1.5])
	var top_t: Array = m.node_world(tank, "top").pos
	var top_a: Array = m.node_world(ad, "bottom").pos
	var bot_t: Array = m.node_world(tank, "bottom").pos
	var top_e: Array = m.node_world(eng, "top").pos
	var fr := DMath.length(DMath.sub([m.parts[fin].pos[0], 0, m.parts[fin].pos[2]], [m.parts[tank].pos[0], 0, m.parts[tank].pos[2]]))
	_check(DMath.length(DMath.sub(top_t, top_a)) < 1e-9 and DMath.length(DMath.sub(bot_t, top_e)) < 1e-9 and fr > float(fin_before[2]) + 0.5,
		"stretched to 1.5 x 2.5: still on the adapter (%s m), the engine still on its bottom (%s m), the fin moved out to %.3f m (was %.3f)" % [
			str(DMath.length(DMath.sub(top_t, top_a))), str(DMath.length(DMath.sub(bot_t, top_e))), fr, float(fin_before[2])])
	m.undo()
	_check(not m.parts[tank].has("size") and DMath.length(DMath.sub(m.node_world(tank, "bottom").pos, m.node_world(eng, "top").pos)) < 1e-9,
		"undo puts it back as it was")

	# 2. on a box's face, 3. mirrored
	var b := CraftModel.new()
	b.place_root(CraftModel.held_single("hull_box"))
	b.set_size(0, [1.6, 1.0, 1.0])
	var hw := 2.5 * 1.6 * 0.5
	var bat := b.attach_surface(CraftModel.held_single("battery"), 0, deg_to_rad(60.0), 0.3, 1)[0]
	var off := CraftModel.surface_offset("battery")
	var bp: Array = b.parts[bat].pos
	var bn := DMath.q_rotate(b.parts[bat].rot, [0.0, 0.0, 1.0])
	_check(absf(float(bp[0]) - (hw + off)) < 1e-9 and absf(float(bn[0]) - 1.0) < 1e-9,
		"a battery aimed at 60 deg on a 4 m wide box sits on its east face: %.3f m out (face %.3f + %.3f), facing +X" % [float(bp[0]), hw, off])
	var wing := CraftModel.held_single("wing")
	wing.parts[0]["size"] = [1.0, 1.2, 1.3]
	var ws := b.attach_surface(wing, 0, PI / 2, -0.4, CraftModel.MIRROR)
	var w0: Dictionary = b.parts[ws[0]]
	var w1: Dictionary = b.parts[ws[1]] if ws.size() > 1 else {}
	var tip0 := DMath.add(w0.pos, DMath.q_rotate(w0.rot, [0.0, 0.0, 3.0]))
	var tip1 := DMath.add(w1.pos, DMath.q_rotate(w1.rot, [0.0, 0.0, 3.0])) if not w1.is_empty() else [0, 0, 0]
	_check(ws.size() == 2 and absf(float(w0.pos[0]) + float(w1.pos[0])) < 1e-9 and absf(float(tip0[0]) + float(tip1[0])) < 1e-9 and
		absf(float(tip0[1]) - float(tip1[1])) < 1e-9 and w1.get("size", []) == [1.0, 1.2, 1.3] and int(w0.sym) != 0 and w0.sym == w1.sym,
		"mirror: the wing's twin at x %.3f (it is at %.3f), its tip mirrored too (%.3f / %.3f), same size, one symmetry group" % [
			float(w1.get("pos", [0])[0]), float(w0.pos[0]), float(tip1[0]), float(tip0[0])])
	# saved and loaded; the readout weighs the parts at their sizes
	var text := b.save_text()
	var b2 := CraftModel.new()
	b2.load_text(text)
	var st: Dictionary = b2.stats()
	var want := 0.0
	for p in b2.parts:
		var s := CraftModel.size_of(p)
		var d := Sim.part(p.part)
		want += (float(d.mass) + float(d.get("fuel_liquid", 0.0))) * float(s[0]) * float(s[1]) * float(s[2])
	_check(b2.parts[0].get("size", []) == [1.6, 1.0, 1.0] and b2.parts[ws[1]].get("size", []) == [1.0, 1.2, 1.3] and absf(float(st.mass) - want) < 1e-6 * want,
		"saved and loaded: sizes kept; the readout's %.1f kg is the parts' at their sizes (%.1f)" % [float(st.mass), want])

	# 4. the Skua from the pad
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			c.queue_free()
	await _frames(tree, 2)
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "skua.craft.json")
	Sim.pending_site = "pad"
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	scene._throttle = 1.0
	scene._stage()
	await _frames(tree, 2)
	s.step_ticks(5)
	var t := s.telemetry()
	var torch: Dictionary = Sim.part("hearth_torch").engine
	var p0 := float(t.pressure)
	var isp := float(torch.isp_sl) + (float(torch.isp_vac) - float(torch.isp_sl)) * (1.0 - p0 / 101325.0)
	var thrust_want := float(torch.thrust_vac) * isp / float(torch.isp_vac)
	_check(absf(float(t.thrust) - thrust_want) < 1e-3 * thrust_want and absf(float(t.get("engine_kw", 0.0)) - float(torch.kw)) < 1e-6,
		"on the pad (%.0f Pa): the Torch pushes %.1f kN (Isp %.0f s there gives %.1f) and draws %.0f kW" % [p0, float(t.thrust) / 1000.0, isp,
			thrust_want / 1000.0, float(t.get("engine_kw", 0.0))])
	for k in 15:
		s.step_ticks(50)
		await _frames(tree, 1)
	t = s.telemetry()
	_check(not t.landed and float(t.altitude_terrain) > 100.0 and float(t.vertical_speed) > 20.0,
		"15 s later: %.0f m up, climbing at %.1f m/s, on one stage" % [float(t.altitude_terrain), float(t.vertical_speed)])
	# the cabin weighs what the thrust gives
	scene._toggle_inside()
	for k in 30:
		s.step_ticks(5)
		await _frames(tree, 1)
	t = s.telemetry()
	var wk: Dictionary = t.get("walker", {})
	var g_want := float(t.g_force)
	_check(not wk.is_empty() and wk.standing and absf(float(wk.felt_g) - g_want) < 0.02,
		"inside the %s under the Torch: stands, feeling %.3f g (the ship's %.3f g)" % [wk.get("room_name", "?"), float(wk.get("felt_g", 0.0)), g_want])
	scene._leave_inside()
	scene.queue_free()
	await _frames(tree, 2)
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	Sim.save_game()
	return {"status": "FAIL" if _failed else "PASS", "log": "\n".join(_log)}
