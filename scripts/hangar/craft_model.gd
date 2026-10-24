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
## The hangar's editable craft. Pure data + editor operations, no UI, so the
## headless gates drive exactly the code the Assembly Hangar uses.
##
## Geometry is kept in double-precision float arrays (see DMath). The saved
## form is the canonical text produced by trs_core.
class_name CraftModel
extends RefCounted

signal changed

const UNDO_LIMIT := 200
const UP := [0.0, 1.0, 0.0]

var craft_name := "Untitled"
var description := ""
## Each part: {part, parent, node, parent_node, pos[3], rot[4], stage, sym}
## plus, when not the default, fuel (kg) and limit (engine thrust fraction).
var parts: Array = []
## When false, any structural change re-runs auto staging.
var manual_staging := false

var _undo: Array[Dictionary] = []
var _redo: Array[Dictionary] = []


# ---- serialization ---------------------------------------------------------
# trs_core owns the file format; see Sim.encode/decode for why numbers never
# pass through text on this side.

const FLOATS_PER_PART := 9


func to_meta() -> Dictionary:
	var out_parts := []
	for p in parts:
		out_parts.append({
			"part": p.part,
			"parent": int(p.parent),
			"node": p.node,
			"parent_node": p.parent_node,
			"stage": int(p.stage),
			"sym": int(p.sym),
		})
	return {"name": craft_name, "description": description, "parts": out_parts}


func to_floats() -> PackedFloat64Array:
	var f := PackedFloat64Array()
	f.resize(parts.size() * FLOATS_PER_PART)
	for i in parts.size():
		var p: Dictionary = parts[i]
		var pos := DMath.clean(p.pos)
		var rot := DMath.clean(p.rot)
		var o := i * FLOATS_PER_PART
		for k in 3:
			f[o + k] = pos[k]
		for k in 4:
			f[o + 3 + k] = rot[k]
		f[o + 7] = p.get("fuel", -1.0)
		f[o + 8] = p.get("limit", 1.0)
	return f


func load_bin(meta: Dictionary, floats: PackedFloat64Array) -> void:
	craft_name = meta.get("name", "Untitled")
	description = meta.get("description", "")
	parts.clear()
	var mp: Array = meta.get("parts", [])
	for i in mp.size():
		var p: Dictionary = mp[i]
		var o := i * FLOATS_PER_PART
		var q := {
			"part": p.part,
			"parent": int(p.parent),
			"node": p.node,
			"parent_node": p.parent_node,
			"pos": [floats[o], floats[o + 1], floats[o + 2]],
			"rot": [floats[o + 3], floats[o + 4], floats[o + 5], floats[o + 6]],
			"stage": int(p.stage),
			"sym": int(p.sym),
		}
		if floats[o + 7] >= 0.0:
			q["fuel"] = floats[o + 7]
		if floats[o + 8] != 1.0:
			q["limit"] = floats[o + 8]
		parts.append(q)
	changed.emit()


func save_text() -> String:
	return Sim.encode(to_meta(), to_floats())


func load_text(text: String) -> bool:
	var d := Sim.decode(text)
	if d.has("error"):
		push_warning("craft load: " + str(d.error))
		return false
	_undo.clear()
	_redo.clear()
	load_bin(d.meta, d.floats)
	manual_staging = true  # a loaded craft keeps its staging until reset
	return true


func save_file(path: String) -> Error:
	var text := save_text()
	if text == "":
		return ERR_INVALID_DATA
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		return FileAccess.get_open_error()
	f.store_string(text)
	return OK


func load_file(path: String) -> bool:
	if not FileAccess.file_exists(path):
		return false
	return load_text(FileAccess.get_file_as_string(path))


func stats(body_id := "floe") -> Dictionary:
	if parts.is_empty():
		return {}
	return Sim.stats(to_meta(), to_floats(), body_id)


## Stats exactly as trs_core prints them (for byte comparisons).
func stats_raw(body_id := "floe") -> String:
	return Sim.core.craft_stats_bin(JSON.stringify(to_meta()), to_floats(), body_id)


