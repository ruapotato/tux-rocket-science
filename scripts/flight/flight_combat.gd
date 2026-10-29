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
## The flight's lasers and the wild, as the pilot meets them: hold the left
## mouse button to fire every laser that can bear at what is under the
## cursor (a penguin on EVA fires its zapper); Shift + click locks the
## lasers on a thing, the computer tracking it and firing while it's in
## reach (Shift + click on nothing lets go). Keeping watch by itself is
## for a script to do (guard.py, or your own). On EVA, E has a
## good look at the animal nearest (it goes in the catalogue). An overlay
## draws the crosshair, the lock, the threats near, and a penguin's
## health; it says so when we're under attack.
class_name FlightCombat
extends Control

const PICK_RANGE := 3000.0
const NEAR := 300.0           # m: threats this near are marked
const LOOK_REACH := 8.0       # m: E looks at an animal this near

var scene: Node
var _firing := false
var _mouse := Vector2.ZERO
var _near: Array = []         # things_near, refreshed now and then
var _near_t := 0.0
var _lock := {}               # thing_info of what we're locked on
var _hurt_seen := 0.0
var _alarm_t := -100.0
var _health := -1.0
var _bite_t := -100.0
## s a penguin has been on foot by its ship (within GUARDED_REACH) with the
## ship's lasers seeing to things -- a script running on its computer -- and
## no shot of the pilot's own (the Safe Ground contract)
var guarded_s := 0.0
var _guard_t := -1.0
const GUARDED_REACH := 300.0


func setup(s: Node) -> void:
	scene = s
	name = "Combat"
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE


func _core() -> TrsSim:
	return scene.session.core


func _active() -> int:
	return scene.session.active


## How many lasers the vessel flown has (a penguin's zapper is one).
func lasers() -> int:
	return int(scene.session.telemetry().get("lasers", 0))


## The camera ray through a screen point, as the core sees it: from (rel.
## the active vessel) and unit direction, sim axes.
func _ray(at: Vector2) -> Array:
	var cam: Camera3D = scene._cam
	var o := cam.project_ray_origin(at)
	var d := cam.project_ray_normal(at)
	return [PackedFloat64Array(Frame.to_sim(o)), PackedFloat64Array(Frame.to_sim(d))]


## What's under a screen point: [kind, id, serial, distance, part] and the point (rel. active, sim axes).
func pick(at: Vector2) -> Array:
	var r := _ray(at)
	var hit: PackedFloat64Array = _core().pick(_active(), r[0], r[1], PICK_RANGE)
	var p := PackedFloat64Array()
	for k in 3:
		p.append(r[0][k] + r[1][k] * hit[3])
	return [hit, p]


## Mouse: true when it was ours (a click on the world, not the panels).
func mouse(mb: InputEventMouseButton) -> bool:
	if mb.button_index != MOUSE_BUTTON_LEFT or scene._inside or lasers() == 0:
		return false
	_mouse = mb.position
	if mb.pressed and mb.shift_pressed:
		lock_at(mb.position)
		return true
	_firing = mb.pressed
	if _firing:
		guarded_s = 0.0  # (by hand: not the ship's doing)
	if not _firing:
		_core().hand_fire(_active(), _active(), false, PackedFloat64Array([0, 0, 0]))
	return true


func motion(mm: InputEventMouseMotion) -> void:
	_mouse = mm.position


## Shift + click: lock on what's under the cursor (nothing: let go).
func lock_at(at: Vector2) -> void:
	var p := pick(at)
	var hit: PackedFloat64Array = p[0]
	var kind := int(hit[0])
	if kind == 1 or kind == 2:
		_core().lock_on(_active(), kind, int(hit[1]), int(hit[2]))
		var info: Dictionary = JSON.parse_string(_core().thing_info(_active(), kind, int(hit[1]), int(hit[2])))
		scene.hud.toast("Lasers locked on %s, %s off: they fire while it's in reach (Shift + click on nothing lets go)" % [
			describe(info), FlightHUD.fmt_dist(float(info.get("distance", 0.0)))], 3.0)
		Sfx.play("click")
	else:
		_core().lock_on(_active(), 0, 0, 0)
		scene.hud.toast("Lasers: lock let go", 1.5)


