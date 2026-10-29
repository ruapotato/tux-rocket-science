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
## --test-guard: the lasers and the wild in the flight scene. A Warden
## Lander set down by a pack of dune stalkers on Saffron, a penguin out for
## a walk, and guard.py (the Script Editor's template) running on the
## ship's computer: the lasers see the stalkers off before they finish the
## penguin, as the script says, and the batteries pay for it. Then by hand:
## the left mouse button held on an animal fires the turrets at it (hp off
## it at the turrets' rate, nothing once let go); Shift + click locks on.
## There is no key for guarding: that is the script's. On Floe the shelf penguins come over to
## ours and guard.py never fires on them; E catalogues one.
extends RefCounted

const Templates := preload("res://scripts/os/templates.gd")
const WildShot := preload("res://scripts/gates/dev/wild_shot.gd")

var _log := PackedStringArray()
var _failed := 0
var _gate := preload("res://scripts/gates/test_by_hand.gd").new()


func _check(ok: bool, msg: String) -> void:
	_log.append(("ok   " if ok else "FAIL ") + msg)
	if not ok:
		_failed += 1


## The lander set down near one of `species` on body (45 m south of it),
## standing on its legs, the pad's rocket gone; its id.
func _set_down(scene: Node3D, body: String, species: String, seed := 1) -> int:
	var s: FlightSession = scene.session
	var core := s.core
	var bi := core.body_find(body)
	var R: float = JSON.parse_string(core.body_info(bi)).radius
	var at: PackedFloat64Array = core.find_life(bi, species, seed)
	var text := WildShot.lander_only(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "warden_lander.craft.json"))
	var pad := s.active
	var id := core.flight_place_landed(text, body, at[0] - rad_to_deg(45.0 / R), at[1])
	s.switch_to(id)
	core.vessel_remove(pad)
	core.vessel_set_legs(id, true)
	s.step_ticks(150)
	return id


## Steps the flight a tick at a time for `secs` with the computer running;
## what the scripts said.
func _run(scene: Node3D, secs: float, each := Callable()) -> Array:
	var s: FlightSession = scene.session
	var comp: ShipComputer = scene.computer
	var said := []
	var seen := comp.log_count
	var t0 := s.time()
	while s.time() - t0 < secs:
		s.step_ticks(1)
		comp.update(scene, s.telemetry())
		if each.is_valid():
			each.call()
		var fresh := mini(comp.log_count - seen, comp.log.size())
		for k in range(comp.log.size() - fresh, comp.log.size()):
			if not str(comp.log[k]).begins_with("> "):
				said.append(str(comp.log[k]))
		seen = comp.log_count
		if not s.telemetry().get("alive", false):
			break
	return said


## Every script stopped, and gone from ps (a kill lands when the process
## next runs: the gates after this one share the computer).
func _stop(comp: ShipComputer) -> void:
	comp.stop_scripts()
	for k in 60:
		var left: Array = _gate._scripts(comp)
		if left.is_empty():
			return
		for pid in left:
			comp.sh("kill " + str(pid))
		comp.machine.tick(1)
	_log.append("     (some scripts would not stop: %s)" % str(_gate._scripts(comp)))


func _hostiles(core: TrsSim, from: int, reach: float) -> int:
	var n := 0
	for th in JSON.parse_string(core.things_near(from, reach)):
		n += 1 if th.get("hostile", false) and str(th.what) == "animal" else 0
	return n


