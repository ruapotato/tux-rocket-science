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
## ours and guard.py never fires on them; E catalogues one. A drill digging
## is heard: on Saffron the stalkers come for it, time warp won't run while
## they're coming, and drill_guard.py keeps them off it while it fills the
## tanks; left unguarded, the drill is bitten off.
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
func _set_down(scene: Node3D, body: String, species: String, seed := 1, south := 45.0, text := "") -> int:
	var s: FlightSession = scene.session
	var core := s.core
	var bi := core.body_find(body)
	var R: float = JSON.parse_string(core.body_info(bi)).radius
	var at: PackedFloat64Array = core.find_life(bi, species, seed)
	if text == "":
		text = WildShot.lander_only(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "warden_lander.craft.json"))
	var pad := s.active
	# (on level ground near there: a lander tips over on a dune's face)
	var lv: PackedFloat64Array = core.level_spot(bi, at[0] - rad_to_deg(south / R), at[1], 60.0, 0.006)
	var id := core.flight_place_landed(text, body, lv[0], lv[1])
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


## The Warden Lander with a drill low on its tank (in reach of the ground
## on its legs), an ore bin and a refinery: one that lives off the land.
static func _drill_lander() -> String:
	var m := CraftModel.new()
	m.load_text(WildShot.lander_only(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "warden_lander.craft.json")))
	var tank := -1
	for i in m.parts.size():
		if str(m.parts[i].part) == "tank_m":
			tank = i
	m.attach_surface(CraftModel.held_single("drill"), tank, 0.0, -0.8, 1)
	m.attach_surface(CraftModel.held_single("ore_bin"), tank, PI / 2, 0.1, 1)
	m.attach_surface(CraftModel.held_single("refinery"), tank, PI, 0.3, 1)
	m.craft_name = "Warden Lander (drilling)"
	# (the tanks low: there is something to dig for)
	var c: Dictionary = JSON.parse_string(m.save_text())
	for p in c.parts:
		if str(p.part).begins_with("tank_"):
			p["fuel"] = 100.0
	return JSON.stringify(c)


func _part_count(core: TrsSim, id: int, part_id: String) -> int:
	var n := 0
	for p in JSON.parse_string(core.vessel_part_states(id)):
		n += 1 if str(p.part) == part_id else 0
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
	core.vessel_remove(pen)

	# 5. a drill digging on Saffron, 300 m from a pack of dune stalkers: it
	# is heard, warp won't run while they come, and drill_guard.py keeps
	# them off the drill; then the same with nobody guarding it
	var outcome := []
	for guarded in [true, false]:
		s.switch_to(ship)
		ship = _set_down(scene, "saffron", "dune_stalkers", 2, 300.0, _drill_lander())
		comp.update(scene, s.telemetry())
		var liq0 := float(s.telemetry().get("liquid", 0.0))
		if guarded:
			comp.machine.write_file("/root/scripts/drill_guard.py", Templates.DRILL)
			why = str(comp.machine.start("/root/scripts/drill_guard.py"))
		else:
			core.vessel_set_mining(ship, true)
		said = _run(scene, 3.0)
		var refused := s.set_warp("rails", 3)
		var heard := s.drill_heard(true)
		var came := {"n": 0}
		said += _run(scene, 150.0, func() -> void:
			for th in JSON.parse_string(core.things_near(ship, 300.0)):
				if str(th.what) == "animal" and th.get("hostile", false) and float(th.distance) < 60.0:
					came.n = maxi(int(came.n), 1))
		var shots := 0
		for l in said:
			shots += 1 if str(l).ends_with("firing") else 0
		outcome.append({"why": why if guarded else "", "refused": refused, "heard": heard, "came": int(came.n), "fired": shots,
			"drills": _part_count(core, ship, "drill"), "dug": float(s.telemetry().get("liquid", 0.0)) - liq0, "alive": s.telemetry().get("alive", false)})
		_stop(comp)
		core.vessel_set_mining(ship, false)
		s.set_warp("physics", 0)
	# a script asleep for an hour wakes when something happens (an alarm:
	# a drill heard), not an hour on
	comp.machine.write_file("/root/scripts/nap.py", "sleep(3600)\nprint(f\"awake after {ship.time:.0f}\")\n")
	comp.machine.start("/root/scripts/nap.py")
	var nap0 := s.time()
	var before_alarm := _run(scene, 2.0)
	comp.alarms += 1
	var after_alarm := _run(scene, 2.0)
	_stop(comp)
	var woke := after_alarm.filter(func(l): return str(l).begins_with("awake")).size() > 0
	_check(before_alarm.filter(func(l): return str(l).begins_with("awake")).is_empty() and woke,
		"a script asleep for an hour (sleep(3600)): still asleep 2 s on; an alarm, and it wakes (%s), %.0f s after it lay down" % [woke, s.time() - nap0])
	var g: Dictionary = outcome[0]
	var u: Dictionary = outcome[1]
	_check(g.why == "" and str(g.refused).contains("the drill") and g.heard != "" and g.came == 0 and g.fired > 0 and g.drills == 1 and g.dug > 0.0
		and u.came > 0 and u.drills == 0,
		("a drill digging on Saffron 300 m from dune stalkers: warp refused (\"%s\"); drill_guard.py fired %d times, none got within 60 m, the drill "
		+ "is still on (%d) and %.1f kg of propellant made; unguarded, they reached it and the drill was bitten off (%d left)") % [g.refused, g.fired,
		g.drills, g.dug, u.drills])
	scene.queue_free()
	await tree.process_frame
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	Sim.save_game()
	return {"status": "FAIL" if _failed else "PASS", "log": "\n".join(_log)}
