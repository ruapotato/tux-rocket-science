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
## Dev (GPU): the Script Editor in flight. The Orbiter on the pad running
## to_orbit.py from the editor, stopped at a breakpoint in the climb with
## the watch live; the console asked something; then the suggestions after
## an engine's name. Saves editor_<k>.png.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.seats = {}
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			c.queue_free()
	await tree.process_frame
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	Sim.pending_site = "pad"
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 10:
		await tree.process_frame
	scene.hud.hide_help()
	scene._toggle_desk()
	for i in 5:
		await tree.process_frame
	var de: Control = scene._de
	de._launch("code")
	for i in 5:
		await tree.process_frame
	var win: Control = de._find("script editor")
	win.position = Vector2(8, 30)
	win.size = Vector2(de.size.x - 16, de.size.y - 70)
	de._relayout(win)
	var app: Control = win.get_meta("content")
	app._open("/root/examples/to_orbit.py")
	for i in 3:
		await tree.process_frame
	app.edit.set_line_as_breakpoint(_line_of(app.edit, "chart("), true)
	app._run()
	var paused := false
	for i in 3000:
		await tree.process_frame
		if str(app.status.text).contains("paused"):
			paused = true
			break
	log.append("paused %s: %s" % [paused, app.status.text])
	for i in 20:
		await tree.process_frame
	await _shot(scene, "editor_breakpoint", log)
	app._console_line("ship.orbit.top / km")
	app._console_line("len(ship.parts)")
	for i in 30:
		await tree.process_frame
	await _shot(scene, "editor_console", log)
	app._pause()  # continue
	var ed: CodeEdit = app.edit
	var n := ed.get_line_count()
	ed.insert_line_at(n - 1, "lifter.")
	ed.set_caret_line(n - 1)
	ed.set_caret_column(7)
	ed.grab_focus()
	app._complete()
	for i in 10:
		await tree.process_frame
	await _shot(scene, "editor_suggest", log)
	app._stop()
	scene.queue_free()
	await tree.process_frame
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	return {"status": "PASS", "log": "\n".join(log)}


static func _line_of(ed: CodeEdit, text: String) -> int:
	for i in ed.get_line_count():
		if ed.get_line(i).contains(text):
			return i
	return 0


func _shot(n: Node, name: String, log: PackedStringArray) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png("user://dev_shots/%s.png" % name)
	log.append("saved " + name)
