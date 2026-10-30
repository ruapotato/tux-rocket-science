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
## Dev: `land near <relic>` on its own. CRAFT (default the Krill Lander's
## lander) in an orbit of BODY (krill) ALT m up (30000), INC degrees
## (12), then the program, its phase and note logged as it goes, and
## where it came down. PLACE: the words after `land` (near chime).
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var env := func(k: String, d: String) -> String: return OS.get_environment(k) if OS.get_environment(k) != "" else d
	var body: String = env.call("BODY", "krill")
	var craft: String = env.call("CRAFT", "")
	var text := ""
	if craft == "":
		var c: Dictionary = JSON.parse_string(FileAccess.get_file_as_string("res://data/craft/krill_lander.craft.json"))
		c.parts = c.parts.slice(0, 12)
		text = JSON.stringify(c)
	else:
		var Play: GDScript = load("res://scripts/gates/dev/playthrough.gd")
		text = Play.design(craft).save_text()
	if tree.current_scene:
		tree.current_scene.queue_free()
	for i in 2:
		await tree.process_frame
	Sim.tutorial = false
	Sim.pending_craft_text = text
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var pad := s.active
	if env.call("NOLADDER", "") != "":
		var c: Dictionary = JSON.parse_string(text)
		var keep := []
		for pp in c.parts:
			if str(pp.part) != "ladder":
				keep.append(pp)
		c.parts = keep
		text = JSON.stringify(c)
	var id: int = -1
	if env.call("STAND", "") != "":
		# set down standing at LAT, LON (the place it'd land), then watched
		# with nothing running: does it stay up?
		id = s.core.flight_place_landed(text, body, float(env.call("LAT", "32")), float(env.call("LON", "16")))
		s.switch_to(id)
		s.core.vessel_remove(pad)
		for k in 500:
			s.step_ticks(1)
			if k % 25 == 0:
				var tt := s.telemetry()
				log.append("    T+%.1f pitch %.1f, contacts %s, landed %s, clearance %.2f, %.2f m/s" % [k / 50.0, float(tt.pitch_deg), tt.get("contacts"), tt.landed, float(tt.get("clearance", -1)), float(tt.surface_speed)])
			await tree.process_frame
		return {"status": "PASS", "log": "\n".join(log)}
	if env.call("PE", "") != "":
		# at the top of an ALT x PE orbit (m up), INC degrees
		var bi_: int = s.core.body_find(body)
		var bd: Dictionary = JSON.parse_string(s.core.body_info(bi_))
		var ra: float = float(bd.radius) + float(env.call("ALT", "30000"))
		var rp: float = float(bd.radius) + float(env.call("PE", "30000"))
		var va: float = sqrt(float(bd.mu) * (2.0 / ra - 2.0 / (ra + rp)))
		var inc := deg_to_rad(float(env.call("INC", "12")))
		id = s.core.flight_spawn_state(text, body, PackedFloat64Array([ra, 0, 0, 0, va * cos(inc), va * sin(inc)]))
	else:
		id = s.core.flight_spawn_orbit(text, body, float(env.call("ALT", "30000")), float(env.call("INC", "12")))
	s.switch_to(id)
	s.core.vessel_remove(pad)
	for i in 3:
		await tree.process_frame
	if env.call("PARTS", "") != "":
		var pp: Array = JSON.parse_string(s.core.vessel_parts(id))
		var ps: Array = JSON.parse_string(s.core.vessel_part_states(id))
		for i in pp.size():
			log.append("    part %d %s parent %d kind %s stage %d" % [i, pp[i].part, int(pp[i].parent), ps[i].kind, int(ps[i].stage)])
		log.append("    next stage %d" % int(s.telemetry().get("next_stage", -9)))
	var comp: ShipComputer = scene.computer
	if comp == null:
		comp = ShipComputer.get_shared()
		scene.computer = comp
	var words: String = "land " + (env.call("PLACE", "near chime") if env.call("PLACE", "") != "none" else "")
	words = words.strip_edges()
	comp.programs.start(scene, words.split(" "), s.telemetry())
	var p := comp.programs
	var last := ""
	var said_t := -1e9
	var t0 := s.time()
	var bi0: int = s.core.body_find(body)
	var rl0: Dictionary = {}
	for r in JSON.parse_string(s.core.body_info(bi0)).get("relics", []):
		rl0 = r
	var R0: float = JSON.parse_string(s.core.body_info(bi0)).radius
	var gap := func(tt: Dictionary) -> float:
		var a := Vector3(cos(deg_to_rad(tt.lat_deg)) * cos(deg_to_rad(tt.lon_deg)), cos(deg_to_rad(tt.lat_deg)) * sin(deg_to_rad(tt.lon_deg)), sin(deg_to_rad(tt.lat_deg)))
		var b := Vector3(cos(deg_to_rad(rl0.lat_deg)) * cos(deg_to_rad(rl0.lon_deg)), cos(deg_to_rad(rl0.lat_deg)) * sin(deg_to_rad(rl0.lon_deg)), sin(deg_to_rad(rl0.lat_deg)))
		return a.angle_to(b) * R0
	var w0 := Time.get_ticks_msec()
	var max_t := float(env.call("MAXT", "1e12"))
	while p.running() and Time.get_ticks_msec() - w0 < 900000 and s.time() - t0 < max_t:
		var t := s.telemetry()
		var low: bool = float(t.get("altitude_terrain", 1e9)) < 300.0 and not t.get("landed", false)
		s.step_ticks(1 if low else 5)
		comp.update(scene, s.telemetry())
		var now := "%s | %s" % [p.phase, p.note]
		if p.phase != last or s.time() - said_t > 20.0:
			last = p.phase
			said_t = s.time()
			t = s.telemetry()
			log.append("T+%7.0f %s (at %.2f, %.2f; alt %.1f km, %.0f m/s, %.0f m/s left, %.0f kg liquid, thrust %.0f kN, stage %d, %.1f km from it)" % [s.time() - t0, now,
				float(t.get("lat_deg", 0)), float(t.get("lon_deg", 0)), float(t.get("altitude", 0)) / 1000.0, float(t.get("surface_speed", 0)), float(t.get("dv_total", 0)),
				float(t.get("liquid", 0)), float(t.get("thrust", 0)) / 1000.0, int(t.get("next_stage", -9)), gap.call(t) / 1000.0])
		if not s.telemetry().get("alive", false):
			break
		var tl := s.telemetry()
		if p.phase == "descent" and OS.get_environment("FEET") != "" and int(s.time()) % 5 == 0:
			log.append("    descent: agl %.1f clearance %.1f vs %.1f thr %.2f contacts %s" % [float(tl.altitude_terrain), float(tl.get("clearance", -1)), float(tl.vertical_speed), float(tl.throttle), tl.get("contacts")])
		if p.phase == "descent" and float(tl.get("clearance", 99)) < 3.0 and not has_meta("td%d" % absi(int(float(tl.clearance) * 4))):
			set_meta("td%d" % absi(int(float(tl.clearance) * 4)), true)
			var upv := Vector3(tl.up[0], tl.up[1], tl.up[2])
			var svv := Vector3(tl.surface_velocity[0], tl.surface_velocity[1], tl.surface_velocity[2])
			log.append("    near the ground: feet %.2f m up, falling %.2f m/s, %.1f m/s across (%.2f by the program's sum), pitch %.1f, throttle %.2f, impact so far %.1f, contacts %s, landed %s, slope %s" % [float(tl.clearance), -float(tl.vertical_speed),
				float(tl.horizontal_speed), (svv - upv * float(tl.vertical_speed)).length(), float(tl.pitch_deg), float(tl.throttle), float(tl.get("max_impact", 0)), tl.get("contacts"), tl.landed, str(tl.get("slope_deg", "?"))])
		if p.phase == "settle" and not has_meta("st%d" % int(s.time() * 2)):
			set_meta("st%d" % int(s.time() * 2), true)
			log.append("    settle: pitch %.1f, contacts %s, %.2f m/s, landed %s" % [float(tl.pitch_deg), tl.get("contacts"), float(tl.surface_speed), tl.landed])
		if p.phase in ["descent", "settle"] and int(s.time()) % 2 == 0 and OS.get_environment("ENG") != "":
			for ps in JSON.parse_string(s.core.vessel_part_states(s.active)):
				if str(ps.kind) in ["engine", "leg"]:
					log.append("    %s temp %.0f/%.0f damage %.1f/%.0f (agl %.1f, vs %.1f)" % [ps.part, float(ps.temp), float(ps.max_temp), float(ps.get("damage", 0)), float(ps.get("hp", 0)),
						float(s.telemetry().get("altitude_terrain", 0)), float(s.telemetry().get("vertical_speed", 0))])
		for e in s.events:
			if str(e.kind) in ["part_destroyed", "overheat"]:
				var te := s.telemetry()
				var nose := Vector3(te.nose[0], te.nose[1], te.nose[2])
				var sv := Vector3(te.surface_velocity[0], te.surface_velocity[1], te.surface_velocity[2])
				log.append("    T+%7.0f %s %s (%s): alt %.1f km, %.0f m/s, q %.0f kPa, heat %s %.2f; nose %.1f deg off the air's way in, sas %s, phase %s" % [s.time() - t0, e.kind, e.get("part", ""), str(e.get("cause", e.get("reason", ""))),
					float(te.get("altitude", 0)) / 1000.0, float(te.get("surface_speed", 0)), float(te.get("dynamic_pressure", 0)) / 1000.0, te.get("heat_part"), float(te.get("heat_frac", 0)),
					rad_to_deg(nose.angle_to(-sv)), te.get("sas"), p.phase])
		s.events.clear()
		for line in p.said:
			log.append("    said: " + line)
		p.said.clear()
		await tree.process_frame
	if env.call("EVA", "") != "" and p.status == "done":
		# a penguin out, and what becomes of it
		var lander := s.active
		var tc := s.telemetry()
		var ladder := false
		for pp in JSON.parse_string(s.core.vessel_parts(lander)):
			ladder = ladder or str(pp.part) == "ladder"
		var pen: int = s.core.vessel_eva_exit(lander, 0)
		s.switch_to(pen)
		var te := s.telemetry()
		log.append("    out: ladder aboard %s, craft pitch %.0f, penguin %.2f m up (clearance %.2f), %s" % [ladder, float(tc.pitch_deg), float(te.get("altitude_terrain", -1)), float(te.get("clearance", -1)), te.get("situation", "")])
		for k in 150:
			s.step_ticks(1)
			for e in s.events:
				if str(e.kind) == "part_destroyed":
					log.append("    T+%.2f destroyed %s on vessel %s (%s)" % [k / 50.0, e.get("part", ""), e.get("vessel", ""), str(e)])
			s.events.clear()
			te = s.telemetry()
			if k % 25 == 0:
				log.append("    T+%.1f penguin alive %s, %.2f m up, %.2f m/s down, contacts %s" % [k / 50.0, te.get("alive", false), float(te.get("altitude_terrain", -1)), -float(te.get("vertical_speed", 0)), te.get("contacts")])
	for line in comp.log:
		log.append("    computer: " + str(line))
	var t := s.telemetry()
	log.append("at the end: thrust_max %.0f kN, engines lit %s, starved %s, heat %s %.2f (%s K), parts %s, events %s" % [float(t.get("thrust_max", 0)) / 1000.0,
		t.get("engines_lit"), t.get("starved"), t.get("heat_part"), float(t.get("heat_frac", 0)), t.get("heat_temp"), t.get("parts"),
		str(s.events.filter(func(e): return str(e.kind) in ["part_destroyed", "overheat", "vessel_created"]).map(func(e): return "%s %s" % [e.kind, e.get("part", "")]))])
	var bi: int = s.core.body_find(body)
	var rl: Dictionary = {}
	for r in JSON.parse_string(s.core.body_info(bi)).get("relics", []):
		rl = r
	var R: float = JSON.parse_string(s.core.body_info(bi)).radius
	var a := Vector3(cos(deg_to_rad(t.lat_deg)) * cos(deg_to_rad(t.lon_deg)), cos(deg_to_rad(t.lat_deg)) * sin(deg_to_rad(t.lon_deg)), sin(deg_to_rad(t.lat_deg)))
	var b := Vector3(cos(deg_to_rad(rl.lat_deg)) * cos(deg_to_rad(rl.lon_deg)), cos(deg_to_rad(rl.lat_deg)) * sin(deg_to_rad(rl.lon_deg)), sin(deg_to_rad(rl.lat_deg)))
	var lv: PackedFloat64Array = s.core.level_spot(bi, float(t.lat_deg), float(t.lon_deg), 1.0, 1.0)
	log.append("ground there: slope %.4f (%.1f deg)" % [lv[2], rad_to_deg(atan(lv[2]))])
	log.append("ended %s: %s; %s, %.0f m from %s, alive %s, %.0f m/s left, %.0f s" % [p.status, p.note, t.get("situation", ""),
		a.angle_to(b) * R, rl.get("name", "?"), t.get("alive", false), float(t.get("dv_total", 0)), s.time() - t0])
	return {"status": "PASS" if p.status == "done" else "FAIL", "log": "\n".join(log)}