static func _floats(a: Array) -> Array:
	var out := []
	for x in a:
		out.append(float(x))
	return out


# ---- undo / redo --------------------------------------------------------------
# Snapshots are exact deep copies (never serialized through text).

func _snapshot() -> Dictionary:
	return {"parts": parts.duplicate(true), "name": craft_name, "desc": description, "manual": manual_staging}


func checkpoint() -> void:
	_undo.append(_snapshot())
	if _undo.size() > UNDO_LIMIT:
		_undo.pop_front()
	_redo.clear()


func can_undo() -> bool:
	return not _undo.is_empty()


func can_redo() -> bool:
	return not _redo.is_empty()


func undo() -> void:
	if _undo.is_empty():
		return
	_redo.append(_snapshot())
	_restore(_undo.pop_back())


func redo() -> void:
	if _redo.is_empty():
		return
	_undo.append(_snapshot())
	_restore(_redo.pop_back())


func _restore(s: Dictionary) -> void:
	parts = s.parts.duplicate(true)
	craft_name = s.name
	description = s.desc
	manual_staging = s.manual
	changed.emit()


# ---- queries -----------------------------------------------------------------

func children_of(i: int) -> Array[int]:
	var out: Array[int] = []
	for k in range(i + 1, parts.size()):
		if parts[k].parent == i:
			out.append(k)
	return out


func subtree(i: int) -> Array[int]:
	var mask := {i: true}
	var out: Array[int] = [i]
	for k in range(i + 1, parts.size()):
		if mask.has(int(parts[k].parent)):
			mask[k] = true
			out.append(k)
	return out


func counterparts(i: int) -> Array[int]:
	var out: Array[int] = []
	var s: int = parts[i].sym
	if s == 0:
		out.append(i)
		return out
	for k in parts.size():
		if parts[k].sym == s:
			out.append(k)
	return out


func node_world(i: int, node_name: String) -> Dictionary:
	var n := Sim.node_of(parts[i].part, node_name)
	if n.is_empty():
		return {}
	var p: Dictionary = parts[i]
	return {
		"pos": DMath.add(p.pos, DMath.q_rotate(p.rot, _floats(n.pos))),
		"dir": DMath.q_rotate(p.rot, _floats(n.dir)),
	}


func node_occupied(i: int, node_name: String) -> bool:
	if parts[i].parent >= 0 and parts[i].node == node_name:
		return true
	for k in children_of(i):
		if parts[k].parent_node == node_name:
			return true
	return false


## Free stack nodes: [{index, node, pos, dir}]
func free_nodes() -> Array:
	var out := []
	for i in parts.size():
		for n in Sim.part(parts[i].part).get("nodes", []):
			if not node_occupied(i, n.name):
				var w := node_world(i, n.name)
				out.append({"index": i, "node": n.name, "pos": w.pos, "dir": w.dir})
	return out


static func surface_offset(part_id: String) -> float:
	var sh: Dictionary = Sim.part(part_id).get("shape", {})
	match sh.get("type", ""):
		"fin", "leg", "hose":
			return 0.0
		"pylon":
			return sh.get("depth", 0.3) * 0.5
		_:
			return sh.get("radius", sh.get("radius_bottom", 0.5))


## Radius of a part's body at local height y (for surface attachment).
static func radius_at(part_id: String, y: float) -> float:
	var sh: Dictionary = Sim.part(part_id).get("shape", {})
	var h: float = sh.get("height", 1.0)
	if sh.has("radius_top"):
		var t := clampf((y + h * 0.5) / h, 0.0, 1.0)
		return lerpf(sh.get("radius_bottom", 0.5), sh.radius_top, t)
	return sh.get("radius", 0.5)


static func part_height(part_id: String) -> float:
	var sh: Dictionary = Sim.part(part_id).get("shape", {})
	return sh.get("height", sh.get("length", sh.get("root", 1.0)))


# ---- held assemblies -------------------------------------------------------------
# A "held" assembly is a detached subtree expressed relative to its root
# part's frame; every placement places one (a new part is a 1-part held).

