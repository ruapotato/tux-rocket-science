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
## Dev: Austra to Lantern in the flight scene. A drive ship starts just
## past Austra's heliopause, points at Lantern (SAS target), runs the
## Stillwater Drive flat out with warp on the ship's clock, turns round
## halfway and stops short of Lantern's heliopause. Logs the HUD on the
## way and the clocks at the end; GPU runs save shots. FRAMES= frames per
## half (default 600).
extends RefCounted

var _log := PackedStringArray()
var _bad := ""


func _nonfinite(scene: Node) -> String:
	for n in scene.get_tree().root.find_children("*", "Node3D", true, false):
		var gx: Transform3D = (n as Node3D).global_transform
		if not (gx.origin.is_finite() and gx.basis.x.is_finite() and gx.basis.y.is_finite() and gx.basis.z.is_finite()):
			return "%s %s" % [n.get_path(), gx]
	for n in scene.get_tree().root.find_children("*", "Node3D", true, false):
		var gx: Transform3D = (n as Node3D).global_transform
		if gx.origin.length() > 1e18 or gx.basis.get_scale().length() > 1e18:
			return "huge: %s %s" % [n.get_path(), gx]
	return "none found"


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var g: FlightSession = Sim.game_session()
	var core: TrsSim = g.core
	var m := CraftModel.new()
	m.place_root(CraftModel.held_single("probe_core"))
	var hc := m.attach_stack(CraftModel.held_single("hearth_core"), 0, "bottom")[0]
	m.attach_stack(CraftModel.held_single("stillwater_drive"), hc, "bottom")
	m.attach_surface(CraftModel.held_single("reaction_wheel"), hc, 0.0, 0.0, 1)
	var text := m.save_text()
	var austra: int = core.body_find("austra")
	var lantern: int = core.body_find("lantern")
	var ai: Dictionary = JSON.parse_string(core.body_info(austra))
	var li: Dictionary = JSON.parse_string(core.body_info(lantern))
	var lg: Array = li.galactic
	var dir := [float(lg[0]), float(lg[1]), float(lg[2])]
	var n := sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2])
	dir = [dir[0] / n, dir[1] / n, dir[2] / n]
	var r0: float = 4.04e10
	var id: int = core.flight_spawn_state(text, "austra", PackedFloat64Array([dir[0] * r0, dir[1] * r0, dir[2] * r0, 0, 0, 0]))
	Sim.pending_vessel = id
	var scene := await _flight(tree)
	var s: FlightSession = scene.session
	s.step_ticks(5)
	var t := s.telemetry()
	_log.append("start: mode %s, situation %s, drive max %.0f g" % [t.mode, t.situation, float(t.get("drive_max", 0.0)) / 9.80665])
	var ok: bool = t.get("interstellar", false)
	var frames := int(OS.get_environment("FRAMES")) if OS.get_environment("FRAMES") != "" else 600
	var errors0: int = runner._errors.count
	var t0: float = s.time()
	var real := 0
	if DisplayServer.get_name() != "headless":
		scene._cam_pitch = -1.2  # under the ship, looking along its nose (toward Lantern), the way ahead off to one side
		scene._cam_dist = 40.0
		for i in 20:
			await tree.process_frame
		await _shot(scene, "interstellar_rest")
	# the Course panel's button for Lantern
	await tree.process_frame
	var btn: Button = null
	for b in scene._course.get_child(0).get_children():
		if b is Button and b.text.contains("Lantern"):
			btn = b
	btn.pressed.emit()
	await tree.process_frame
	_log.append("course set: toast '%s'" % scene.hud._toast.text)
	s.set_warp("rails", 7)
	var shot := false
	for k in frames * 100:
		s.advance(1.0 / 60.0)  # a 60 Hz frame of warp, without waiting for one
		real += 1
		if k % 100 == 0:
			await tree.process_frame
			if runner._errors.count > errors0 and _bad == "":
				_bad = _nonfinite(scene)
				_log.append("first engine error: %s | node %s" % [runner._errors.first, _bad])
		t = s.telemetry()
		if DisplayServer.get_name() != "headless" and float(t.get("beta", 0.0)) > 0.5 and not has_meta("half_c"):
			set_meta("half_c", true)
			await tree.process_frame
			await tree.process_frame
			await _shot(scene, "interstellar_half_c")
		if not shot and float(t.get("beta", 0.0)) > 0.99:
			shot = true
			await tree.process_frame
			_log.append("cruise: %s | %s | %s | %s" % [scene.hud._clock.text, scene.hud._left.get_parsed_text().replace("\n", " / "), scene.hud._right.get_parsed_text().replace("\n", " / "),
				scene._course.get_child(0).get_child(1).text])
			var stn: Node3D = scene._scaled.get_node("Stars")
			_log.append("stars node: visible %s, in tree %s, count %d, sky viewport size %s, cam far %s" % [stn.is_visible_in_tree(), stn.is_inside_tree(), scene._scaled._real_stars.size(), scene._scaled.size, scene._scaled.cam.far])
			if DisplayServer.get_name() != "headless":
				await _shot(scene, "interstellar_cruise")
				scene._scaled.get_texture().get_image().save_png("user://dev_shots/interstellar_sky.png")
		if not t.has("course"):
			break
	s.set_throttle(0.0)
	s.set_warp("rails", 0)
	t = s.telemetry()
	var home: float = s.time() - t0
	_log.append("real time %.1f s of 60 Hz frames; warp now %s %d" % [real / 60.0, s.warp_kind, s.warp_index])
	_log.append("end: %s; beta %.6f; home %.4f yr, ship behind by %.4f yr; mode %s; errors %d" % [
		scene.hud._clock.text, t.get("beta", 0.0), home / 31557600.0, float(t.tau_lag) / 31557600.0, t.mode, runner._errors.count - errors0])
	_log.append("stars: " + scene.hud._right.get_parsed_text().replace("\n", " / "))
	ok = ok and home > 3.0 * 31557600.0 and runner._errors.count == errors0
	if DisplayServer.get_name() != "headless":
		await _shot(scene, "interstellar_end")
	return {"status": "PASS" if ok else "FAIL", "log": "\n".join(_log)}


func _flight(tree: SceneTree) -> Node3D:
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	return scene


func _shot(n: Node, name: String) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png("user://dev_shots/%s.png" % name)
	_log.append("saved " + name)