## A thing's name as a pilot would say it.
func describe(info: Dictionary) -> String:
	if info.is_empty():
		return "nothing"
	if str(info.get("what", "")) == "animal":
		var body := str(scene.session.telemetry().get("body", ""))
		var k: Dictionary = Sim.life_kind(body, str(info.get("species", "")))
		return "a " + str(k.get("name", info.species)).to_lower().trim_suffix("s") if not k.is_empty() else str(info.species)
	return str(info.get("name", "a ship"))


## E on EVA: a good look at the animal nearest. True if there was one.
func look() -> bool:
	var near: Array = JSON.parse_string(_core().things_near(_active(), LOOK_REACH))
	for th in near:
		if str(th.get("what", "")) != "animal":
			continue
		var body := str(scene.session.telemetry().get("body", ""))
		var k: Dictionary = Sim.life_kind(body, str(th.species))
		var nm := str(k.get("name", th.species))
		var r := Career.catalogue(body, str(th.species))
		var mood: String = {"following": " It follows you about.", "fleeing": " It's off before you get a good look.",
			"charging": " It wants you for lunch!", "down": " It's not moving."}.get(str(th.state), "")
		var got := "  New to the catalogue: +%.0f research (%d of %d kinds on %s)" % [r, Career.catalogued_on(body).size(),
			_fauna(body), Career.body_name(body)] if r > 0.0 else ""
		scene.hud.toast("%s: %s%s%s" % [nm, str(k.get("about", "")), mood, got], 7.0)
		Sfx.play("chime" if r > 0.0 else "click")
		return true
	return false


func _fauna(body: String) -> int:
	var n := 0
	for k in Sim.life(body):
		n += 1 if k.get("animal", false) else 0
	return n


## Every frame: the hand's aim, what is near, the overlay, the alarms.
func step(delta: float) -> void:
	var t: Dictionary = scene.session.telemetry()
	if not t.get("alive", false):
		_firing = false
		queue_redraw()
		return
	if _firing and lasers() > 0:
		# (let go over a panel, the release never comes here: ask the mouse)
		if not Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT) and DisplayServer.get_name() != "headless":
			_firing = false
			_core().hand_fire(_active(), _active(), false, PackedFloat64Array([0, 0, 0]))
		else:
			_core().hand_fire(_active(), _active(), true, pick(_mouse)[1])
	_near_t -= delta
	if _near_t <= 0.0:
		_near_t = 0.25
		_near = JSON.parse_string(_core().things_near(_active(), NEAR))
	_lock = {}
	var lk = t.get("lock", null)
	if lk is Array and lk.size() == 3:
		_lock = JSON.parse_string(_core().thing_info(_active(), int(lk[0]), int(lk[1]), int(lk[2])))
	# taking hits: say so (a penguin's own health, a ship's worst part)
	var hurt := float(t.get("hurt", 0.0))
	if hurt > _hurt_seen + 0.02:
		if scene._shake < 0.3:
			scene._shake = 0.3
		if t.get("eva", false):
			if scene.session.time() - _bite_t > 0.8:
				Sfx.play("ouch")
				_bite_t = scene.session.time()
		if scene.session.time() - _alarm_t > 6.0:
			_alarm_t = scene.session.time()
			Sfx.play("alarm")
			var who := ""
			for th in _near:
				if th.get("after_us", false):
					who = describe(th)
					break
			scene.hud.toast(("%s is hurt%s! %.0f%% left" % [t.get("name", "The penguin"), " by " + who if who != "" else "", t.get("health", 0.0)])
				if t.get("eva", false) else ("Under attack%s! The worst-hit part has %.0f%% left%s" % [" by " + who if who != "" else "",
				100.0 * (1.0 - hurt), ""]), 3.5)
	_hurt_seen = hurt
	_health = float(t.get("health", -1.0)) if t.get("eva", false) else -1.0
	_guarded(t)
	queue_redraw()