static func held_single(part_id: String) -> Dictionary:
	return {"parts": [{
		"part": part_id, "parent": -1, "node": "", "parent_node": "",
		"pos": [0.0, 0.0, 0.0], "rot": DMath.q_identity(), "stage": -1, "sym": 0,
	}]}


## Removes i's subtree (one instance; counterparts are removed too) and
## returns it as a held assembly relative to part i.
func pick_up(i: int) -> Dictionary:
	if i <= 0:
		return {}
	checkpoint()
	var held := _extract(i)
	_remove_indices(_with_counterpart_subtrees(i))
	_after_structure_change()
	return held


func _extract(i: int) -> Dictionary:
	var ids := subtree(i)
	var root: Dictionary = parts[i]
	var inv := DMath.q_conj(root.rot)
	var map := {}
	var out := []
	for k in ids:
		var p: Dictionary = parts[k]
		map[k] = out.size()
		out.append({
			"part": p.part,
			"parent": -1 if k == i else map[int(p.parent)],
			"node": "" if k == i else p.node,
			"parent_node": "" if k == i else p.parent_node,
			"pos": DMath.q_rotate(inv, DMath.sub(p.pos, root.pos)),
			"rot": DMath.q_mul(inv, p.rot),
			"stage": p.stage,
			# symmetry inside the subtree survives only if the whole group is in it
			"sym": p.sym if k != i and _group_within(p.sym, ids) else 0,
		})
		if p.has("limit"):
			out[-1]["limit"] = p.limit
	return {"parts": out, "root_node": parts[i].node}


func _group_within(sym: int, ids: Array[int]) -> bool:
	if sym == 0:
		return false
	for k in parts.size():
		if parts[k].sym == sym and not ids.has(k):
			return false
	return true


func _with_counterpart_subtrees(i: int) -> Array:
	var all := {}
	for c in counterparts(i):
		for k in subtree(c):
			all[k] = true
	return all.keys()


func remove(i: int) -> void:
	if i < 0 or i >= parts.size():
		return
	checkpoint()
	if i == 0:
		parts.clear()
	else:
		_remove_indices(_with_counterpart_subtrees(i))
	_after_structure_change()


func _remove_indices(ids: Array) -> void:
	var drop := {}
	for k in ids:
		drop[int(k)] = true
	var remap := {}
	var kept := []
	for k in parts.size():
		if drop.has(k):
			continue
		var p: Dictionary = parts[k].duplicate(true)
		remap[k] = kept.size()
		p.parent = -1 if p.parent < 0 else remap[int(p.parent)]
		kept.append(p)
	parts = kept
	_cleanup_symmetry()


## Drops symmetry groups that have only one member left.
func _cleanup_symmetry() -> void:
	var count := {}
	for p in parts:
		if p.sym != 0:
			count[p.sym] = count.get(p.sym, 0) + 1
	for p in parts:
		if p.sym != 0 and count[p.sym] < 2:
			p.sym = 0


func _next_sym() -> int:
	var m := 0
	for p in parts:
		m = maxi(m, p.sym)
	return m + 1


## Appends a held assembly with its root at world transform (pos, rot),
## attached to parent via (node, parent_node). Returns the root's index.
func _append_held(held: Dictionary, parent: int, node: String, parent_node: String, pos: Array, rot: Array,
		sym: int) -> int:
	var base := parts.size()
	var sym_map := {}  # held-internal symmetry ids -> fresh ids
	var next := maxi(_next_sym(), sym + 1)
	for k in held.parts.size():
		var h: Dictionary = held.parts[k]
		if k > 0 and h.sym != 0 and not sym_map.has(h.sym):
			sym_map[h.sym] = next
			next += 1
		parts.append({
			"part": h.part,
			"parent": parent if k == 0 else base + int(h.parent),
			"node": node if k == 0 else h.node,
			"parent_node": parent_node if k == 0 else h.parent_node,
			"pos": DMath.add(pos, DMath.q_rotate(rot, h.pos)),
			"rot": DMath.q_normalized(DMath.q_mul(rot, h.rot)),
			"stage": h.stage,
			"sym": sym if k == 0 else sym_map.get(h.sym, 0),
		})
		if h.has("limit"):
			parts[-1]["limit"] = h.limit
	return base


