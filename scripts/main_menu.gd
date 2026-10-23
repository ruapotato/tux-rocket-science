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
## PASA main menu (full branding pass lands in M6).
extends Control


func _ready() -> void:
	$Center/VBox/Version.text = "trs_core %s" % Sim.core.version()
	theme = HangarTheme.make()
	var box: VBoxContainer = $Center/VBox
	for item in [["Assembly Hangar", _hangar], ["Quit", func(): get_tree().quit()]]:
		var b := Button.new()
		b.text = item[0]
		b.custom_minimum_size = Vector2(320, 48)
		b.pressed.connect(item[1])
		box.add_child(b)


func _hangar() -> void:
	get_tree().change_scene_to_file("res://scenes/hangar.tscn")
