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
## Dev (GPU): the wild and the lasers, filmed. A Warden Lander set down
## near a herd of SPECIES (default crawlers) on BODY (gloam), a penguin out
## beside it, the lander's lasers on GUARD (0..2, default 1), in daylight;
## frames saved as wild_<n>.png every EVERY frames for FRAMES frames, from
## DIST m off (CAM_YAW, CAM_PITCH). NEST=1: by the body's first Jaeger
## nest instead, 2.5 km off it. Env SEED picks the herd. ZAP=1: the
## penguin zaps the nearest hostile.
extends RefCounted

const DIR := "user://dev_shots/"


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var env := func(k: String, d: String) -> String: return OS.get_environment(k) if OS.get_environment(k) != "" else d
	var bid: String = env.call("BODY", "gloam")
	var species: String = env.call("SPECIES", "crawlers")
	var text := lander_only(FileAccess.get_file_as_string("res://data/craft/warden_lander.craft.json"))
	if tree.current_scene:
		tree.current_scene.queue_free()
	await _frames(tree, 2)
	Sim.tutorial = false
	Sim.use_sandbox()
	Sim.pending_craft_text = text
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	scene.hud.visible = env.call("HUD", "1") == "1"
	var s: FlightSession = scene.session
	var core: TrsSim = s.core
	var bi: int = core.body_find(bid)
	var R: float = JSON.parse_string(core.body_info(bi)).radius
	var lat := 0.0
	var lon := 0.0
	if env.call("NEST", "") != "":
		var n: Dictionary = JSON.parse_string(core.nests(bi))[0]
		lat = float(n.lat) - rad_to_deg(float(env.call("NEST_KM", "2.5")) * 1000.0 / R)
		lon = float(n.lon)
	else:
		var at: PackedFloat64Array = core.find_life(bi, species, int(env.call("SEED", "1")))
		if at.size() < 2:
			return {"status": "FAIL", "log": "no %s on %s" % [species, bid]}
		lat = at[0] - rad_to_deg(45.0 / R)
		lon = at[1]
	# daylight first, with the lander alone there
	var pad := s.active
	var lv: PackedFloat64Array = core.level_spot(bi, lat, lon, 250.0, 0.006)  # (on level ground: a lander tips on a dune's face)
	log.append("level spot: slope %.3f, %.0f m from where asked" % [lv[2], lv[3]])
	lat = lv[0]
	lon = lv[1]
	var id: int = core.flight_place_landed(text, bid, lat, lon)
	var tilt := func(why: String) -> void:
		var lt: Dictionary = JSON.parse_string(core.telemetry(id))
		log.append("lander %s: pitch %.1f deg, mode %s, %s, %.2f m up, %.2f m/s" % [why, float(lt.get("pitch_deg", 0)), lt.get("mode", ""),
			lt.get("situation", ""), float(lt.get("altitude_terrain", 0)), float(lt.get("surface_speed", 0))])
	tilt.call("placed")
	s.switch_to(id)
	core.vessel_remove(pad)
	await _frames(tree, 3)
	for b in scene._booms:  # (its view goes up in smoke as it goes: not here)
		if is_instance_valid(b):
			b.queue_free()
	scene._booms.clear()
	scene._wreck_set = false
	if env.call("LEGS", "1") == "1":
		core.vessel_set_legs(id, true)
	tilt.call("legs out")
	s.step_ticks(150)  # (standing on its legs)
	tilt.call("150 ticks on")
	for q in 0 if Sim.look(bid).get("locked", false) else 400:
		var sp: PackedFloat64Array = core.subsolar_point(bi)
		if _angle(sp[0], sp[1], lat, lon) < 60.0:
			break
		s.warp_to(s.time() + 1800.0)
	core.set_guard(id, int(env.call("GUARD", "1")))
	var pen: int = core.vessel_eva_exit(id, 0) if env.call("PEN", "1") == "1" else -1
	if pen < 0:
		core.vessel_set_mode(id, "physics")
	tilt.call("penguin out")
	if pen >= 0:
		s.switch_to(pen)
	scene._cam_yaw = float(env.call("CAM_YAW", "2.6"))
	scene._cam_pitch = float(env.call("CAM_PITCH", "0.25"))
	scene._cam_dist = float(env.call("DIST", "18"))
	var every := int(env.call("EVERY", "30"))
	var frames := int(env.call("FRAMES", "600"))
	var shot := 0
	for f in frames:
		await tree.process_frame
		# ZAP=1: the penguin keeps its zapper on the nearest hostile (as a
		# player holding the mouse on it would)
		if env.call("ZAP", "") != "" and pen >= 0 and f % 6 == 0 and s.active == pen:
			var best := {}
			for th in JSON.parse_string(core.things_near(pen, 60)):
				if th.get("hostile", false) and (best.is_empty() or float(th.distance) < float(best.distance)):
					best = th
			if not best.is_empty():
				core.lock_on(pen, int(best.kind), int(best.id), int(best.serial))
		# FOLLOW=1: the camera on a Jaeger raider once one is up
		if env.call("FOLLOW", "") != "" and f % 30 == 0:
			for v in JSON.parse_string(core.vessel_list()):
				if v.get("pilot", false) and not JSON.parse_string(core.telemetry(s.active)).get("alien", false):
					s.switch_to(int(v.id))
					for b in scene._booms:
						if is_instance_valid(b):
							b.queue_free()
					scene._booms.clear()
					break
		if f % every == every - 1:
			await RenderingServer.frame_post_draw
			tilt.call("at shot %d" % shot)
			var tv: TerrainView = scene._terrain
			var missing := 0
			for k in tv._want:
				missing += 0 if tv._patches.has(k) else 1
			log.append("frame %d: ground %d of %d patches still to build" % [f, missing, tv._want.size()])
			scene.get_viewport().get_texture().get_image().save_png(DIR + "wild_%02d.png" % shot)
			var t := s.telemetry()
			var near: Array = JSON.parse_string(core.things_near(s.active, 200))
			var hostile := 0
			for th in near:
				hostile += 1 if th.get("hostile", false) else 0
			log.append("shot %d at %.1f s: penguin %s health %.0f, %d near (%d hostile), lander firing %d" % [shot, f / 60.0,
				"alive" if t.get("alive", false) else "LOST", float(t.get("health", 0.0)), near.size(), hostile,
				int(JSON.parse_string(core.telemetry(id)).get("firing", 0))])
			if scene._views.has(s.active):
				var vv = scene._views[s.active]
				var pp := []
				for u in vv._parts:
					var nd: Node3D = vv._parts[u].node
					pp.append("%s %s vis %s kids %d" % [vv._parts[u].part, str(nd.global_position), nd.is_visible_in_tree(), nd.get_child_count()])
				log.append("   active view: %s; camera at %s; wreck %s %s" % [", ".join(pp), str(scene._cam.global_position), scene._wreck_set, str(scene._wreck)])
			for e in s.events:
				if str(e.kind) in ["part_destroyed", "vessel_created"]:
					log.append("   event %s vessel %s part %s speed %s" % [e.kind, e.get("vessel"), e.get("part", ""), e.get("speed", "")])
			s.events.clear()
			for v in JSON.parse_string(core.vessel_list()):
				if not v.get("alien", false):
					log.append("   ours %d %s mode %s situation %s" % [int(v.id), v.name, v.mode, v.situation])
				if v.get("alien", false):
					log.append("   alien %d %s parts %d render %d mode %s views %s" % [int(v.id), v.name, int(v.get("parts", 0)),
						core.vessel_render(int(v.id), s.active).size(), v.mode, scene._views.has(int(v.id))])
			shot += 1
	return {"status": "PASS", "log": "\n".join(log)}