func _ec(core: TrsSim, id: int) -> float:
	return float(JSON.parse_string(core.telemetry(id)).get("ec", 0.0))


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false

	# 1. guard.py keeps the stalkers off a penguin out for a walk
	var scene: Node3D = await _gate._scene(tree, "warden_lander", "pad")
	var s: FlightSession = scene.session
	var core := s.core
	scene.computer = ShipComputer.get_shared()
	var comp: ShipComputer = scene.computer
	for pid in _gate._scripts(comp):
		comp.sh("kill " + str(pid))
	var ship := _set_down(scene, "saffron", "dune_stalkers")
	comp.update(scene, s.telemetry())
	comp.machine.write_file("/root/scripts/guard.py", Templates.GUARD)
	var why := str(comp.machine.start("/root/scripts/guard.py"))
	var pen := core.vessel_eva_exit(ship, 0)
	core.vessel_rename(pen, "Pip")
	s.switch_to(pen)
	var ps := core.vessel_serial(pen)
	var ec0 := _ec(core, ship)
	var near0 := _hostiles(core, pen, 120.0)
	var said := _run(scene, 40.0)
	var alive := s.still(pen, ps)
	var health := float(JSON.parse_string(core.telemetry(pen)).get("health", 0.0)) if alive else 0.0
	var near1 := _hostiles(core, ship, 300.0)
	var zapped := 0
	var first := ""
	for l in said:
		if str(l).begins_with("Zapping a Dune stalker"):
			zapped += 1
			first = str(l) if first == "" else first
	var used := ec0 - _ec(core, ship)
	_check(why == "" and near0 > 0 and near1 == 0 and alive and health > 40.0 and zapped > 0 and used > 0.0,
		("guard.py on the Warden Lander, a penguin out among %d dune stalkers on Saffron: 40 s on, none left within 300 m of the ship, "
		+ "the penguin lives (%.0f%% health); the script called %d of them (\"%s\"); the batteries paid %.1f kJ") % [near0, health, zapped,
		first, used])
	_stop(comp)
	core.vessel_remove(pen)

	# 2. by hand: the left mouse button held on an animal fires at it (by
	# a herd of dune skimmers: the stalkers there are all gone)
	s.switch_to(ship)
	ship = _set_down(scene, "saffron", "dune_skimmers", 3)
	core.set_guard(ship, 0)
	core.lock_on(ship, 0, 0, 0)
	var lander_parts: Array = JSON.parse_string(core.vessel_part_states(ship))
	var dps := 0.0
	for p in lander_parts:
		if p.has("laser_dps"):
			dps = float(p.laser_dps)
	var target := {}
	for th in JSON.parse_string(core.things_near(ship, 400.0)):
		if str(th.what) == "animal" and float(th.distance) > 15.0:
			target = th
			break
	var hp0 := float(target.get("health", 0.0))
	var n := {"beams": 0, "after": 0, "fired": 0, "target": target, "on_penguin": 0, "locks": 0}  # (a lambda's own copies of plain locals would never come back)
	var spot := func() -> Vector2:
		var tg: Dictionary = n.target
		var info: Dictionary = JSON.parse_string(core.thing_info(ship, int(tg.kind), int(tg.id), int(tg.serial)))
		return scene._cam.unproject_position(Frame.vec_arr(info.rel)) if info.has("rel") else Vector2(-1, -1)
	scene._render(1.0 / 60.0)
	var press := InputEventMouseButton.new()
	press.button_index = MOUSE_BUTTON_LEFT
	press.pressed = true
	press.position = spot.call()
	Input.parse_input_event(press)
	scene._unhandled_input(press)
	var held := func() -> void:
		scene._render(1.0 / 60.0)
		var mm := InputEventMouseMotion.new()
		mm.position = spot.call()
		scene._unhandled_input(mm)
		scene.combat.step(1.0 / 60.0)
		for p in JSON.parse_string(core.vessel_part_states(ship)):
			n.beams += 1 if p.get("firing", false) else 0
	_run(scene, 3.0, held)
	var info: Dictionary = JSON.parse_string(core.thing_info(ship, int(target.kind), int(target.id), int(target.serial)))
	var hp1 := float(info.get("health", 0.0))
	var release := press.duplicate()
	release.pressed = false
	Input.parse_input_event(release)
	scene._unhandled_input(release)
	scene.combat.step(1.0 / 60.0)
	_run(scene, 1.0, func() -> void:
		for p in JSON.parse_string(core.vessel_part_states(ship)):
			n.after += 1 if p.get("firing", false) else 0)
	var beams: int = n.beams
	var after: int = n.after
	var kind: Dictionary = Sim.life_kind("saffron", str(target.get("species", "")))
	var lost := (hp0 - hp1) / 100.0 * float(kind.get("hp", 1.0))
	# (a beam tick may land on it or on what's in the way: no more than the beams' worth)
	_check(not target.is_empty() and beams > 0 and lost > 0.0 and lost <= beams * dps / 50.0 + 1e-6 and after == 0,
		("by hand: the left button held on a %s %.0f m off for 3 s fires the turrets (%d beam ticks, up to %.0f hp); it lost %.1f hp; "
		+ "let go, they stop (%d ticks after)") % [str(target.get("species", "?")), float(target.get("distance", 0.0)), beams,
		beams * dps / 50.0, lost, after])

	# 3. Shift + click locks on
	var other := {}
	for th in JSON.parse_string(core.things_near(ship, 600.0)):
		if str(th.what) == "animal":
			other = th
			break
	var locked := false
	if not other.is_empty():
		n.target = other
		scene._render(1.0 / 60.0)
		scene.combat.lock_at(spot.call())
		var lk = JSON.parse_string(core.telemetry(ship)).get("lock", [])
		locked = lk is Array and lk.size() == 3 and int(lk[1]) == int(other.id)
	_check(locked, "Shift + click on a %s locks the lasers on it (%s)" % [str(other.get("species", "?")), locked])
	core.lock_on(ship, 0, 0, 0)

	# 4. on Floe the shelf penguins come over; guard.py leaves them be; E catalogues one
	scene.queue_free()
	await tree.process_frame
	scene = await _gate._scene(tree, "warden_lander", "pad")
	s = scene.session
	core = s.core
	scene.computer = ShipComputer.get_shared()
	comp = scene.computer
	ship = _set_down(scene, "floe", "shelf_penguins")
	comp.update(scene, s.telemetry())
	comp.machine.write_file("/root/scripts/guard.py", Templates.GUARD)
	comp.machine.start("/root/scripts/guard.py")
	pen = core.vessel_eva_exit(ship, 0)
	core.vessel_rename(pen, "Pip")
	s.switch_to(pen)
	n.fired = 0
	_run(scene, 40.0, func() -> void:
		for p in JSON.parse_string(core.vessel_part_states(ship)):
			n.fired += 1 if p.get("firing", false) else 0
		# what it's locked on: never one of the penguins
		var lk = JSON.parse_string(core.telemetry(ship)).get("lock", [])
		if lk is Array and lk.size() == 3:
			n.locks += 1
			var on: Dictionary = JSON.parse_string(core.thing_info(ship, int(lk[0]), int(lk[1]), int(lk[2])))
			n.on_penguin += 1 if on.get("penguin", false) else 0)
	var fired: int = n.fired
	var hurt := 0
	for th in JSON.parse_string(core.things_near(ship, 600.0)):
		hurt += 1 if th.get("penguin", false) and float(th.get("health", 100.0)) < 100.0 else 0
	var close := 0
	for th in JSON.parse_string(core.things_near(pen, 4.0)):
		close += 1 if th.get("penguin", false) and str(th.what) == "animal" else 0
	var r0 := Career.research
	Career.catalogued.erase("floe/shelf_penguins")
	var e := InputEventKey.new()
	e.keycode = KEY_E
	e.pressed = true
	scene._key(e)
	var got := Career.research - r0
	_check(close > 0 and int(n.on_penguin) == 0 and hurt == 0 and "floe/shelf_penguins" in Career.catalogued and got == 2.0,
		("on Floe, 40 s out: %d shelf penguins at ours' feet; guard.py fired %d beam ticks on the rest (seals, puffins), locked on a "
		+ "penguin %d times of %d, and no penguin near is hurt (%d); E catalogued them (+%.0f research)") % [close, fired, int(n.on_penguin),
		int(n.locks), hurt, got])
	_stop(comp)
	comp.update(scene, s.telemetry())
	scene.queue_free()
	await tree.process_frame
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	Sim.save_game()
	return {"status": "FAIL" if _failed else "PASS", "log": "\n".join(_log)}
