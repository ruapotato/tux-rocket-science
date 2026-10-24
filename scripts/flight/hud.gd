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
## Flight HUD. Every number on it is read from trs_core telemetry.
class_name FlightHUD
extends Control

signal sas_pressed(mode: String)
signal menu_action(action: String)
signal warp_pressed(dir: int)       # +1 faster, -1 slower, 0 stop
signal throttle_set(value: float)   # dragged on the throttle gauge
signal action_pressed(name: String) # "stage", "chute", "legs"

const SAS_BUTTONS := [
	["stability", "SAS"], ["prograde", "PRO"], ["retrograde", "RET"], ["normal", "NRM"],
	["antinormal", "ANT"], ["radial_out", "RAD+"], ["radial_in", "RAD-"], ["maneuver", "MNV"],
]

var navball: Navball
var speed_mode := "surface"  # or "orbit"
var maneuver_active := false
var _clock: Label
var _status: Label
var _left: RichTextLabel
var _right: RichTextLabel
var _stages: RichTextLabel
var _fuel: Control
var _throttle: Control
var _gmeter: Control
var _heat: Control
var _sas_buttons := {}
var _toast: Label
var _toast_tween: Tween
var _menu: PanelContainer
var _t: Dictionary = {}
var _warp_text := ""
var portrait_slot: Control
var _node_label: Label
var _warp_label: Label
var _action_buttons := {}
var _hint: Label
var _fail: PanelContainer
var _fail_title: Label
var _fail_body: Label


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	theme = HangarTheme.make()

	var top := PanelContainer.new()
	top.set_anchors_preset(Control.PRESET_CENTER_TOP)
	top.offset_left = -230
	top.offset_right = 230
	top.offset_top = 8
	add_child(top)
	var tv := VBoxContainer.new()
	top.add_child(tv)
	_clock = _label(tv, 22, PartVisual.ORANGE)
	_clock.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_status = _label(tv, 14, Color(0.8, 0.88, 1.0))
	_status.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	# time warp: the buttons say what they do, the label says what is on
	var wr := HBoxContainer.new()
	wr.alignment = BoxContainer.ALIGNMENT_CENTER
	wr.add_theme_constant_override("separation", 6)
	tv.add_child(wr)
	for b in [["◀◀", -1, "Slower time warp (,)"], ["■", 0, "Back to normal time (/)"], ["▶▶", 1, "Faster time warp (.)\nIn the air or under thrust only up to 4x physics warp"]]:
		var btn := Button.new()
		btn.text = b[0]
		btn.tooltip_text = b[2]
		btn.focus_mode = Control.FOCUS_NONE
		btn.custom_minimum_size = Vector2(44, 0)
		btn.add_theme_font_size_override("font_size", 13)
		var dir: int = b[1]
		btn.pressed.connect(func(): warp_pressed.emit(dir))
		wr.add_child(btn)
	_warp_label = Label.new()
	_warp_label.custom_minimum_size = Vector2(150, 0)
	_warp_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_warp_label.add_theme_font_size_override("font_size", 13)
	wr.add_child(_warp_label)
	wr.move_child(_warp_label, 2)

	_left = _panel_text(Vector2(10, 10), Vector2(270, 0))
	_right = _panel_text(Vector2(-280, 10), Vector2(270, 0), true)
	_stages = _panel_text(Vector2(10, 230), Vector2(230, 0))

	# bottom centre: throttle | navball | g-meter, SAS row beneath
	var bottom := VBoxContainer.new()
	bottom.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	bottom.offset_left = -230
	bottom.offset_right = 230
	bottom.offset_top = -330
	bottom.offset_bottom = -6
	bottom.alignment = BoxContainer.ALIGNMENT_END
	add_child(bottom)
	var row := HBoxContainer.new()
	row.alignment = BoxContainer.ALIGNMENT_CENTER
	row.add_theme_constant_override("separation", 14)
	bottom.add_child(row)
	_throttle = _gauge(row, "THR")
	navball = Navball.new()
	row.add_child(navball)
	_gmeter = _gauge(row, "G")
	_heat = _gauge(row, "HEAT")
	var spacer := Control.new()
	spacer.custom_minimum_size = Vector2(0, 20)
	bottom.add_child(spacer)
	var act_row := HBoxContainer.new()
	act_row.alignment = BoxContainer.ALIGNMENT_CENTER
	act_row.add_theme_constant_override("separation", 6)
	bottom.add_child(act_row)
	for a in [["stage", "STAGE  (Space)", "Fire the next stage"], ["chute", "CHUTE  (P)", "Arm the parachutes: they open once the air is thick enough and you are slow enough"],
			["legs", "LEGS  (G)", "Landing legs down / up"]]:
		var ab := Button.new()
		ab.text = a[1]
		ab.tooltip_text = a[2]
		ab.focus_mode = Control.FOCUS_NONE
		ab.add_theme_font_size_override("font_size", 12)
		var nm: String = a[0]
		ab.pressed.connect(func(): action_pressed.emit(nm))
		act_row.add_child(ab)
		_action_buttons[nm] = ab
	var sas_row := HBoxContainer.new()
	sas_row.alignment = BoxContainer.ALIGNMENT_CENTER
	bottom.add_child(sas_row)
	for b in SAS_BUTTONS:
		var btn := Button.new()
		btn.text = b[1]
		btn.toggle_mode = true
		btn.focus_mode = Control.FOCUS_NONE
		btn.add_theme_font_size_override("font_size", 12)
		btn.pressed.connect(func(): sas_pressed.emit(b[0]))
		sas_row.add_child(btn)
		_sas_buttons[b[0]] = btn

	_fuel = PanelContainer.new()
	_fuel.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	_fuel.offset_left = -290
	_fuel.offset_top = -190
	_fuel.offset_right = -10
	_fuel.offset_bottom = -10
	add_child(_fuel)
	var fv := VBoxContainer.new()
	fv.name = "V"
	_fuel.add_child(fv)

	portrait_slot = Control.new()
	portrait_slot.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	portrait_slot.offset_left = 10
	portrait_slot.offset_top = -250
	portrait_slot.offset_right = 250
	portrait_slot.offset_bottom = -10
	portrait_slot.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(portrait_slot)

	_node_label = Label.new()
	_node_label.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	_node_label.offset_top = -360
	_node_label.offset_bottom = -336
	_node_label.offset_left = -300
	_node_label.offset_right = 300
	_node_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_node_label.add_theme_color_override("font_color", Color(0.45, 0.7, 1.0))
	_node_label.add_theme_font_size_override("font_size", 15)
	add_child(_node_label)

	_toast = Label.new()
	_toast.set_anchors_preset(Control.PRESET_CENTER)
	_toast.offset_top = -180
	_toast.offset_left = -400
	_toast.offset_right = 400
	_toast.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_toast.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_toast.add_theme_font_size_override("font_size", 20)
	_toast.add_theme_color_override("font_color", Color(1.0, 0.85, 0.5))
	_toast.add_theme_color_override("font_outline_color", Color(0, 0, 0))
	_toast.add_theme_constant_override("outline_size", 6)
	add_child(_toast)

	_hint = Label.new()
	_hint.set_anchors_preset(Control.PRESET_CENTER)
	_hint.offset_top = 120
	_hint.offset_left = -300
	_hint.offset_right = 300
	_hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_hint.add_theme_font_size_override("font_size", 18)
	_hint.add_theme_color_override("font_color", Color(1.0, 0.75, 0.4))
	_hint.add_theme_color_override("font_outline_color", Color(0, 0, 0))
	_hint.add_theme_constant_override("outline_size", 5)
	_hint.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_hint)

	_build_menu()
	_build_help()
	_build_failure()


