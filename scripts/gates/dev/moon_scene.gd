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
## Dev: the Moon mission autopilot flown inside the real flight scene. Its
## UI (HUD, navball, map, vessel views, reentry, audio) updates as the
## mission goes, through SOI changes, landings and staging, with the map
## flipped open now and then. Every update checks the HUD's numbers are
## possible. Any script error fails it.
extends RefCounted

var _log := PackedStringArray()
var _bad := 0
var _n := 0


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	Sim.tutorial = false
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "krill_lander.craft.json")
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	var api := ControlAPI.new(scene.session)
	var m := MoonMission.new(api)
	m.on_step = func():
		_n += 1
		if _n % 7 != 0:
			return
		scene._map.visible = (_n / 700) % 3 == 1
		scene._render(1.0 / 60.0)
		_check(scene)
	var ok := m.fly()
	_log.append("mission %s, %d UI updates, flaws %d" % ["flown" if ok and m.landed_home else "FAILED", _n / 7, _bad])
	_log.append("\n".join(m.log.slice(maxi(0, m.log.size() - 4))))
	return {"status": "PASS" if _bad == 0 and ok else "FAIL", "log": "\n".join(_log)}


func _check(scene: Node) -> void:
	var t: Dictionary = scene.session.telemetry()
	if not t.get("alive", false):
		return
	var txt: String = scene.hud._left.text + scene.hud._right.text + scene.hud._stages.text
	for bad in ["nan", "inf", "-0.0 m/s", "null"]:
		if txt.to_lower().contains(bad) and _bad < 5:
			_log.append("FLAW: HUD shows '%s' at %s over %s:\n%s" % [bad, t.situation, t.body, txt])
			_bad += 1