# ---- placement ------------------------------------------------------------------

func place_root(held: Dictionary) -> int:
	if not parts.is_empty():
		return -1
	checkpoint()
	_append_held(held, -1, "", "", [0.0, 0.0, 0.0], DMath.q_identity(), 0)
	_after_structure_change()
	return 0


## Picks which of the held root's nodes mates with a target node, and the
## rotation that makes the two node directions oppose each other.
func _stack_fit(held: Dictionary, target_dir: Array) -> Dictionary:
	var root_part: String = held.parts[0].part
	var want := DMath.scale(target_dir, -1.0)
	var best := {}
	var best_dot := -INF
	var prefer: String = held.get("root_node", "")
	for n in Sim.part(root_part).get("nodes", []):
		# a held subtree can't reuse nodes its own children occupy
		var used := false
		for k in range(1, held.parts.size()):
			if held.parts[k].parent == 0 and held.parts[k].parent_node == n.name:
				used = true
		if used:
			continue
		var d := DMath.dot(_floats(n.dir), want) + (0.001 if n.name == prefer else 0.0)
		if d > best_dot:
			best_dot = d
			best = n
	if best.is_empty():
		return {}
	var rot := DMath.q_from_to(_floats(best.dir), want)
	return {"node": best.name, "rot": rot, "node_pos": _floats(best.pos)}


## Where a stack attachment of `held` at (target, target_node) would put each
## symmetric copy: [{parent, node, parent_node, pos, rot}]. Empty = invalid.
## `spin` turns the held assembly about the attach axis.
func stack_placements(held: Dictionary, target: int, target_node: String, spin := 0.0) -> Array:
	var out := []
	if target < 0 or target >= parts.size() or node_occupied(target, target_node):
		return out
	var tn := node_world(target, target_node)
	if tn.is_empty():
		return out
	var fit := _stack_fit(held, tn.dir)
	if fit.is_empty():
		return out
	var base_rot: Array = fit.rot
	if spin != 0.0:
		base_rot = DMath.q_normalized(DMath.q_mul(DMath.q_axis_angle(tn.dir, spin), base_rot))
	for t in counterparts(target):
		if node_occupied(t, target_node):
			continue
		var w := node_world(t, target_node)
		# same orientation relative to each counterpart as to the target
		var rel := DMath.q_mul(parts[t].rot, DMath.q_conj(parts[target].rot))
		var rot := DMath.q_normalized(DMath.q_mul(rel, base_rot))
		var pos := DMath.sub(w.pos, DMath.q_rotate(rot, fit.node_pos))
		out.append({"parent": t, "node": fit.node, "parent_node": target_node, "pos": pos, "rot": rot})
	return out


# ---- inserting into a stack ---------------------------------------------------------

## Stack joints a single held part could be slipped into: {index, node,
## child, pos} where `child` hangs from part `index` at `node`. Symmetric
## joints are left out (inserting would have to happen on every copy).
func insert_joints() -> Array:
	var out := []
	for k in range(1, parts.size()):
		var p: Dictionary = parts[k]
		if p.node == "" or p.parent_node == "" or p.sym != 0 or counterparts(int(p.parent)).size() > 1:
			continue
		var w := node_world(int(p.parent), p.parent_node)
		if not w.is_empty():
			out.append({"index": int(p.parent), "node": p.parent_node, "child": k, "pos": w.pos})
	return out


## Where `held` would sit if inserted at the joint (target, node): only a
## single part with a node on each end can go between two others.
func insert_placements(held: Dictionary, target: int, target_node: String, spin := 0.0) -> Array:
	if held.parts.size() != 1 or target < 0 or target >= parts.size():
		return []
	if Sim.part(held.parts[0].part).get("nodes", []).size() < 2:
		return []
	var tn := node_world(target, target_node)
	if tn.is_empty():
		return []
	var fit := _stack_fit(held, tn.dir)
	if fit.is_empty():
		return []
	var rot: Array = fit.rot
	if spin != 0.0:
		rot = DMath.q_normalized(DMath.q_mul(DMath.q_axis_angle(tn.dir, spin), rot))
	return [{"parent": target, "node": fit.node, "parent_node": target_node, "pos": DMath.sub(tn.pos, DMath.q_rotate(rot, fit.node_pos)), "rot": rot}]