func _guarded(t: Dictionary) -> void:
	var now: float = scene.session.time()
	var comp: ShipComputer = scene.computer
	var ok: bool = t.get("eva", false) and t.get("landed", false) and comp != null and comp.gun >= 0 and comp.gun != _active()
	if ok:
		var gt: Dictionary = JSON.parse_string(_core().telemetry(comp.gun))
		var running := not (comp.panel().get("scripts", []) as Array).is_empty()
		var d: PackedFloat64Array = _core().vessel_render(comp.gun, _active())
		ok = gt.get("alive", false) and running and d.size() >= 4 \
			and Vector3(d[1], d[2], d[3]).length() <= GUARDED_REACH
	if ok and _guard_t >= 0.0:
		guarded_s += now - _guard_t
	elif not ok:
		guarded_s = 0.0
	_guard_t = now if ok else -1.0


func _draw() -> void:
	if scene == null or scene._inside or scene._desk_open:
		return
	var t: Dictionary = scene.session.telemetry()
	if not t.get("alive", false):
		return
	var cam: Camera3D = scene._cam
	var red := Color(1.0, 0.3, 0.2)
	var amber := Color(1.0, 0.75, 0.2)
	# what is near: hostile ones marked red, ours not at all
	for th in _near:
		if th.get("penguin", false) or not (th.get("hostile", false) or th.get("threat", false)):
			continue
		var p: Vector3 = Frame.vec_arr(th.rel)
		if cam.is_position_behind(p):
			continue
		var sp := cam.unproject_position(p)
		var c := red if th.get("after_us", false) else amber
		var s := 7.0
		draw_line(sp + Vector2(-s, -s - 6), sp + Vector2(0, -6), c, 2.0)
		draw_line(sp + Vector2(s, -s - 6), sp + Vector2(0, -6), c, 2.0)
	# the lock: a bracket round it, its name and health
	if not _lock.is_empty() and _lock.has("rel"):
		var p: Vector3 = Frame.vec_arr(_lock.rel)
		if not cam.is_position_behind(p):
			var sp := cam.unproject_position(p)
			var r := clampf(float(_lock.get("radius", 1.0)) * 600.0 / maxf(float(_lock.get("distance", 1.0)), 1.0), 12.0, 80.0)
			for k in 4:
				var q := Vector2(1 if k % 2 == 0 else -1, 1 if k < 2 else -1)
				draw_line(sp + q * r, sp + q * r - Vector2(q.x * r * 0.4, 0), red, 2.0)
				draw_line(sp + q * r, sp + q * r - Vector2(0, q.y * r * 0.4), red, 2.0)
			var f := get_theme_default_font()
			draw_string(f, sp + Vector2(r + 6, -4), "%s  %s" % [describe(_lock), FlightHUD.fmt_dist(float(_lock.get("distance", 0.0)))],
				HORIZONTAL_ALIGNMENT_LEFT, -1, 14, red)
			var hw := 60.0
			draw_rect(Rect2(sp + Vector2(r + 6, 2), Vector2(hw, 5)), Color(0, 0, 0, 0.5))
			draw_rect(Rect2(sp + Vector2(r + 6, 2), Vector2(hw * clampf(float(_lock.get("health", 0.0)) / 100.0, 0.0, 1.0), 5)), red)
	# the crosshair, where a click fires
	if lasers() > 0:
		var c := Color(1.0, 0.45, 0.3) if _firing else Color(1, 1, 1, 0.75)
		var m := _mouse
		for v in [Vector2(1, 0), Vector2(-1, 0), Vector2(0, 1), Vector2(0, -1)]:
			draw_line(m + v * 5.0, m + v * 13.0, c, 2.0)
		draw_arc(m, 3.0, 0, TAU, 12, c, 1.5)
	# a penguin's health
	if _health >= 0.0 and _health < 100.0:
		var w := 220.0
		var at := Vector2(size.x * 0.5 - w * 0.5, 96)
		draw_rect(Rect2(at, Vector2(w, 9)), Color(0, 0, 0, 0.55))
		var col := Color(0.3, 0.9, 0.4).lerp(red, clampf(1.0 - _health / 100.0, 0.0, 1.0))
		draw_rect(Rect2(at, Vector2(w * _health / 100.0, 9)), col)
		draw_string(get_theme_default_font(), at + Vector2(0, -4), "%s  %.0f%%" % [t.get("name", "Penguin"), _health], HORIZONTAL_ALIGNMENT_LEFT, -1, 13, col)