func _label(parent: Node, size: int, col: Color) -> Label:
	var l := Label.new()
	l.add_theme_font_size_override("font_size", size)
	l.add_theme_color_override("font_color", col)
	parent.add_child(l)
	return l


func _panel_text(pos: Vector2, size: Vector2, right := false) -> RichTextLabel:
	var p := PanelContainer.new()
	if right:
		p.set_anchors_preset(Control.PRESET_TOP_RIGHT)
		p.offset_left = pos.x
		p.offset_right = pos.x + size.x
		p.offset_top = pos.y
	else:
		p.position = pos
	p.custom_minimum_size = size
	add_child(p)
	var t := RichTextLabel.new()
	t.bbcode_enabled = true
	t.fit_content = true
	t.scroll_active = false
	t.custom_minimum_size = Vector2(size.x - 20, 0)
	t.add_theme_font_size_override("normal_font_size", 14)
	t.add_theme_font_size_override("bold_font_size", 14)
	p.add_child(t)
	return t


func _gauge(parent: Node, title: String) -> Control:
	var g := Control.new()
	g.custom_minimum_size = Vector2(40, Navball.SIZE)
	g.set_meta("title", title)
	g.set_meta("value", 0.0)
	g.set_meta("max", 1.0)
	g.draw.connect(func(): _draw_gauge(g))
	if title == "HEAT":
		g.mouse_filter = Control.MOUSE_FILTER_PASS
		parent.add_child(g)
		return g
	if title == "THR":
		g.tooltip_text = "Throttle: drag here, or Shift / Ctrl, Z full, X cut"
		g.mouse_default_cursor_shape = Control.CURSOR_VSIZE
		g.gui_input.connect(func(e: InputEvent):
			var press: bool = e is InputEventMouseButton and (e as InputEventMouseButton).button_index == MOUSE_BUTTON_LEFT and e.pressed
			var drag: bool = e is InputEventMouseMotion and ((e as InputEventMouseMotion).button_mask & MOUSE_BUTTON_MASK_LEFT) != 0
			if press or drag:
				var h := g.size.y - 40
				throttle_set.emit(clampf(1.0 - (e.position.y - 22.0) / (h - 4.0), 0.0, 1.0))
				g.accept_event())
	else:
		g.mouse_filter = Control.MOUSE_FILTER_IGNORE
	parent.add_child(g)
	return g


