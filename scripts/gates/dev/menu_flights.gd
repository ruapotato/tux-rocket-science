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
## Dev: the main menu with craft left in the world (one in orbit, one
## landed at home, a piece of debris), the "Craft in flight" panel open.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var g := Sim.game_session()
	var orbiter := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	var hopper := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "hopper.craft.json")
	var o: int = g.core.flight_spawn_orbit(orbiter, "floe", 120000.0, 0.0)
	Sim.seat_crew(o, Crew.assign(1))
	var h: int = g.core.flight_launch(hopper, "floe")
	Sim.seat_crew(h, Crew.assign(1))
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("tank_s"))
	m.craft_name = "Spent tank"
	g.core.flight_spawn_orbit(m.save_text(), "floe", 95000.0, 0.0)
	Sim.save_game()
	var menu: Control = load("res://scenes/main_menu.tscn").instantiate()
	tree.root.add_child(menu)
	for i in 5:
		await tree.process_frame
	menu._toggle_flights()
	for i in 5:
		await tree.process_frame
	var log := PackedStringArray()
	for v in Sim.vessels():
		log.append("%s: %s %s crew %s" % [v.name, v.situation, v.body, v.crew_names])
	if DisplayServer.get_name() != "headless":
		await RenderingServer.frame_post_draw
		DirAccess.make_dir_recursive_absolute("user://dev_shots")
		menu.get_viewport().get_texture().get_image().save_png("user://dev_shots/menu_flights.png")
	return {"status": "PASS", "log": "\n".join(log)}
