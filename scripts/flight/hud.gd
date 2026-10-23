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

const SAS_BUTTONS := [
	["stability", "SAS"], ["prograde", "PRO"], ["retrograde", "RET"], ["normal", "NRM"],
	["antinormal", "ANT"], ["radial_out", "RAD+"], ["radial_in", "RAD-"],
]

var navball: Navball
var speed_mode := "surface"  # or "orbit"
var _clock: Label
var _status: Label
var _left: RichTextLabel
var _right: RichTextLabel
var _stages: RichTextLabel
var _fuel: Control
var _throttle: Control
var _gmeter: Control
var _sas_buttons := {}
var _toast: Label
var _toast_tween: Tween
var _menu: PanelContainer
var _t: Dictionary = {}
var _warp_text := ""
var portrait_slot: Control


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

	_left = _panel_text(Vector2(10, 10), Vector2(270, 0))
	_right = _panel_text(Vector2(-280, 10), Vector2(270, 0), true)
	_stages = _panel_text(Vector2(10, 230), Vector2(230, 0))

	# bottom centre: throttle | navball | g-meter, SAS row beneath
	var bottom := VBoxContainer.new()
	bottom.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	bottom.offset_left = -230
	bottom.offset_right = 230
	bottom.offset_top = -300
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
	var spacer := Control.new()
	spacer.custom_minimum_size = Vector2(0, 20)
	bottom.add_child(spacer)
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
	_fuel.offset_top = -170
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

	_build_menu()


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
	g.custom_minimum_size = Vector2(34, Navball.SIZE)
	g.set_meta("title", title)
	g.set_meta("value", 0.0)
	g.set_meta("max", 1.0)
	g.draw.connect(func(): _draw_gauge(g))
	parent.add_child(g)
	return g


func _draw_gauge(g: Control) -> void:
	var h := g.size.y - 24
	var v: float = clampf(g.get_meta("value") / g.get_meta("max"), 0.0, 1.0)
	g.draw_rect(Rect2(6, 4, 22, h), Color(0.05, 0.07, 0.11, 0.9))
	var col := PartVisual.ORANGE if g.get_meta("title") == "THR" else Color(0.4, 0.95, 0.5).lerp(Color(1, 0.3, 0.2), v)
	g.draw_rect(Rect2(8, 6 + (h - 4) * (1 - v), 18, (h - 4) * v), col)
	g.draw_rect(Rect2(6, 4, 22, h), Color(0.32, 0.5, 0.72), false, 1.5)
	g.draw_string(get_theme_default_font(), Vector2(2, g.size.y - 4), g.get_meta("title"), HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.8, 0.85, 0.95))


func _build_menu() -> void:
	_menu = PanelContainer.new()
	_menu.set_anchors_preset(Control.PRESET_CENTER)
	_menu.offset_left = -170
	_menu.offset_right = 170
	_menu.offset_top = -150
	_menu.offset_bottom = 150
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


func set_warp_text(s: String) -> void:
	_warp_text = s


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
	var st := "[color=#ff8a2a][b]STAGES[/b][/color]  [color=#6f86a6]SPACE to stage[/color]\n"
	var stages: Array = t.get("stages", [])
	var total := 0.0
	for s in stages:
		if s.dv_vac < 0.5:
			continue
		var cur: bool = int(s.stage) == int(t.next_stage) + 1
		st += "%s Stage %d  [b]%.0f m/s[/b]  %s\n" % ["[color=#ffcf6a]▶[/color]" if cur else "  ", s.stage, s.dv if t.in_atmosphere else s.dv_vac,
			"TWR %.2f" % s.twr if s.twr > 0 else ""]
		total += s.dv if t.in_atmosphere else s.dv_vac
	st += "[color=#9fb4cc]Total[/color] [b]%.0f m/s[/b] %s" % [total, "(local pressure)" if t.in_atmosphere else "(vacuum)"]
	_stages.text = st
	_throttle.set_meta("value", t.throttle)
	_throttle.queue_redraw()
	_gmeter.set_meta("value", t.g_force)
	_gmeter.set_meta("max", 8.0)
	_gmeter.queue_redraw()
	for k in _sas_buttons:
		_sas_buttons[k].set_pressed_no_signal(t.sas == k)
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
	for kind in [["Liquid", "liquid", Color(0.45, 0.75, 1.0)], ["Solid", "solid", Color(1.0, 0.6, 0.25)]]:
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
	var tw := Label.new()
	tw.text = "Thrust %.0f kN   Δv left %.0f m/s" % [t.get("thrust", 0.0) / 1000.0, t.get("dv_total", 0.0)]
	tw.add_theme_font_size_override("font_size", 13)
	v.add_child(tw)