func _draw_gauge(g: Control) -> void:
	var top := 16.0
	var h := g.size.y - 24 - top
	var v: float = clampf(g.get_meta("value") / g.get_meta("max"), 0.0, 1.0)
	var title: String = g.get_meta("title")
	var font := get_theme_default_font()
	var ttw := font.get_string_size(title, HORIZONTAL_ALIGNMENT_LEFT, -1, 10).x
	g.draw_rect(Rect2(g.size.x * 0.5 - ttw * 0.5 - 3, 1, ttw + 6, 13), Color(0.05, 0.07, 0.11, 0.85))
	g.draw_string(font, Vector2(0, 11), title, HORIZONTAL_ALIGNMENT_CENTER, g.size.x, 10, Color(0.7, 0.78, 0.88))
	g.draw_rect(Rect2(6, top + 4, 22, h), Color(0.05, 0.07, 0.11, 0.9))
	var col := PartVisual.ORANGE if title == "THR" else Color(0.4, 0.95, 0.5).lerp(Color(1, 0.3, 0.2), v)
	if title == "HEAT":
		col = Color(0.45, 0.5, 0.6).lerp(Color(1.0, 0.55, 0.1), smoothstep(0.3, 0.6, v)).lerp(Color(1, 0.15, 0.1), smoothstep(0.75, 0.95, v))
		if v > 0.85 and fmod(Time.get_ticks_msec() / 1000.0, 0.5) < 0.25:
			col = Color(1, 0.9, 0.8)
	g.draw_rect(Rect2(8, top + 6 + (h - 4) * (1 - v), 18, (h - 4) * v), col)
	g.draw_rect(Rect2(6, top + 4, 22, h), Color(0.32, 0.5, 0.72), false, 1.5)
	var label := "%d%%" % roundi(v * 100.0)
	if title == "G":
		label = "%.1fg" % float(g.get_meta("value"))
	var tw := font.get_string_size(label, HORIZONTAL_ALIGNMENT_LEFT, -1, 12).x
	g.draw_rect(Rect2(g.size.x * 0.5 - tw * 0.5 - 4, g.size.y - 18, tw + 8, 17), Color(0.05, 0.07, 0.11, 0.85))
	g.draw_string(font, Vector2(0, g.size.y - 5), label, HORIZONTAL_ALIGNMENT_CENTER, g.size.x, 12, Color(0.85, 0.9, 0.97))