## The Warden Lander without its boosters and transfer stage: what sets
## down (the parts above its lander's engine, the turrets and battery).
static func lander_only(text: String) -> String:
	var c: Dictionary = JSON.parse_string(text)
	var keep := []
	var at := {}
	for i in c.parts.size():
		var p: Dictionary = c.parts[i]
		var par := int(p.parent)
		if i <= 11 or (par >= 0 and at.has(par) and not str(p.part) in ["decoupler_stack"]):
			at[i] = keep.size()
			var q := p.duplicate(true)
			q.parent = at[par] if par >= 0 else -1
			keep.append(q)
	c.parts = keep
	c.name = "Warden Lander (lander)"
	return JSON.stringify(c)


static func _angle(la1: float, lo1: float, la2: float, lo2: float) -> float:
	var a := Vector3(cos(deg_to_rad(la1)) * cos(deg_to_rad(lo1)), cos(deg_to_rad(la1)) * sin(deg_to_rad(lo1)), sin(deg_to_rad(la1)))
	var b := Vector3(cos(deg_to_rad(la2)) * cos(deg_to_rad(lo2)), cos(deg_to_rad(la2)) * sin(deg_to_rad(lo2)), sin(deg_to_rad(la2)))
	return rad_to_deg(a.angle_to(b))


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame
