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
## First flight: walks a new player through a Hopper hop, one step at a
## time. Each step is done when the flight itself shows it happened (the
## throttle is up, the engine is lit, the chute is out...), never on a timer.
class_name FirstFlight
extends PanelContainer

const STEPS := [
	["Full throttle", "This is the Hopper: one engine, one tank, one parachute. Press [b]Z[/b] for full throttle (the THR bar fills)."],
	["Launch", "Press [b]SPACE[/b]. The engine lights and the launch clamps let go."],
	["Climb", "Going up! SAS is holding the nose straight up. Just watch: the engine burns until its tank is dry."],
	["Drop the tank", "Engine out. Press [b]SPACE[/b] again to drop the empty tank: less to carry down."],
	["Coast", "Still climbing on momentum. Press [b]M[/b] to see your path on the map, and [b]M[/b] again to come back."],
	["Parachute", "Falling now. Press [b]P[/b]: the parachute waits until the air is thick enough and you are slow enough, then opens."],
	["Ride it down", "Canopy out. Ride it down; the penguin will let you know how it went."],
	["Touchdown", "You flew your first rocket! [b]Recover vessel[/b] brings the crew home. Next: build your own in the [b]Assembly Hangar[/b], or try [b]Quick launch: Orbiter[/b] and go around Floe."],
]

var step := 0
var failed := false
var _scene: Node
var _title: Label
var _body: RichTextLabel
var _map_seen := false


func setup(scene: Node) -> void:
	_scene = scene
	add_theme_stylebox_override("panel", HangarTheme.box(Color(0.06, 0.1, 0.18, 0.92), 8, PartVisual.ORANGE, 2))
	set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
	offset_left = -380
	offset_right = -10
	offset_top = 112
	custom_minimum_size = Vector2(370, 0)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	var v := VBoxContainer.new()
	v.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(v)
	_title = Label.new()
	_title.add_theme_color_override("font_color", PartVisual.ORANGE)
	_title.add_theme_font_size_override("font_size", 17)
	v.add_child(_title)
	_body = RichTextLabel.new()
	_body.bbcode_enabled = true
	_body.fit_content = true
	_body.scroll_active = false
	_body.custom_minimum_size = Vector2(350, 0)
	_body.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_body.add_theme_font_size_override("normal_font_size", 15)
	_body.add_theme_font_size_override("bold_font_size", 15)
	v.add_child(_body)
	_show()


func _process(_dt: float) -> void:
	update()


## Advances through every step the flight has already completed.
func update() -> void:
	if failed or step >= STEPS.size() - 1 or _scene == null:
		return
	var t: Dictionary = _scene.session.telemetry()
	if _scene._map.visible:
		_map_seen = true
	if _scene._launched and (not t.get("alive", false) or int(t.get("crew_capacity", 0)) == 0):
		failed = true
		_title.text = "That didn't go to plan"
		_body.text = "Press [b]Esc[/b] and choose [b]Revert to launch[/b] to try again."
		return
	var before := step
	while step < STEPS.size() - 1 and _done(step, t):
		step += 1
	if step != before:
		Sfx.play("click")
		_show()


func _done(s: int, t: Dictionary) -> bool:
	match s:
		0:
			return _scene._throttle >= 0.99
		1:
			return _scene._launched and t.get("thrust", 0.0) > 0.0
		2:
			return int(t.get("engines_lit", 0)) == 0
		3:
			return int(t.get("next_stage", 0)) < 0
		4:
			return _map_seen or t.get("vertical_speed", 1.0) < 0.0
		5:
			return int(t.get("chutes", 0)) > 0 and int(t.get("chutes_stowed", 0)) == 0
		6:
			return t.get("landed", false)
	return false


func _show() -> void:
	_title.text = "FIRST FLIGHT  %d/%d · %s" % [step + 1, STEPS.size(), STEPS[step][0]]
	_body.text = STEPS[step][1]