func _build_menu() -> void:
	_menu = PanelContainer.new()
	_menu.set_anchors_preset(Control.PRESET_CENTER)
	_menu.offset_left = -170
	_menu.offset_right = 170
	_menu.offset_top = -170
	_menu.offset_bottom = 170
	_menu.visible = false
	add_child(_menu)
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 10)
	_menu.add_child(v)
	var t := Label.new()
	t.text = "PASA MISSION CONTROL"
	t.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	t.add_theme_color_override("font_color", PartVisual.ORANGE)
	v.add_child(t)
	for item in [["Resume", "resume"], ["Revert to launch", "revert_launch"], ["Revert to hangar", "revert_hangar"], ["Main menu", "main_menu"]]:
		var b := Button.new()
		b.text = item[0]
		b.custom_minimum_size = Vector2(0, 42)
		b.pressed.connect(func(): menu_action.emit(item[1]))
		v.add_child(b)
	var vol_row := HBoxContainer.new()
	var vl := Label.new()
	vl.text = "Sound"
	vol_row.add_child(vl)
	var slider := HSlider.new()
	slider.min_value = 0.0
	slider.max_value = 1.0
	slider.step = 0.05
	slider.value = 0.0 if Sfx.muted else Sfx.master_volume
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slider.value_changed.connect(func(x: float):
		Sfx.master_volume = x
		Sfx.muted = x <= 0.0
		Sfx.save_settings())
	vol_row.add_child(slider)
	v.add_child(vol_row)


var _help: PanelContainer

const HELP := """[b][color=#ff8a2a]FLIGHT CONTROLS[/color][/b]      [color=#6f86a6]F1 to toggle[/color]

[b]Z[/b] full throttle   [b]X[/b] cut   [b]Shift / Ctrl[/b] throttle up / down (or drag the THR bar)
[b]Space[/b] stage (first press releases the clamps)   [b]P[/b] parachute   [b]G[/b] legs
[b]W A S D[/b] / arrows steer [i]as you see it[/i]: W tips the nose away from the camera,
   D swings it right on screen.   [b]Q E[/b] roll   [b]T[/b] SAS on/off
SAS buttons: hold, prograde, retrograde, normal, radial, [b]MNV[/b] (point at the burn)
[b]V[/b] surface / orbital speed

[b]. ,[/b] or the [b]▶▶ ◀◀[/b] buttons: faster / slower time   [b]/[/b] or [b]■[/b] normal time
   (big warps need you coasting above the air; in it, or under thrust, max 4x)
[b]M[/b] map, centred on you: click your orbit to add a maneuver node, drag its handles,
   then [b]Skip to burn[/b] fast-forwards until just before it. [b]Tab[/b] changes the focus
[b][ ][/b] switch vessel   [b]Esc[/b] pause: revert, sound, main menu
Right-drag to orbit the camera, wheel to zoom.

[color=#9fb4cc]Getting to orbit: climb straight up, start leaning east past ~1 km,
be nearly horizontal by ~40 km, cut the engine when the apoapsis is ~80 km,
then burn prograde at apoapsis until the periapsis clears 70 km.[/color]"""