## Slips `held` into the stack at (target, node): whatever hung there is
## re-hung from the new part's far end, moved along to make room. One undo
## step. Returns the new part's index, or [] if it does not fit.
func insert_stack(held: Dictionary, target: int, target_node: String, spin := 0.0) -> Array[int]:
	var child := -1
	for k in children_of(target):
		if parts[k].parent_node == target_node:
			child = k
	if child < 0:
		return attach_stack(held, target, target_node, spin)
	var pl := insert_placements(held, target, target_node, spin)
	if pl.is_empty():
		return []
	var dir: Array = node_world(target, target_node).dir
	checkpoint()
	var keep := _snapshot()
	var tail := _extract(child)
	_remove_indices(subtree(child))
	var ni := _append_held(held, target, pl[0].node, target_node, pl[0].pos, pl[0].rot, 0)
	# the tail hangs from the new part's node that faces the way the joint did
	var far := ""
	var best := -INF
	for n in Sim.part(parts[ni].part).get("nodes", []):
		if n.name == parts[ni].node:
			continue
		var d := DMath.dot(node_world(ni, n.name).dir, dir)
		if d > best:
			best = d
			far = n.name
	var tp := stack_placements(tail, ni, far) if far != "" else []
	if tp.is_empty():
		_restore(keep)
		_undo.pop_back()
		return []
	_append_held(tail, ni, tp[0].node, far, tp[0].pos, tp[0].rot, 0)
	_after_structure_change()
	var out: Array[int] = [ni]
	return out


## Stack-attaches `held` to part `target` at `target_node`; symmetric copies
## go on every counterpart of the target. Returns the new roots' indices.
func attach_stack(held: Dictionary, target: int, target_node: String, spin := 0.0) -> Array[int]:
	return _commit(held, stack_placements(held, target, target_node, spin))


## Surface point on part `target` at local angle (rad, about its Y axis,
## 0 = local +Z) and local height y: {pos, normal, axis} in vessel space.
func surface_frame(target: int, angle: float, y: float) -> Dictionary:
	var p: Dictionary = parts[target]
	var r := radius_at(p.part, y)
	var n_local := [sin(angle), 0.0, cos(angle)]
	var local := [r * n_local[0], y, r * n_local[2]]
	return {
		"pos": DMath.add(p.pos, DMath.q_rotate(p.rot, local)),
		"normal": DMath.q_rotate(p.rot, n_local),
		"axis": DMath.q_rotate(p.rot, UP),
	}


## Placements for `count` symmetric surface copies around `target`, and the
## same again on each of the target's symmetric counterparts.
## `extra` is a local rotation applied after aligning +Z with the surface
## normal (twist/tilt from the player).
func surface_placements(held: Dictionary, target: int, angle: float, y: float, count := 1, extra: Array = []) -> Array:
	var out := []
	if target < 0 or target >= parts.size():
		return out
	var root_part: String = held.parts[0].part
	if not Sim.part(root_part).get("surface_attach", false):
		return out
	count = maxi(1, count)
	var off := surface_offset(root_part)
	# on a part with symmetric copies, every copy gets the same attachment
	var on: Array = []
	for t in counterparts(target):
		for k in count:
			on.append([t, angle + TAU * k / count])
	for tk in on:
		var f := surface_frame(tk[0], tk[1], y)
		var z: Array = f.normal
		var yax := DMath.normalized(DMath.sub(f.axis, DMath.scale(z, DMath.dot(f.axis, z))))
		var x := DMath.cross(yax, z)
		var rot := DMath.q_from_basis(x, yax, z)
		if not extra.is_empty():
			rot = DMath.q_normalized(DMath.q_mul(rot, extra))
		var pos := DMath.add(f.pos, DMath.scale(z, off))
		out.append({"parent": tk[0], "node": "", "parent_node": "", "pos": pos, "rot": rot})
	return out


