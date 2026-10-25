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
## Dev: leaving Floe's sphere of influence with the map open: the camera
## should ease out to show Floe's neighbourhood, not jump.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	var craft := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	Sim.pending_craft_text = craft
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var core := s.core
	var soi: float = JSON.parse_string(core.body_info(1)).soi
	var state := PackedFloat64Array([soi * 0.97, 0, 0, 1400.0, 0, 300.0])
	core.vessel_remove(s.active)
	s.active = core.flight_spawn_state(craft, "floe", state)
	scene._clear_views()
	scene._map.visible = true
	scene._map.focus_on_vessel()
	scene._map._dist = 6.0
	var log := PackedStringArray(["Floe SOI %.0f km" % (soi / 1000.0)])
	var why := s.set_warp("rails", 5)
	log.append("warp: '%s' kind %s idx %d, body %s, r %.0f km" % [why, s.warp_kind, s.warp_index, s.telemetry().body, s.telemetry().altitude / 1000.0])
	var changed := -1
	var shots := [0, 20, 60, 160]
	for f in 400:
		await tree.process_frame
		if changed < 0 and s.telemetry().body != "floe":
			changed = f
			s.set_warp("rails", 0)
			log.append("left Floe at frame %d, now around %s" % [f, s.telemetry().body])
		if changed >= 0 and shots.has(f - changed):
			await RenderingServer.frame_post_draw
			scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/map_soi_%d.png" % (f - changed))
			log.append("  +%d frames: map distance %.1f" % [f - changed, scene._map._dist])
		if changed >= 0 and f - changed > shots.max():
			break
	log.append("end: body %s alt %.0f km, warp %s %d, t %.0f" % [s.telemetry().body, s.telemetry().altitude / 1000.0, s.warp_kind, s.warp_index, s.time()])
	return {"status": "PASS" if changed >= 0 else "FAIL", "log": "\n".join(log)}