func _build_help() -> void:
	_help = PanelContainer.new()
	_help.set_anchors_preset(Control.PRESET_CENTER)
	_help.offset_left = -330
	_help.offset_right = 330
	_help.offset_top = -250
	_help.offset_bottom = 250
	_help.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var t := RichTextLabel.new()
	t.bbcode_enabled = true
	t.fit_content = true
	t.scroll_active = false
	t.custom_minimum_size = Vector2(640, 0)
	t.text = HELP
	t.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_help.add_child(t)
	_help.visible = false
	add_child(_help)
	var cf := ConfigFile.new()
	cf.load(Sfx.SETTINGS)
	if not cf.get_value("ui", "seen_flight_help", false):
		_help.visible = true
		cf.set_value("ui", "seen_flight_help", true)
		cf.save(Sfx.SETTINGS)


func _build_failure() -> void:
	_fail = PanelContainer.new()
	_fail.set_anchors_preset(Control.PRESET_CENTER_TOP)
	_fail.offset_left = -270
	_fail.offset_right = 270
	_fail.offset_top = 130
	_fail.offset_bottom = 130
	_fail.add_theme_stylebox_override("panel", HangarTheme.box(Color(0.12, 0.04, 0.04, 0.94), 10, Color(0.85, 0.25, 0.2)))
	_fail.visible = false
	add_child(_fail)
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 10)
	_fail.add_child(v)
	_fail_title = Label.new()
	_fail_title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_fail_title.add_theme_font_size_override("font_size", 30)
	_fail_title.add_theme_color_override("font_color", Color(1.0, 0.4, 0.3))
	v.add_child(_fail_title)
	_fail_body = Label.new()
	_fail_body.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_fail_body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_fail_body.custom_minimum_size = Vector2(480, 0)
	_fail_body.add_theme_font_size_override("font_size", 16)
	v.add_child(_fail_body)
	var row := HBoxContainer.new()
	row.alignment = BoxContainer.ALIGNMENT_CENTER
	row.add_theme_constant_override("separation", 8)
	v.add_child(row)
	for item in [["Revert to launch", "revert_launch"], ["Back to hangar", "revert_hangar"], ["Keep watching", "dismiss"], ["Main menu", "main_menu"]]:
		var b := Button.new()
		b.text = item[0]
		b.focus_mode = Control.FOCUS_NONE
		var act: String = item[1]
		b.pressed.connect(func():
			if act == "dismiss":
				_fail.visible = false
			else:
				menu_action.emit(act))
		row.add_child(b)


func show_failure(title: String, body: String, hint: String) -> void:
	_fail_title.text = title
	_fail_body.text = body + "\n\n" + hint
	_fail.visible = true


func hide_failure() -> void:
	_fail.visible = false


func set_hint(s: String) -> void:
	_hint.text = s


func toggle_help() -> void:
	_help.visible = not _help.visible


func hide_help() -> void:
	_help.visible = false


func show_menu(on: bool) -> void:
	_menu.visible = on


func menu_visible() -> bool:
	return _menu.visible


func toast(msg: String, secs := 3.0) -> void:
	_toast.text = msg
	_toast.modulate.a = 1.0
	if _toast_tween:
		_toast_tween.kill()
	_toast_tween = create_tween()
	_toast_tween.tween_interval(secs)
	_toast_tween.tween_property(_toast, "modulate:a", 0.0, 0.8)


func clear_toast() -> void:
	if _toast_tween:
		_toast_tween.kill()
	_toast.modulate.a = 0.0


func set_node_text(s: String) -> void:
	_node_label.text = s


func set_warp_text(s: String) -> void:
	_warp_text = s
	_warp_label.text = s if s != "" else "time: normal"


static func fmt_dist(m: float) -> String:
	if absf(m) >= 1.0e6:
		return "%.2f Mm" % (m / 1.0e6)
	if absf(m) >= 1.0e4:
		return "%.2f km" % (m / 1000.0)
	return "%.0f m" % m