## Surface-attaches `count` symmetric copies of `held` around `target`.
## `twist` rotates each copy about its outward normal (radians).
func attach_surface(held: Dictionary, target: int, angle: float, y: float, count := 1, twist := 0.0) -> Array[int]:
	var extra := [] if twist == 0.0 else DMath.q_axis_angle([0.0, 0.0, 1.0], twist)
	return _commit(held, surface_placements(held, target, angle, y, count, extra))


## Appends one copy of `held` per placement, as one undo step.
func _commit(held: Dictionary, placements: Array) -> Array[int]:
	var out: Array[int] = []
	if placements.is_empty():
		return out
	checkpoint()
	var sym := _next_sym() if placements.size() > 1 else 0
	for pl in placements:
		out.append(_append_held(held, pl.parent, pl.node, pl.parent_node, pl.pos, pl.rot, sym))
	_after_structure_change()
	return out


# ---- adjustments -----------------------------------------------------------------

## Rotates part i (and its subtree, and each counterpart in its own frame)
## by `angle` about the part's local axis.
func rotate_part(i: int, local_axis: Array, angle: float) -> void:
	if i <= 0 or i >= parts.size():
		return
	checkpoint()
	for c in counterparts(i):
		var pivot: Array = parts[c].pos
		var axis := DMath.q_rotate(parts[c].rot, local_axis)
		var q := DMath.q_axis_angle(axis, angle)
		for k in subtree(c):
			parts[k].pos = DMath.add(pivot, DMath.q_rotate(q, DMath.sub(parts[k].pos, pivot)))
			parts[k].rot = DMath.q_normalized(DMath.q_mul(q, parts[k].rot))
	changed.emit()


## Moves part i's subtree by `delta` expressed in part i's local frame;
## counterparts move the same amount in their own frames.
func offset_part(i: int, local_delta: Array) -> void:
	if i <= 0 or i >= parts.size():
		return
	checkpoint()
	for c in counterparts(i):
		var d := DMath.q_rotate(parts[c].rot, local_delta)
		for k in subtree(c):
			parts[k].pos = DMath.add(parts[k].pos, d)
	changed.emit()


func set_name_text(n: String) -> void:
	craft_name = n
	changed.emit()


# ---- staging ----------------------------------------------------------------------

func activatable(i: int) -> bool:
	var d := Sim.part(parts[i].part)
	return d.has("engine") or d.has("decoupler")  # parachutes are deployed by hand (P)


## Parts that firing decoupler i would detach (mirrors trs_core's rule).
func detach_set(i: int) -> Array[int]:
	var out: Array[int] = []
	var dec: Dictionary = Sim.part(parts[i].part).get("decoupler", {})
	if dec.is_empty():
		return out
	if dec.kind == "radial":
		return subtree(i) if parts[i].parent >= 0 else out
	if parts[i].parent >= 0 and parts[i].node == "top":
		return subtree(i)
	for k in children_of(i):
		if parts[k].parent_node == "top":
			return subtree(k)
	return out


