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
## Dev: a monkey in the hangar. The mouse jumps around the screen and clicks
## (picking up, attaching, setting aside), parts come off the palette, keys
## rotate / offset / delete / change symmetry and thrust limits, undo and
## redo, staging cards are dragged about and new crafts started. After
## every action the craft must save and load back to the same text, its
## stats must be sane, and at the end undoing everything and redoing it
## must give the same craft. Needs the GPU (mouse picking). Env: SEED, N.
extends RefCounted

var _rng := RandomNumberGenerator.new()
var _log := PackedStringArray()
var _bad := 0
var _seen := {}


func _flaw(what: String) -> void:
	var key := what.get_slice(":", 0)
	_seen[key] = int(_seen.get(key, 0)) + 1
	if _seen[key] <= 3:
		_log.append("  FLAW: " + what)
	_bad += 1


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	_rng.seed = int(OS.get_environment("SEED")) if OS.get_environment("SEED") != "" else 1
	var n := int(OS.get_environment("N")) if OS.get_environment("N") != "" else 1500
	if tree.current_scene:
		tree.current_scene.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	tree.root.add_child(hangar)
	for i in 5:
		await tree.process_frame
	var ids: Array = Sim.catalog_order.duplicate()
	var vp := hangar.get_viewport()
	var size := vp.get_visible_rect().size
	var actions := {}
	var m: CraftModel = hangar.model
	var history_start := m.save_text()
	for step in n:
		var r := _rng.randf()
		var what := ""
		if r < 0.25:
			what = "palette"
			hangar._on_palette(ids[_rng.randi() % ids.size()])
		elif r < 0.6:
			what = "click"
			# aim near the craft most of the time
			var c := size * 0.5 + Vector2(_rng.randfn(0, size.x * 0.12), _rng.randfn(0, size.y * 0.2))
			vp.warp_mouse(c)
			await tree.process_frame
			await tree.process_frame
			hangar._click()
		elif r < 0.75:
			var k: int = [KEY_W, KEY_S, KEY_A, KEY_D, KEY_Q, KEY_E, KEY_X, KEY_DELETE, KEY_ESCAPE, KEY_BRACKETLEFT, KEY_BRACKETRIGHT,
				KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_PAGEUP, KEY_F][_rng.randi() % 17]
			what = "key " + OS.get_keycode_string(k)
			hangar._key(_ev(k, _rng.randf() < 0.2))
		elif r < 0.85:
			what = "undo" if _rng.randf() < 0.6 else "redo"
			var e := _ev(KEY_Z, _rng.randf() < 0.3)
			e.ctrl_pressed = true
			if what == "redo":
				e.shift_pressed = true
			hangar._key(e)
		elif r < 0.95 and m.stage_count() > 0:
			what = "staging"
			var sc := m.stage_count()
			var pi := _rng.randi() % maxi(1, m.parts.size())
			match _rng.randi() % 4:
				0: hangar._drop_on_gap({"kind": "stage_part", "index": pi}, _rng.randi() % (sc + 1))
				1: hangar._drop_on_stage({"kind": "stage_part", "index": pi}, _rng.randi() % sc)
				2: hangar._drop_on_gap({"kind": "stage", "stage": _rng.randi() % sc}, _rng.randi() % (sc + 1))
				3: hangar._drop_on_stage({"kind": "stage", "stage": _rng.randi() % sc}, _rng.randi() % sc)
		elif r < 0.97:
			what = "new"
			hangar._on_new()
		else:
			what = "load"
			m.load_text(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + ["orbiter", "sidecar", "rookery_heavy", "igloo_station", "krill_lander"][_rng.randi() % 5] + ".craft.json"))
		actions[what.get_slice(" ", 0)] = int(actions.get(what.get_slice(" ", 0), 0)) + 1
		await tree.process_frame
		_check(m, "after %s (step %d)" % [what, step])
	# undo everything, then redo everything
	var end := m.save_text()
	var undos := 0
	while m.can_undo() and undos < 5000:
		m.undo()
		undos += 1
	var bottom := m.save_text()
	var redos := 0
	while m.can_redo() and redos < 5000:
		m.redo()
		redos += 1
	if m.save_text() != end:
		_flaw("undo/redo: %d undos then %d redos don't give back the craft" % [undos, redos])
	_log.append("%d actions %s; end craft %d parts, %d stages; undo depth %d (bottom %d parts)" % [n, actions, m.parts.size(), m.stage_count(), undos, CraftModel.new().parts.size() if bottom == "" else JSON.parse_string(bottom).parts.size()])
	return {"status": "PASS" if _bad == 0 else "FAIL", "log": "\n".join(_log)}


func _check(m: CraftModel, when: String) -> void:
	if m.parts.is_empty():
		return  # an empty craft has nothing to save
	var text := m.save_text()
	var m2 := CraftModel.new()
	if not m2.load_text(text):
		_flaw("reload: craft text refused %s" % when)
		return
	if m2.save_text() != text:
		_flaw("roundtrip: save/load changes the craft %s" % when)
	for i in m.parts.size():
		var p: Dictionary = m.parts[i]
		if (i == 0 and p.parent != -1) or (i > 0 and (p.parent < 0 or p.parent >= i)):
			_flaw("tree: part %d (%s) has parent %d %s" % [i, p.part, p.parent, when])
		if p.stage < -1 or p.stage >= maxi(1, m.stage_count()):
			_flaw("stage: part %d stage %d of %d %s" % [i, p.stage, m.stage_count(), when])
	if m.parts.is_empty():
		return
	var st := m.stats()
	for s in st.get("stages", []):
		for k in ["dv_vac", "dv_sl", "burn_time", "twr_vac"]:
			var v = s.get(k, 0.0)
			if typeof(v) == TYPE_FLOAT and (not is_finite(v) or v < 0.0):
				_flaw("stats: stage %s %s = %s %s" % [s.get("stage", "?"), k, v, when])
	for k in ["mass", "dv_total", "height"]:
		var v = st.get(k, 0.0)
		if typeof(v) == TYPE_FLOAT and not is_finite(v):
			_flaw("stats: %s = %s %s" % [k, v, when])
	# stages must be compact: every stage number below the count used
	var used := {}
	for p in m.parts:
		used[p.stage] = true
	for s in m.stage_count():
		if not used.has(s):
			_flaw("stages: stage %d empty of %d %s" % [s, m.stage_count(), when])


func _ev(code: Key, shift: bool) -> InputEventKey:
	var e := InputEventKey.new()
	e.keycode = code
	e.physical_keycode = code
	e.pressed = true
	e.shift_pressed = shift
	return e