static func fmt_time(s: float) -> String:
	if s < 0 or is_inf(s) or is_nan(s):
		return "—"
	var d := int(s / 21600.0)  # Floe days are short; show hours instead
	var h := int(s / 3600.0)
	var m := int(fmod(s, 3600.0) / 60.0)
	var sec := fmod(s, 60.0)
	if h > 0:
		return "%dh %02dm %02ds" % [h, m, int(sec)]
	if m > 0:
		return "%dm %02ds" % [m, int(sec)]
	return "%.1fs" % sec


func update_from(t: Dictionary, mission_t: float) -> void:
	_t = t
	if not t.get("alive", false):
		_clock.text = "SIGNAL LOST"
		return
	_clock.text = "T+ " + fmt_time(mission_t)
	var body: String = t.body.capitalize()
	_status.text = "%s · %s%s" % [t.situation.replace("_", " ").to_upper(), body, ("   " + _warp_text) if _warp_text != "" else ""]
	var spd: float = t.surface_speed if speed_mode == "surface" else t.orbital_speed
	_left.text = "[color=#9fb4cc]ALTITUDE[/color]  [b]%s[/b]\n[color=#9fb4cc]Above terrain[/color]  %s\n[color=#9fb4cc]Vertical[/color]  %+.1f m/s\n[color=#9fb4cc]%s speed[/color]  [b]%.1f m/s[/b]  [color=#6f86a6](V to toggle)[/color]\n[color=#9fb4cc]Mass[/color] %.2f t   [color=#9fb4cc]TWR[/color] %.2f\n[color=#9fb4cc]Dyn. pressure[/color] %.1f kPa" % [
		fmt_dist(t.altitude), fmt_dist(t.altitude_terrain), t.vertical_speed,
		"Surface" if speed_mode == "surface" else "Orbital", spd, t.get("mass", 0.0) / 1000.0, t.get("twr", 0.0),
		t.dynamic_pressure / 1000.0]
	var o: Dictionary = t.orbit
	var ap: String = "—" if o.ap_alt == null else fmt_dist(o.ap_alt)
	var tap: String = "—" if o.t_to_ap == null else fmt_time(o.t_to_ap)
	_right.text = "[color=#9fb4cc]APOAPSIS[/color]  [b]%s[/b]   in %s\n[color=#9fb4cc]PERIAPSIS[/color]  [b]%s[/b]   in %s\n[color=#9fb4cc]Inclination[/color] %.2f°   [color=#9fb4cc]e[/color] %.4f\n[color=#9fb4cc]Period[/color] %s" % [
		ap, tap, fmt_dist(o.pe_alt), fmt_time(o.t_to_pe), o.inc_deg, o.ecc, fmt_time(o.period) if o.period > 0 else "—"]
	var st := "[color=#ff8a2a][b]STAGES[/b][/color]  [color=#6f86a6]next SPACE first[/color]\n"
	var dv_of := {}
	var total := 0.0
	for s in t.get("stages", []):
		dv_of[int(s.stage)] = s
		total += s.dv if t.in_atmosphere else s.dv_vac
	var burning: int = int(t.next_stage) + 1
	if dv_of.has(burning) and dv_of[burning].dv_vac >= 0.5 and t.get("engines_lit", 0) > 0:
		var b: Dictionary = dv_of[burning]
		st += "[color=#ffcf6a]▶ burning[/color]  [b]%.0f m/s[/b]  %s\n" % [b.dv if t.in_atmosphere else b.dv_vac, "TWR %.2f" % b.twr if b.twr > 0 else ""]
	var first := true
	for p in t.get("stage_plan", []):
		var what := PackedStringArray()
		if p.decouplers > 0:
			what.append("drop %d" % p.decouplers if p.decouplers > 1 else "drop")
		if p.engines > 0:
			what.append("ignite %d" % p.engines)
		if p.chutes > 0:
			what.append("chute")
		var line := "%s [color=#9fb4cc]%d[/color] %s" % ["[color=#ffcf6a]SPACE[/color]" if first else "       ", p.stage, ", ".join(what)]
		var d: Dictionary = dv_of.get(int(p.stage), {})
		if p.engines > 0 and not d.is_empty() and d.dv_vac >= 0.5:
			line += "  [b]%.0f m/s[/b]" % (d.dv if t.in_atmosphere else d.dv_vac)
		st += line + "\n"
		first = false
	st += "[color=#9fb4cc]Total[/color] [b]%.0f m/s[/b] %s" % [total, "(local pressure)" if t.in_atmosphere else "(vacuum)"]
	_stages.text = st
	_throttle.set_meta("value", t.throttle)
	_throttle.queue_redraw()
	_gmeter.set_meta("value", t.g_force)
	_gmeter.set_meta("max", 8.0)
	_gmeter.queue_redraw()
	var hf: float = t.get("heat_frac", 0.0)
	_heat.set_meta("value", hf)
	_heat.tooltip_text = "Skin heating: how much of its heat headroom the hottest part has used.\n%s at %.0f K, fails at %.0f K" % [
		Sim.part(t.heat_part).get("name", t.heat_part), t.heat_temp, t.heat_limit] if t.has("heat_part") else "Skin heating"
	_heat.queue_redraw()
	for k in _sas_buttons:
		_sas_buttons[k].set_pressed_no_signal(t.sas == k or (k == "maneuver" and maneuver_active))
	_update_fuel(t)