## Automatic staging: the deepest stack segment lights first, radial
## boosters drop next, then each stack decoupler gets a press of its own
## and the next segment's engines light on the press after. Parachutes are not staged: the pilot deploys
## them (P) when it is safe.
func auto_stage() -> void:
	var n := parts.size()
	var sd := []  # stack-decoupler depth per part
	sd.resize(n)
	sd.fill(0)
	var stack_decs := []
	var radial_decs := []
	for i in n:
		var dec: Dictionary = Sim.part(parts[i].part).get("decoupler", {})
		if dec.is_empty():
			continue
		var s := detach_set(i)
		if dec.kind == "radial":
			radial_decs.append(i)
		else:
			stack_decs.append(i)
			for k in s:
				sd[k] += 1
	var depth := 0
	for i in n:
		depth = maxi(depth, sd[i])
	var steps := []  # firing order: arrays of part indices
	for d in range(depth, -1, -1):
		# dropping the spent stage and lighting the next are separate presses
		var drop := []
		for i in stack_decs:
			var s := detach_set(i)
			if not s.is_empty() and _min_depth(s, sd) == d + 1:
				drop.append(i)
		if not drop.is_empty():
			steps.append(drop)
		var light := []
		for i in n:
			if Sim.part(parts[i].part).has("engine") and sd[i] == d:
				light.append(i)
		if not light.is_empty():
			steps.append(light)
		var radial := []
		for i in radial_decs:
			if parts[i].parent >= 0 and sd[parts[i].parent] == d:
				radial.append(i)
		if not radial.is_empty():
			steps.append(radial)
	for p in parts:
		p.stage = -1
	for k in steps.size():
		for i in steps[k]:
			parts[i].stage = steps.size() - 1 - k
	changed.emit()


func _min_depth(ids: Array, sd: Array) -> int:
	var m := 1 << 30
	for k in ids:
		m = mini(m, sd[k])
	return m


func stage_count() -> int:
	var m := -1
	for p in parts:
		m = maxi(m, p.stage)
	return m + 1


## Moves part i (and its symmetry counterparts) to stage s. A stage number
## past the end inserts a new first-to-fire stage.
func set_stage(i: int, s: int) -> void:
	checkpoint()
	manual_staging = true
	for c in counterparts(i):
		parts[c].stage = s
	_compact_stages()
	changed.emit()


## Inserts an empty stage at s (shifting s and above up) and moves part i in.
func insert_stage(i: int, s: int) -> void:
	checkpoint()
	manual_staging = true
	for p in parts:
		if p.stage >= s:
			p.stage += 1
	for c in counterparts(i):
		parts[c].stage = s
	_compact_stages()
	changed.emit()


## Moves every part of stage s into a new stage at g (the gap semantics of
## insert_stage: the new stage takes number g, g and above shift up).
func move_stage(s: int, g: int) -> void:
	if g == s or g == s + 1:
		return  # the gaps either side of a stage leave it where it is
	checkpoint()
	manual_staging = true
	var moving := []
	for i in parts.size():
		if parts[i].stage == s:
			moving.append(i)
	for i in parts.size():
		if parts[i].stage >= g and not moving.has(i):
			parts[i].stage += 1
	for i in moving:
		parts[i].stage = g
	_compact_stages()
	changed.emit()


## Fires stage s together with stage t (all of s joins t).
func merge_stage(s: int, t: int) -> void:
	if s == t:
		return
	checkpoint()
	manual_staging = true
	for p in parts:
		if p.stage == s:
			p.stage = t
	_compact_stages()
	changed.emit()


## Engine thrust limit (fraction of full, 0.05..1) for part i and its
## symmetric counterparts. Returns the value set.
func set_limit(i: int, limit: float) -> float:
	if i < 0 or i >= parts.size() or not Sim.part(parts[i].part).has("engine"):
		return 1.0
	limit = clampf(roundf(limit * 20.0) / 20.0, 0.05, 1.0)
	checkpoint()
	for c in counterparts(i):
		if limit == 1.0:
			parts[c].erase("limit")
		else:
			parts[c]["limit"] = limit
	changed.emit()
	return limit


func _compact_stages() -> void:
	var used := {}
	for p in parts:
		if p.stage >= 0:
			used[p.stage] = true
	var keys := used.keys()
	keys.sort()
	var remap := {}
	for k in keys.size():
		remap[keys[k]] = k
	for p in parts:
		if p.stage >= 0:
			p.stage = remap[p.stage]


func reset_staging() -> void:
	checkpoint()
	manual_staging = false
	auto_stage()


func _after_structure_change() -> void:
	if not manual_staging:
		auto_stage()
	else:
		# new activatable parts still need a stage
		for i in parts.size():
			if activatable(i) and parts[i].stage < 0:
				parts[i].stage = 0
		_compact_stages()
	changed.emit()
