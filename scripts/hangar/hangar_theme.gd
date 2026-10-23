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
## PASA UI theme: deep navy glass panels, ice-blue edges, signal-orange accents.
class_name HangarTheme
extends RefCounted

const BG := Color(0.06, 0.09, 0.15, 0.92)
const EDGE := Color(0.32, 0.5, 0.72, 0.8)
const TEXT := Color(0.9, 0.94, 1.0)


static func box(color: Color, radius := 8, border := Color(0, 0, 0, 0), border_w := 1) -> StyleBoxFlat:
	var s := StyleBoxFlat.new()
	s.bg_color = color
	s.set_corner_radius_all(radius)
	if border.a > 0:
		s.border_color = border
		s.set_border_width_all(border_w)
	s.content_margin_left = 8
	s.content_margin_right = 8
	s.content_margin_top = 5
	s.content_margin_bottom = 5
	return s


static func make() -> Theme:
	var t := Theme.new()
	t.default_font_size = 15
	t.set_stylebox("panel", "PanelContainer", box(BG, 10, EDGE))
	t.set_stylebox("normal", "Button", box(Color(0.13, 0.19, 0.3), 6, Color(0.25, 0.38, 0.55)))
	t.set_stylebox("hover", "Button", box(Color(0.18, 0.27, 0.42), 6, Color(0.5, 0.7, 0.95)))
	t.set_stylebox("pressed", "Button", box(Color(0.9, 0.45, 0.1), 6))
	t.set_stylebox("disabled", "Button", box(Color(0.1, 0.12, 0.16), 6))
	t.set_stylebox("focus", "Button", StyleBoxEmpty.new())
	t.set_color("font_color", "Button", TEXT)
	t.set_color("font_hover_color", "Button", Color(1, 1, 1))
	t.set_color("font_disabled_color", "Button", Color(0.45, 0.5, 0.6))
	t.set_color("font_color", "Label", TEXT)
	t.set_color("default_color", "RichTextLabel", TEXT)
	t.set_stylebox("normal", "LineEdit", box(Color(0.04, 0.06, 0.1), 6, Color(0.25, 0.38, 0.55)))
	t.set_color("font_color", "LineEdit", TEXT)
	t.set_stylebox("panel", "TabContainer", box(Color(0, 0, 0, 0), 0))
	t.set_stylebox("tab_selected", "TabContainer", box(Color(0.9, 0.45, 0.1), 5))
	t.set_stylebox("tab_unselected", "TabContainer", box(Color(0.12, 0.17, 0.27), 5))
	t.set_stylebox("tab_hovered", "TabContainer", box(Color(0.18, 0.27, 0.42), 5))
	t.set_font_size("font_size", "TabContainer", 12)
	t.set_stylebox("panel", "PopupMenu", box(Color(0.06, 0.09, 0.15, 0.98), 8, EDGE))
	t.set_stylebox("panel", "TooltipPanel", box(Color(0.04, 0.06, 0.1, 0.97), 6, EDGE))
	t.set_color("font_color", "TooltipLabel", TEXT)
	return t