func _update_fuel(t: Dictionary) -> void:
	var v: VBoxContainer = _fuel.get_node("V")
	for c in v.get_children():
		c.queue_free()
	var title := Label.new()
	title.text = "PROPELLANT"
	title.add_theme_color_override("font_color", Color(0.62, 0.75, 0.9))
	title.add_theme_font_size_override("font_size", 13)
	v.add_child(title)
	title.text = "PROPELLANT · this stage"
	for kind in [["Liquid", "stage_liquid", Color(0.45, 0.75, 1.0)], ["Solid", "stage_solid", Color(1.0, 0.6, 0.25)]]:
		var cap: float = t.get(kind[1] + "_capacity", 0.0)
		if cap <= 0:
			continue
		var amt: float = t.get(kind[1], 0.0)
		var bar := ProgressBar.new()
		bar.max_value = cap
		bar.value = amt
		bar.show_percentage = false
		bar.custom_minimum_size = Vector2(250, 16)
		var fill := StyleBoxFlat.new()
		fill.bg_color = kind[2]
		bar.add_theme_stylebox_override("fill", fill)
		var l := Label.new()
		l.text = "%s  %.0f / %.0f kg" % [kind[0], amt, cap]
		l.add_theme_font_size_override("font_size", 12)
		v.add_child(l)
		v.add_child(bar)
	if t.get("stage_liquid_capacity", 0.0) <= 0.0 and t.get("stage_solid_capacity", 0.0) <= 0.0:
		var none := Label.new()
		none.text = "No engine in this stage"
		none.add_theme_font_size_override("font_size", 12)
		v.add_child(none)
	var whole := Label.new()
	whole.text = "Whole vessel: liquid %.0f kg%s" % [t.get("liquid", 0.0), ("   solid %.0f kg" % t.get("solid", 0.0)) if t.get("solid_capacity", 0.0) > 0.0 else ""]
	whole.add_theme_font_size_override("font_size", 11)
	whole.add_theme_color_override("font_color", Color(0.62, 0.72, 0.85))
	v.add_child(whole)
	var tw := Label.new()
	tw.text = "Thrust %.0f kN   Δv left %.0f m/s" % [t.get("thrust", 0.0) / 1000.0, t.get("dv_total", 0.0)]
	tw.add_theme_font_size_override("font_size", 13)
	v.add_child(tw)
