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
## Main-menu panels for the space program: Mission Control (PASA's
## missions, done and on offer) and R&D (the tech tree, unlocked with
## research). Both read and change only Career.
class_name ProgramPanels
extends RefCounted


static func _panel(title: String, width: float) -> Array:
	var p := PanelContainer.new()
	p.custom_minimum_size = Vector2(width, 0)
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 8)
	p.add_child(v)
	var h := Label.new()
	h.text = title
	h.add_theme_color_override("font_color", PartVisual.ORANGE)
	h.add_theme_font_size_override("font_size", 18)
	v.add_child(h)
	var s := Label.new()
	s.text = "%s · %.1f research" % [Career.money(Career.funds) if Career.is_career() else "Sandbox: parts are free", Career.research]
	s.add_theme_color_override("font_color", Color(0.75, 0.85, 1.0))
	v.add_child(s)
	return [p, v]


static func missions() -> PanelContainer:
	var pv := _panel("MISSION CONTROL", 620)
	var v: VBoxContainer = pv[1]
	var active := Career.active_missions().map(func(m): return m.id)
	for m in Career.missions_def.missions:
		var done: bool = m.id in Career.done
		var open: bool = m.id in active
		if not done and not open:
			continue
		var l := RichTextLabel.new()
		l.bbcode_enabled = true
		l.fit_content = true
		l.scroll_active = false
		l.custom_minimum_size = Vector2(590, 0)
		var reward := "+%d research%s" % [int(m.research), ", +%s" % Career.money(float(m.funds)) if Career.is_career() else ""]
		l.text = "%s [b]%s[/b] · %s\n[color=#9fb4cc]%s[/color]" % ["[color=#7fe08a]✓[/color]" if done else "[color=#ffcf6a]▶[/color]",
			m.title, reward, m.text]
		v.add_child(l)
	var later: int = Career.missions_def.missions.size() - Career.done.size() - active.size()
	if later > 0:
		var more := Label.new()
		more.text = "%d more mission%s once these are done" % [later, "" if later == 1 else "s"]
		more.add_theme_color_override("font_color", Color(0.55, 0.62, 0.72))
		v.add_child(more)
	return pv[0]


static func rnd(on_change: Callable) -> PanelContainer:
	var pv := _panel("RESEARCH & DEVELOPMENT", 660)
	var v: VBoxContainer = pv[1]
	if not Career.is_career():
		var l := Label.new()
		l.text = "In the sandbox every part is unlocked. Start a career for the tech tree."
		v.add_child(l)
		return pv[0]
	for n in Career.tech:
		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 10)
		var b := Button.new()
		b.focus_mode = Control.FOCUS_NONE
		b.custom_minimum_size = Vector2(150, 0)
		var why := Career.tech_refusal(n.id)
		var have: bool = n.id in Career.unlocked
		b.text = "Researched" if have else "Research (%d)" % int(n.cost)
		b.disabled = why != ""
		var id: String = n.id
		b.pressed.connect(func():
			Career.unlock(id)
			Sfx.play("click")
			on_change.call())
		row.add_child(b)
		var l := RichTextLabel.new()
		l.bbcode_enabled = true
		l.fit_content = true
		l.scroll_active = false
		l.custom_minimum_size = Vector2(480, 0)
		var names := (n.parts as Array).map(func(p): return Sim.part(p).get("name", p))
		l.text = "[b]%s[/b]%s\n[color=#9fb4cc]%s[/color]" % [n.name, "" if have or why == "" else "  [color=#ff9a7a](%s)[/color]" % why, ", ".join(names)]
		row.add_child(l)
		v.add_child(row)
	return pv[0]
