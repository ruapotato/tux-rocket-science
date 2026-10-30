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
## Dev: a career played from the start, as a player would: craft launched
## from the pad through the real flight scene (paid for, refused when
## their parts aren't researched), flown by the keys or by the ship's
## computer's programs, penguins walked out by the keys, research spent in
## R&D. Nothing is teleported. Each chapter goes on from the save the last
## one left (user://playtest/); CHAPTERS= picks which (default: all, a new
## career first). Logs what happened and where it got stuck.
extends RefCounted

const DIR := "user://playtest/"
const CHAPTERS := ["floe", "computer", "orbit", "krill", "chime", "hearth", "keel", "lantern", "ember", "brine", "marrow", "tarn", "hollow"]
## the thread, star by star: [star, world, relic, a word for it]
const THREAD := {"lantern": ["lantern", "haven", "haven_archive", "archive"], "ember": ["ember", "gloam", "gloam_lamp", "lamp"],
	"brine": ["brine", "selkie", "selkie_anchor", "anchor"], "marrow": ["marrow", "saffron", "saffron_hourglass", "hourglass"],
	"tarn": ["tarn", "mere", "mere_garden", "garden"], "hollow": ["hollow", "cairn", "cairn_beacon", "beacon"]}

var tree: SceneTree
var scene: Node3D
var s: FlightSession
var log := PackedStringArray()
var stuck := ""
var wall0 := 0
var _sit_seen := ""
var _beat := 0
var _probe := 0


func say(line: String) -> void:
	var txt := "[%5.0fs real, T+%s] %s" % [(Time.get_ticks_msec() - wall0) / 1000.0, _clock(), line]
	log.append(txt)
	print("PLAY ", txt)


func _clock() -> String:
	if Sim.game == null:
		return "-"
	var t: float = Sim.game.core.time()
	return "%dd %02d:%02d:%02d" % [int(t / 86400.0), int(fmod(t, 86400.0) / 3600.0), int(fmod(t, 3600.0) / 60.0), int(fmod(t, 60.0))]


func run(runner: Node) -> Dictionary:
	tree = runner.get_tree()
	tree.paused = false
	wall0 = Time.get_ticks_msec()
	var want := OS.get_environment("CHAPTERS")
	var from := OS.get_environment("FROM")
	var chapters: Array = CHAPTERS if want == "" else Array(want.split(","))
	if from != "":
		# (on from a chapter's checkpoint: the save as it ended)
		chapters = CHAPTERS.slice(CHAPTERS.find(from) + 1) if want == "" else chapters
		_restore(from)
	_program(want == "" and from == "")
	for ch in chapters:
		say("== chapter %s: %s, %.0f research, done %s" % [ch, Career.money(Career.funds), Career.research, Career.done])
		await call("ch_" + ch)
		if stuck != "":
			say("STUCK in %s: %s" % [ch, stuck])
			break
		Career.save()
		Sim.save_game()
		_checkpoint(ch)
	say("ended: %s, %.0f research, %d missions done: %s; unlocked %s" % [Career.money(Career.funds), Career.research, Career.done.size(), Career.done, Career.unlocked])
	Engine.time_scale = 1.0
	return {"status": "FAIL" if stuck != "" else "PASS", "log": "\n".join(log)}


const SAVES := ["world.json", "game.json", "crew.json", "career.json"]


## The save as chapter `ch` left it, kept (user://playtest_ckpt/<ch>/).
func _checkpoint(ch: String) -> void:
	var to := "user://playtest_ckpt/%s/" % ch
	DirAccess.make_dir_recursive_absolute(to)
	for f in SAVES:
		DirAccess.copy_absolute(ProjectSettings.globalize_path(DIR + f), ProjectSettings.globalize_path(to + f))


func _restore(ch: String) -> void:
	var from := "user://playtest_ckpt/%s/" % ch
	DirAccess.make_dir_recursive_absolute(DIR)
	for f in SAVES:
		DirAccess.copy_absolute(ProjectSettings.globalize_path(from + f), ProjectSettings.globalize_path(DIR + f))
	say("on from the end of chapter %s" % ch)


## The playtest's own career (never the player's saves).
func _program(fresh: bool) -> void:
	Sim.program_chosen = true
	Sim.save_dir = DIR
	Sim.game = null
	Sim.seats = {}
	DirAccess.make_dir_recursive_absolute(DIR)
	if fresh:
		for f in ["world.json", "game.json", "crew.json", "career.json"]:
			DirAccess.remove_absolute(ProjectSettings.globalize_path(DIR + f))
	Crew.path = DIR + "crew.json"
	Crew.load_roster()
	Career.load_from(DIR + "career.json", "career")
	Sim.game_session()


# ---- the player's hands -----------------------------------------------------

func frames(n: int) -> void:
	for i in n:
		await tree.process_frame


func key(code: Key, hold := 0) -> void:
	var e := InputEventKey.new()
	e.keycode = code
	e.physical_keycode = code
	e.pressed = true
	Input.parse_input_event(e)
	await frames(maxi(hold, 1))
	var u := e.duplicate()
	u.pressed = false
	Input.parse_input_event(u)
	await frames(1)


func hold(code: Key, down: bool) -> void:
	var e := InputEventKey.new()
	e.keycode = code
	e.physical_keycode = code
	e.pressed = down
	Input.parse_input_event(e)


func tel() -> Dictionary:
	return s.telemetry()


## Waits (the flight running at `speed` times; with `warp`, on rails at
## that rate when the game allows it, as the . key would) until cond() or
## max_s of sim time pass or the craft is lost. True if cond() came true.
func until(cond: Callable, max_s: float, speed := 4.0, warp := 0) -> bool:
	var t0 := s.time()
	var w0 := Time.get_ticks_msec()
	var ok := false
	while true:
		var t := tel()
		if cond.call():
			ok = true
			break
		# an eye on the science panel: whatever's new where the craft is now
		var sit := "%s/%s/%s" % [t.get("body", ""), Career.situation(t), t.get("biome", "")]
		if sit != _sit_seen and t.get("alive", false):
			_sit_seen = sit
			science()
		if not t.get("alive", false) or s.time() - t0 > max_s or Time.get_ticks_msec() - w0 > 3600000:
			break
		if Engine.get_frames_per_second() < 15 and Time.get_ticks_msec() - _probe > 30000 and Engine.get_frames_per_second() > 0:
			_probe = Time.get_ticks_msec()
			probe_frame()
		if Time.get_ticks_msec() - _beat > 60000:
			_beat = Time.get_ticks_msec()
			var pr: FlightPrograms = scene.computer.programs if scene.computer else null
			say("   (still going: %s %s '%s'; warp %s %d; %.0f fps; %s over %s, %s)" % [pr.name if pr else "-", pr.phase if pr else "",
				pr.note if pr else "", s.warp_kind, s.warp_index, Engine.get_frames_per_second(), t.get("situation", ""), t.get("body", ""),
				FlightPrograms.km(float(t.get("altitude", 0.0)))])
		# fine steps near the ground, as a careful player would
		var low: bool = float(t.get("altitude_terrain", 1e9)) < 200.0 and not t.get("landed", false)
		if warp > 0 and not low and not (s.warp_kind == "rails" and s.warp_index >= warp):
			s.set_warp("rails", warp)
		Engine.time_scale = 1.0 if low or s.warp_kind == "rails" else speed
		await tree.process_frame
	Engine.time_scale = 1.0
	if warp > 0:
		s.set_warp("physics", 0)
	return ok


## Where a slow frame goes: each part of the flight scene's frame, timed.
func probe_frame() -> void:
	var out := []
	var dt := 1.0 / 60.0
	var parts := {
		"telemetry": func(): s._telemetry_tick = -1; s.telemetry(),
		"advance": func(): s.advance(dt),
		"computer": func(): scene.computer.update(scene, s.telemetry()),
		"render": func(): scene._render(dt),
		"combat": func(): scene.combat.step(dt),
		"course": func(): scene._update_course_panel(),
		"missions": func(): scene._check_missions(),
		"step 8": func(): s.step_ticks(8),
	}
	for k in parts:
		var u := Time.get_ticks_usec()
		parts[k].call()
		out.append("%s %.1f ms" % [k, (Time.get_ticks_usec() - u) / 1000.0])
	say("   slow frame (%.0f fps): %s" % [Engine.get_frames_per_second(), ", ".join(out)])


## A program on the ship's computer (as `ship <words>` at its shell), run
## to its end. Returns its last note.
func program(words: String, max_s := 4.0e6) -> String:
	var comp: ShipComputer = scene.computer
	var seen := comp.log_count
	comp.programs.start(scene, words.split(" "), tel())
	var p := comp.programs
	var ok := await until(func(): return not p.running(), max_s)
	# (what it said on the way, as the computer's log has it)
	var n := mini(comp.log_count - seen, comp.log.size())
	for line in comp.log.slice(comp.log.size() - n):
		say("   computer: " + str(line))
	var how := "%s %s: %s" % [words, p.status, p.note]
	say("program " + how + ("" if ok else " (still going when given up on)"))
	if p.status == "failed":
		var t := tel()
		say("   the ship then: %s, %.1f km up, %.0f m/s, thrust max %.0f kN, %.0f m/s left, %.0f kg liquid, engine power %s of %s kW, ec %.0f/%.0f (gen %.0f, use %.0f), next stage %d, starved %s, parts %d, heat %s %.2f" % [
			t.get("situation", "?"), float(t.get("altitude", 0)) / 1000.0, float(t.get("surface_speed", 0)), float(t.get("thrust_max", 0)) / 1000.0, float(t.get("dv_total", 0)),
			float(t.get("liquid", 0)), str(t.get("engine_power", "-")), str(t.get("engine_kw", "-")), float(t.get("ec", 0)), float(t.get("ec_capacity", 0)), float(t.get("ec_gen", 0)), float(t.get("ec_use", 0)), int(t.get("next_stage", -9)), str(t.get("starved", [])),
			int(t.get("parts", 0)), t.get("heat_part", ""), float(t.get("heat_frac", 0))])
	return p.status


func launch(craft: String, site := "pad") -> bool:
	var text := craft if craft.begins_with("{") else FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + craft + ".craft.json")
	var why := Career.launch_refusal(text)
	var cost := Career.craft_cost(Career.craft_parts(text))
	if why != "" or Career.funds < cost:
		say("can't launch %s: %s" % [craft if not craft.begins_with("{") else JSON.parse_string(text).name, why if why != "" else "costs %s, PASA has %s" % [Career.money(cost), Career.money(Career.funds)]])
		return false
	await _leave_scene()
	Sim.tutorial = false
	Sim.pending_craft_text = text
	Sim.pending_site = site
	scene = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	tree.current_scene = scene
	await frames(5)
	scene.hud.hide_help()
	s = scene.session
	if not s.sim_event.is_connected(_on_event):
		s.sim_event.connect(_on_event)
	say("launched %s from the %s (%s; PASA has %s)" % [tel().get("name", "?"), site, Career.money(cost), Career.money(Career.funds)])
	return true


## Parts lost, as they go (what a player would see blow).
func _on_event(e: Dictionary) -> void:
	if str(e.get("kind", "")) == "part_destroyed":
		var t := tel()
		say("   lost: %s (%s, %.0f m up, %.1f m/s, %s)" % [e.get("part", "?"), t.get("situation", ""), float(t.get("altitude_terrain", 0)),
			float(t.get("surface_speed", 0)), "vessel %s: %s" % [e.get("vessel", "?"), str(e)]])


func _leave_scene() -> void:
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			c.queue_free()
	await frames(3)


## The Recover button.
func recover() -> void:
	var f0 := Career.funds
	var r0 := Career.research
	scene._recover()
	await frames(4)
	say("recovered: %s (+%s, +%.1f research)" % [Sim.last_mission_note, Career.money(Career.funds - f0), Career.research - r0])
	await _leave_scene()


## Every experiment aboard that would bring something home from here.
func science() -> void:
	var got := []
	for x in Career.experiments_on(s.active, tel()):
		if x.why == "" and Career.run(s.active, x.uid, x.exp, tel()) == "":
			got.append("%s %.1f" % [x.name, x.value])
	if not got.is_empty():
		say("science (%s): %s" % [Career.situation(tel()), ", ".join(got)])


## Missions that came in since `before`.
func news(before: Array) -> String:
	var out := []
	for id in Career.done:
		if not id in before:
			out.append(Career.mission(id).title)
	return ", ".join(out)


func unlock(ids: Array) -> void:
	for id in ids:
		if id in Career.unlocked:
			continue
		var why := Career.unlock(id)
		say("R&D %s: %s" % [id, "unlocked" if why == "" else why])


## On EVA: walks the penguin toward a point (east, north metres from it now
## given by `where`, a callable) by holding W with the camera behind it,
## until within `reach` m. True if it got there.
func walk_to(where: Callable, reach: float, max_s := 600.0) -> bool:
	var ok := false
	hold(KEY_W, true)
	var t0 := s.time()
	var said_t := -1e9
	while s.time() - t0 < max_s:
		var d: Vector2 = where.call()
		if d.length() < reach:
			ok = true
			break
		if not tel().get("alive", false):
			break
		if s.time() - said_t > 30.0:
			said_t = s.time()
			say("   walking: %.0f m to go, at %.2f m/s, %s" % [d.length(), float(tel().get("surface_speed", 0.0)), "on the ground" if tel().get("landed", false) else "off the ground"])
		scene._cam_yaw = atan2(d.x, d.y)
		Engine.time_scale = 4.0
		await tree.process_frame
	Engine.time_scale = 1.0
	hold(KEY_W, false)
	await frames(2)
	return ok


## On EVA: walks toward the nearest of `species` and presses E when one is
## near enough for a good look (a bird: once right under it). True if the
## look got it into the catalogue.
func look_at(species: String, max_s: float) -> bool:
	var t0 := s.time()
	var said_t := -1e9
	var body := str(tel().body)
	while s.time() - t0 < max_s and tel().get("alive", false):
		var best := {}
		var seen := false
		for th in JSON.parse_string(s.core.things_near(s.active, 1500)):
			if str(th.get("species", "")) != species:
				continue
			seen = seen or FlightCombat.in_view(th)
			if best.is_empty() or bearing(th).length() < bearing(best).length():
				best = th
		if seen:
			hold(KEY_W, false)
			await key(KEY_E)
			Engine.time_scale = 1.0
			return (body + "/" + species) in Career.catalogued
		if best.is_empty():
			break
		var d := bearing(best)
		if s.time() - said_t > 60.0:
			said_t = s.time()
			say("   after the %s: %.0f m off along the ground, %.0f m up" % [species, d.length(), float(best.get("above", 0.0))])
		# (a flock overhead goes round: wait under it rather than chase)
		var stand := float(best.get("above", 0.0)) > 5.0 and d.length() < 25.0
		hold(KEY_W, not stand)
		if not stand:
			scene._cam_yaw = atan2(d.x, d.y)
		Engine.time_scale = 4.0
		await tree.process_frame
	hold(KEY_W, false)
	Engine.time_scale = 1.0
	await frames(2)
	return false


## On EVA: back to `craft` and in: walks to it until a hatch (or its
## ladder) is in reach, then F. True if aboard.
func board(craft: int) -> bool:
	var me := s.active
	var t0 := s.time()
	var said_t := -1e9
	var last := INF
	var stuck_t := s.time()
	var side := 0.0
	while s.time() - t0 < 1800.0 and tel().get("alive", false):
		var h: PackedFloat64Array = s.core.eva_hatch(me, craft)
		if h[0] >= 0:
			break
		var d := toward_vessel(craft)
		if s.time() - said_t > 60.0:
			said_t = s.time()
			say("   back to the craft: %.1f m off, hatch %.1f m out of reach" % [d.length(), h[1]])
		# (pressed up against it and still out of reach: round it a little)
		if d.length() < last - 0.3:
			last = d.length()
			stuck_t = s.time()
		elif s.time() - stuck_t > 8.0:
			side = 1.2 if side == 0.0 else 0.0
			stuck_t = s.time()
			last = INF
		var go := d
		if d.length() < 2.5:
			# (close by and still out of reach: round it, a step out from
			# its middle, till the ladder or a hatch is at hand; under a
			# tall lander's middle neither is)
			var radial := -d.normalized() if d.length() > 0.05 else Vector2(1, 0)
			var tangent := Vector2(-radial.y, radial.x)
			go = tangent + radial * (1.7 - d.length())
		scene._cam_yaw = atan2(go.x, go.y) + (side if d.length() >= 2.5 else 0.0)
		hold(KEY_W, true)
		Engine.time_scale = 4.0
		await tree.process_frame
	hold(KEY_W, false)
	Engine.time_scale = 1.0
	await frames(2)
	await key(KEY_F)
	await frames(5)
	return s.active == craft


## (east, north) metres to a thing_info / things_near row.
static func bearing(th: Dictionary) -> Vector2:
	var b := deg_to_rad(float(th.bearing))
	var d := float(th.distance)
	return Vector2(sin(b), cos(b)) * sqrt(maxf(d * d - pow(float(th.get("above", 0.0)), 2.0), 0.0))


## (east, north) metres from the active vessel to vessel `id`, in its frame.
func toward_vessel(id: int) -> Vector2:
	var me := tel()
	var it: Dictionary = JSON.parse_string(s.core.telemetry(id))
	if not it.get("alive", false):
		return Vector2.ZERO
	var R: float = JSON.parse_string(s.core.body_info(int(me.body_index))).radius
	var dn := deg_to_rad(float(it.lat_deg) - float(me.lat_deg)) * R
	var de := deg_to_rad(wrapf(float(it.lon_deg) - float(me.lon_deg), -180.0, 180.0)) * R * cos(deg_to_rad(float(me.lat_deg)))
	return Vector2(de, dn)


# ---- the craft, as a player would build them in the hangar --------------------

## Each design and the world it's for.
const DESIGNS := {"hearth_diver": "kelp", "hearth_yard": "kelp", "hearth_yard_capture": "kelp", "star_skua": "haven"}


static func held(id: String) -> Dictionary:
	return CraftModel.held_single(id)


static func design(name: String) -> CraftModel:
	var m := CraftModel.new()
	match name:
		"hearth_diver", "hearth_diver_lander", "hearth_diver_transfer", "hearth_yard", "hearth_yard_capture":
			# a lander for the thick air and the weight of Kelp: down on the
			# Skua-sized lifter engine, with a scanner to chart the Hearth
			m.place_root(held("pod_1"))
			m.attach_stack(held("parachute"), 0, "top")
			var w := m.attach_stack(held("reaction_wheel"), 0, "bottom")[0]
			var tl := m.attach_stack(held("tank_l"), w, "bottom")[0]
			var lander_engine := m.attach_stack(held("engine_lifter"), tl, "bottom")[0]
			# (on the engine's housing: the feet reach below its bell)
			m.attach_surface(held("landing_leg"), lander_engine, PI / 4, 0.3, 4)
			m.attach_surface(held("signal_scanner"), tl, 0.0, 1.2, 1)
			m.attach_surface(held("battery"), tl, PI, 1.2, 1)
			m.attach_surface(held("solar_panel"), tl, PI / 2, 0.0, 2)
			m.attach_surface(held("ladder"), tl, PI / 4 + PI / 2, 0.0, 1)
			# a wide heat shield under the engine for the way in through the
			# air (its shadow covers the legs), dropped when it's slow
			# enough to fly the rest on the engine (which mustn't fire
			# before: its jet would burn through what holds the shield;
			# the stage under the shield makes the burn down)
			var ds := m.attach_stack(held("decoupler_stack"), lander_engine, "bottom")[0]
			var ad1 := m.attach_stack(held("adapter_21"), ds, "bottom")[0]
			var shield := m.attach_stack(held("heat_shield_xl"), ad1, "bottom")[0]
			if name == "hearth_diver_lander":
				m.auto_stage()
				m.craft_name = "Hearth Diver"
				return m
			if name.begins_with("hearth_yard"):
				# built in the Orbital Yard, round Floe already: a stage under
				# the shield that can make the whole capture into a low orbit
				# round Kelp and the burn down (a lifter: quick about it), and
				# one above for the way there
				var dy := m.attach_stack(held("decoupler_xl"), shield, "bottom")[0]
				var ty := m.attach_stack(held("tank_xl"), dy, "bottom")[0]
				var ey := m.attach_stack(held("engine_lifter"), ty, "bottom")[0]
				if name == "hearth_yard_capture":
					m.auto_stage()
					m.craft_name = "Hearth Diver"
					return m
				var dz := m.attach_stack(held("decoupler_stack"), ey, "bottom")[0]
				var az := m.attach_stack(held("adapter_21"), dz, "bottom")[0]
				var tz := m.attach_stack(held("tank_xl"), az, "bottom")[0]
				var tz2 := m.attach_stack(held("tank_xl"), tz, "bottom")[0]
				var mz := m.attach_stack(held("mount_tri"), tz2, "bottom")[0]
				for nd in ["bottom", "bottom_2", "bottom_3"]:
					m.attach_stack(held("engine_vac"), mz, nd)
				m.auto_stage()
				m.craft_name = "Hearth Diver"
				return m
			# a stage under the shield for the capture round Kelp and the burn
			# down (the lander's engine can't fire with the shield under it)
			var d1 := m.attach_stack(held("decoupler_xl"), shield, "bottom")[0]
			var tc := m.attach_stack(held("tank_xm"), d1, "bottom")[0]
			var ec := m.attach_stack(held("engine_vac"), tc, "bottom")[0]
			if name == "hearth_diver_transfer":
				m.auto_stage()
				m.craft_name = "Hearth Diver"
				return m
			# the stage that takes it from Floe's orbit on its way to Kelp
			var d0 := m.attach_stack(held("decoupler_stack"), ec, "bottom")[0]
			var ad0 := m.attach_stack(held("adapter_21"), d0, "bottom")[0]
			var t1 := m.attach_stack(held("tank_xl"), ad0, "bottom")[0]
			# (three vacuum engines: one is too weak to round the orbit off
			# with all that, and a lifter's poorer exhaust costs the margin)
			var mt := m.attach_stack(held("mount_tri"), t1, "bottom")[0]
			var ev := -1
			for nd in ["bottom", "bottom_2", "bottom_3"]:
				var e := m.attach_stack(held("engine_vac"), mt, nd)
				if not e.is_empty() and ev < 0:
					ev = e[0]
			# and the one off the pad: a wide core, lifters strapped on (each
			# pass of the loop gives the pair its symmetry twins: four in all)
			var d2 := m.attach_stack(held("decoupler_stack"), ev, "bottom")[0]
			var ad := m.attach_stack(held("adapter_21"), d2, "bottom")[0]
			var x1 := m.attach_stack(held("tank_xl"), ad, "bottom")[0]
			var x2 := m.attach_stack(held("tank_xl"), x1, "bottom")[0]
			m.attach_stack(held("engine_big"), x2, "bottom")
			m.attach_surface(held("fin"), x2, PI / 4, -1.2, 4)
			for r in m.attach_surface(held("decoupler_radial"), x1, 0.0, 0.0, 2):
				var side := m.attach_stack(held("tank_l"), r, "outer")
				if side.is_empty():
					side = m.attach_surface(held("tank_l"), r, 0.0, 0.0, 1)
				if side.is_empty():
					continue
				var st := m.attach_stack(held("tank_l"), side[0], "bottom")
				m.attach_stack(held("engine_lifter"), st[0] if not st.is_empty() else side[0], "bottom")
				m.attach_stack(held("nosecone"), side[0], "top")
			m.auto_stage()
			m.craft_name = "Hearth Diver"
		"star_skua":
			# the Skua (one stage, pad to orbit to anywhere on the Hearth
			# Torch) with the Stillwater Drive for the stars, a scanner to
			# hear the relics and lasers for the Jaegers
			m.place_root(held("hab_box"))
			m.set_size(0, [1.0, 1.0, 1.4])
			var prow := held("glasswing_wedge")
			prow.parts[0]["size"] = [1.0, 0.9, 1.4]
			m.attach_stack(prow, 0, "top")
			var tank := held("tank_box")
			tank.parts[0]["size"] = [1.0, 1.1, 1.4]
			var t := m.attach_stack(tank, 0, "bottom")[0]
			var core := m.attach_stack(held("hearth_core"), t, "bottom")[0]
			var drive := m.attach_stack(held("stillwater_drive"), core, "bottom")[0]
			var torch := m.attach_stack(held("hearth_torch"), drive, "bottom")[0]
			var wing := held("wing")
			wing.parts[0]["size"] = [1.0, 1.2, 1.3]
			m.attach_surface(wing, t, PI / 2, -0.8, CraftModel.MIRROR)
			# (low on the torch's housing: the feet a metre below its bell,
			# or on a dune's slope the bell's rim is what it stands on and
			# it goes over like a pencil on its point; wide: the ship
			# stands tall; twelve, as fewer springs give
			# under a ship this heavy and tall (its middle 7 m over the
			# feet) and let a lean on a dune's face grow till it goes over)
			m.attach_surface(held("landing_leg"), torch, PI / 12, -0.6, 12)
			m.attach_surface(held("signal_scanner"), 0, 0.0, 0.0, 1)
			m.attach_surface(held("laser_turret"), core, PI / 4, 0.0, 2)
			# (low on the ship, by the legs: from the ground a penguin must
			# reach it to climb in; but its foot above theirs, or the ship
			# lands on the ladder and goes over)
			m.attach_surface(held("ladder"), torch, 0.0, -0.3, 1)
			# (and living off the land between the stars: drills low on the
			# torch, between the legs and in reach of the ground, refineries
			# and a bin on the drive; the tanks filled on each world)
			m.attach_surface(held("drill"), torch, PI / 2, -0.8, 3)
			m.attach_surface(held("refinery"), drive, 0.0, 0.0, 2)
			m.attach_surface(held("ore_bin"), drive, PI / 2, 0.0, 1)
			m.auto_stage()
			m.craft_name = "Star Skua"
	return m


# ---- the chapters -----------------------------------------------------------

## (No flight: the wild round the pad, as a map would show it.)
func ch_wildmap() -> void:
	s = Sim.game_session()
	var bi := s.core.body_find("floe")
	map_life(bi, -0.097, -74.6, 15000.0)
	map_life(bi, -0.097, -74.6, 3000.0)


## A new career: the Hopper by hand, a penguin out among Floe's animals.
func ch_floe() -> void:
	say("new career: %s, unlocked %s" % [Career.money(Career.funds), Career.unlocked])
	if not await launch("hopper"):
		stuck = "the Hopper won't launch in a new career"
		return
	var before := Career.done.duplicate()
	science()
	await key(KEY_T)
	await key(KEY_Z)
	await key(KEY_SPACE)
	await until(func(): return float(tel().altitude) > 300.0, 60)
	science()
	var hi := {"top": 0.0}  # (a lambda keeps its own copy of a plain local)
	await until(func():
		hi.top = maxf(hi.top, float(tel().get("altitude", 0.0)))
		return float(tel().get("vertical_speed", 0.0)) < -5.0, 300)
	var top: float = hi.top
	say("the Hopper tops out at %.0f m; missions: %s" % [top, news(before)])
	await until(func(): return float(tel().get("altitude", 0.0)) < top - 200.0, 300)
	await key(KEY_P)
	var down := await until(func(): return tel().get("landed", false) or tel().get("situation", "") == "splashed", 600)
	var t := tel()
	say("down: %s, %s, worst contact %.1f m/s, alive %s" % [down, t.get("situation", ""), float(t.get("max_impact", 0.0)), t.get("alive", false)])
	if not t.get("alive", false):
		stuck = "the Hopper was lost coming down"
		return
	science()
	await frames(10)
	# a penguin out among the animals
	var craft := s.active
	await key(KEY_F)
	await frames(5)
	if not tel().get("eva", false):
		stuck = "F didn't let a penguin out"
		return
	await until(func(): return tel().get("landed", false), 30)
	science()
	map_life(tel().body_index, float(tel().lat_deg), float(tel().lon_deg), 15000.0)
	var seen := {}
	for round in 6:
		var best := {}
		for th in JSON.parse_string(s.core.things_near(s.active, 3000)):
			if str(th.get("what", "")) == "animal" and not seen.has(str(th.species)) and not ("floe/" + str(th.species)) in Career.catalogued:
				if best.is_empty() or float(th.distance) < float(best.distance):
					best = th
		if best.is_empty():
			break
		seen[str(best.species)] = true
		say("nearest new animal: %s, %.0f m off" % [best.species, float(best.distance)])
		if float(best.distance) > 1500.0:
			continue
		var got := await look_at(str(best.species), 900.0)
		say("a good look at the %s: %s; catalogued on Floe now %s" % [best.species, got, Career.catalogued_on("floe")])
	say("missions now: %s" % news(before))
	# back aboard and home
	var back := await board(craft)
	say("back at the Hopper: %s, aboard %s" % [back, not tel().get("eva", false)])
	if tel().get("eva", false):
		stuck = "the penguin couldn't get back in the Hopper"
		return
	await recover()
	unlock(["rocketry"])


## The computer flies: the Hopper up past a kilometre, down to 50 m and
## held there, and set down, by a script of the player's own (hover.py
## from the examples, changed: there's no Guidance Computer yet).
const HOP_PY := """# hop.py: up past a kilometre, back down to 50 m, held there, then down
lifter = ship.engines[0]
lifter.throttle = 100
aim("up")
while ship.height < 1150:
    sleep(0.1)
print("over a kilometre:", round(ship.height), "m")
lifter.throttle = 0
def fly(target, secs):
    t_in = -1
    while True:
        push = lifter.max_thrust * 1000 / ship.mass
        g = ship.body.gravity
        h = ship.height
        # a falling speed it can still stop from, short of the height
        room = max(h - target, 0)
        vt = max(-(2 * (push - g) * room) ** 0.5 * 0.6 - 1.5, min((target - h) * 0.5, 8))
        if target == 0:
            vt = max(vt, -1.5)
        lifter.throttle = clamp(100 * (g + (vt - ship.vertical_speed) * 1.5) / push, 0, 100)
        if abs(target - h) < 1.5 and t_in < 0:
            t_in = ship.time
        if t_in >= 0 and ship.time - t_in > secs:
            return
        if target == 0 and ship.landed:
            lifter.throttle = 0
            return
        sleep(0.1)
fly(50, 26)
print("held 50 m")
fly(0, 0)
print("down:", ship.fuel, "% fuel left")
"""


func ch_computer() -> void:
	if not await launch("hopper"):
		stuck = "no second Hopper"
		return
	var before := Career.done.duplicate()
	var r := await script_run("/root/scripts/hop.py", HOP_PY, 900)
	var t := tel()
	say("hop.py: %s; %s, %.1f m/s worst contact; missions %s" % [" / ".join(r.said), t.get("situation", ""), float(t.get("max_impact", 0)), news(before)])
	if not t.get("alive", false):
		stuck = "hop.py lost the Hopper"
		return
	await recover()


## Orbit: the Orbiter up by the example to_orbit.py, a spacewalk, and home
## by hand (retrograde, burn, drop the stage, chute).
func ch_orbit() -> void:
	unlock(["orbits"])
	if not await launch("orbiter"):
		stuck = "the Orbiter won't launch"
		return
	var before := Career.done.duplicate()
	science()
	var r := await script_run("/root/examples/to_orbit.py", "", 3000)
	var t := tel()
	var o: Dictionary = t.get("orbit", {})
	say("to_orbit.py: %s; orbit %.0f x %.0f km; missions %s" % [" / ".join(r.said.slice(-4)), float(o.get("pe_alt", 0)) / 1000.0, float(o.get("ap_alt", 0)) / 1000.0, news(before)])
	if not t.get("alive", false) or o.get("pe_alt") == null or float(o.pe_alt) < 45000.0:
		stuck = "to_orbit.py didn't make orbit"
		return
	await until(func(): return false, 30)
	science()
	# a spacewalk
	var craft := s.active
	await key(KEY_F)
	await frames(5)
	science()
	await until(func(): return false, 10)
	await key(KEY_F)
	await frames(5)
	say("spacewalk: back aboard %s; missions %s" % [s.active == craft, news(before)])
	await until(func(): return false, 2700, 1.0, 5)  # (round the far side: the space high over?)
	science()
	await home_by_hand()


## Down to Floe from orbit by hand: retrograde, burn the periapsis into the
## air, drop what's below the capsule, shield first, chute when slow.
func home_by_hand(burn := true) -> void:
	var before := Career.done.duplicate()
	scene._on_sas("retrograde")
	await until(func(): return off_retro() < 3.0, 90)
	await until(func(): return false, 20)
	if burn:
		await key(KEY_Z)
		await until(func(): return float(tel().get("orbit", {}).get("pe_alt", 1e9)) < 25000.0 or float(tel().get("thrust_max", 0)) <= 0.0, 300, 1.0)
		await key(KEY_X)
	var o: Dictionary = tel().get("orbit", {})
	say("deorbit burn: periapsis %.0f km" % [float(o.get("pe_alt", 0)) / 1000.0])
	# everything but the capsule goes
	while int(tel().get("next_stage", -1)) >= 0 and tel().get("crew_capacity", 0) > 0:
		var n0 := int(tel().get("parts", 0))
		await key(KEY_SPACE)
		await frames(5)
		if int(tel().get("parts", 0)) == n0:
			break
	scene._on_sas("retrograde")
	say("capsule alone: %d parts" % int(tel().get("parts", 0)))
	await until(func(): return tel().get("in_atmosphere", false), 2.0e6, 4.0, 7)
	var peak := {"g": 0.0, "heat": 0.0}
	await until(func():
		peak.g = maxf(peak.g, float(tel().get("g_force", 0.0)))
		return float(tel().get("surface_speed", 1e9)) < 250.0 and float(tel().get("altitude", 0)) < 20000.0, 2000, 2.0)
	await key(KEY_P)
	var down := await until(func(): return tel().get("landed", false) or str(tel().get("situation", "")) == "splashed", 2000)
	var t := tel()
	say("home: %s %s, peak %.1f g, alive %s; missions %s" % [down, t.get("situation", ""), peak.g, t.get("alive", false), news(before)])
	if not t.get("alive", false):
		stuck = "the capsule was lost coming home"
		return
	science()
	await recover()


## Degrees between the nose and retrograde (along the orbit).
func off_retro() -> float:
	var t := tel()
	return rad_to_deg(Frame.vec_arr(t.nose).angle_to(-Frame.vec_arr(t.velocity)))


## Krill: the Guidance Computer flies the Krill Lander there and down
## (the example krill.py); a penguin out, a sample, a flag; and home by a
## script of our own: up, the burn home, the lander dropped, the capsule
## down through the air.
const HOME_PY := """# home.py: off Krill and back to Floe
launch()
transfer("floe")
warp_to("floe")
print("at Floe:", round(get("periapsis") / 1000), "km periapsis")
"""


const TO_KRILL_PY := """# to_krill.py: into orbit round Krill
countdown(3)
launch(80000)
transfer("krill")
warp_to("krill")
capture(30000)
print("round Krill:", round(get("periapsis") / 1000), "x", round(get("apoapsis") / 1000), "km")
"""


func ch_krill() -> void:
	unlock(["staging", "landing", "planner", "guidance"])
	if not "guidance" in Career.unlocked:
		stuck = "not enough research for the Guidance Computer (%.0f)" % Career.research
		return
	if not await launch("krill_lander"):
		stuck = "the Krill Lander won't launch"
		return
	var before := Career.done.duplicate()
	var r := await script_run("/root/examples/krill.py", "", 4.0e6)
	var t := tel()
	say("krill.py: %s; %s on %s; missions %s" % [" / ".join(r.said.slice(-3)), t.get("situation", ""), t.get("body", ""), news(before)])
	if not (t.get("landed", false) and str(t.body) == "krill"):
		stuck = "krill.py didn't land on Krill"
		return
	await eva_walkabout(true)
	say("on Krill: missions %s" % news(before))
	await fly_home()
	unlock(["power", "listening"])


## Off the ground and home to Floe: up, the burn, the lander dropped, the
## capsule down through the air on its shield and chute.
func fly_home() -> void:
	var r := await script_run("/root/scripts/home.py", HOME_PY, 4.0e6)
	var t := tel()
	say("home.py: %s; %s at %s" % [" / ".join(r.said.slice(-3)), t.get("situation", ""), t.get("body", "")])
	if str(t.get("body", "")) != "floe":
		stuck = "home.py didn't get back to Floe"
		return
	await home_by_hand(false)


## The Hum and the Chime: the Krill Lander with a Signal Scanner (and the
## power to run it the whole way: a battery and a solar panel) round Krill
## until the scanner hears the Chime, then down by it, and a penguin up to it.
func ch_chime() -> void:
	unlock(["power", "listening"])
	if not Career.part_unlocked("signal_scanner") or not Career.part_unlocked("solar_panel"):
		stuck = "no scanner or solar panel researched (%.0f research)" % Career.research
		return
	var m := CraftModel.new()
	m.load_text(FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "krill_lander.craft.json"))
	m.attach_surface(CraftModel.held_single("signal_scanner"), 6, 0.0, 0.0, 1)
	m.attach_surface(CraftModel.held_single("battery"), 6, PI, 0.0, 1)
	m.attach_surface(CraftModel.held_single("solar_panel"), 5, PI * 0.5, 0.0, 2)
	if not await launch(m.save_text()):
		stuck = "the scanner lander won't launch"
		return
	var before := Career.done.duplicate()
	var r := await script_run("/root/scripts/to_krill.py", TO_KRILL_PY, 4.0e6)
	var t := tel()
	say("to_krill.py: %s; %s at %s, power %s (%.0f of %.0f kJ); missions %s" % [" / ".join(r.said.slice(-3)), t.get("situation", ""), t.get("body", ""),
		t.get("powered", false), float(t.get("ec", 0)), float(t.get("ec_capacity", 0)), news(before)])
	if str(t.get("body", "")) != "krill" or t.get("orbit", {}).get("ap_alt") == null:
		stuck = "to_krill.py didn't get into orbit round Krill"
		return
	# round and round until the scanner hears the Chime
	var heard := await until(func(): return "krill_chime" in Career.heard, float(t.orbit.period) * 3.0, 4.0, 3)
	t = tel()
	say("the Chime heard: %s (scanner powered %s); missions %s" % [heard, t.get("scanner_powered", false), news(before)])
	if not heard:
		stuck = "three times round Krill and the scanner never heard the Chime"
		return
	if await program("land near chime", 1.0e6) != "done":
		stuck = "couldn't land by the Chime"
		return
	t = tel()
	say("down on Krill %.0f m from the Chime" % float(Career.relic_in(t, "krill_chime").get("surface_distance", -1.0)))
	await eva_walkabout(false, "krill_chime")
	say("the Chime: missions %s" % news(before))
	await fly_home()


## A penguin out: down on the ground, every experiment, a flag if `flag`,
## a look round, and back in.
func eva_walkabout(flag: bool, relic := "") -> void:
	var craft := s.active
	await key(KEY_F)
	await frames(5)
	if not tel().get("eva", false):
		say("nobody came out")
		return
	await until(func(): return tel().get("landed", false), 30)
	science()
	if flag:
		await key(KEY_G)
		await frames(3)
	if relic != "":
		var got := await walk_to(func():
			var rr := Career.relic_in(tel(), relic)
			return bearing({"bearing": rr.get("bearing_deg", 0.0), "distance": rr.get("surface_distance", 0.0)}), 12.0, 1800.0)
		await until(func(): return false, 3)
		say("at %s: %s, %.0f m from it" % [relic, got, float(Career.relic_in(tel(), relic).get("surface_distance", -1))])
	await until(func(): return false, 5)
	await board(craft)
	say("walkabout: back aboard %s" % (s.active == craft))


## Kelp's Hearth: the Hearth Diver out to Kelp, round it until the scanner
## hears the Warm Tone, down by the Hearth on its engine through the thick
## air, and a penguin up to it. (One way: Kelp keeps the lander.)
func ch_hearth() -> void:
	await relic_trip("hearth_yard", "kelp", "kelp_hearth", "hearth", 250000.0)


## Narwhal's Keel, the same way.
func ch_keel() -> void:
	await relic_trip("hearth_yard", "narwhal", "narwhal_keel", "keel", 150000.0)


## Out to a relic and down by it. Built in the Orbital Yard (round Floe
## already: a lander for thick air, with a stage for a full capture under
## its heat shield, is too big to lift from the pad and still get there).
func relic_trip(craft: String, body: String, relic: String, word: String, park: float) -> void:
	unlock(["heavy", "staging", "power", "listening", "landing", "habitation", "yard"])
	var m := design(craft)
	if not Career.site_unlocked("orbit"):
		stuck = "no Orbital Yard (research %.0f)" % Career.research
		return
	if not await launch(m.save_text(), "orbit"):
		stuck = "the %s won't launch" % m.craft_name
		return
	var before := Career.done.duplicate()
	var go := """# out to %s
transfer("%s")
warp_to("%s")
capture(%d)
print("round %s:", round(get("periapsis") / 1000), "x", round(get("apoapsis") / 1000), "km")
""" % [body, body, body, int(park), body]
	var r := await script_run("/root/scripts/out.py", go, 2.0e7)
	var t := tel()
	say("out.py: %s; %s at %s, %.0f m/s left; missions %s" % [" / ".join(r.said.slice(-3)), t.get("situation", ""), t.get("body", ""),
		float(t.get("dv_total", 0)), news(before)])
	if str(t.get("body", "")) != body or t.get("orbit", {}).get("ap_alt") == null:
		stuck = "out.py didn't get into orbit round %s" % body
		return
	var t_listen := s.time()
	var heard := relic in Career.heard or await until(func(): return relic in Career.heard, float(t.orbit.period) * 40.0, 4.0, 4)
	say("%s heard: %s, after %s round %s; missions %s" % [relic, heard, FlightPrograms.span(s.time() - t_listen), body, news(before)])
	if not heard:
		stuck = "forty times round %s and the scanner never heard %s" % [body, relic]
		return
	if not await land_by(word, relic):
		return
	t = tel()
	say("down on %s %.0f m from %s, %.0f m/s left" % [body, float(Career.relic_in(t, relic).get("surface_distance", -1.0)), relic, float(t.get("dv_total", 0))])
	await eva_walkabout(true, relic)
	say("%s: missions %s; unlocked %s" % [relic, news(before), Career.unlocked])


func ch_lantern() -> void:
	unlock(["flight", "shapes", "habitation", "glasswing", "torch", "lasers", "mining"])
	var m := design("star_skua")
	if not await launch(m.save_text()):
		stuck = "the Star Skua won't launch (research %.0f, unlocked %s)" % [Career.research, Career.unlocked]
		return
	var r := await script_run("/root/scripts/up.py", "countdown(3)\nlaunch(120000)\n", 3000)
	say("up.py: %s; %s at %s" % [" / ".join(r.said.slice(-2)), tel().get("situation", ""), tel().get("body", "")])
	if tel().get("orbit", {}).get("pe_alt") == null or float(tel().orbit.pe_alt) < 70000.0:
		stuck = "the Star Skua didn't make orbit"
		return
	await star_hop("lantern")


func ch_ember() -> void:
	await star_hop("ember")


func ch_brine() -> void:
	await star_hop("brine")


func ch_marrow() -> void:
	await star_hop("marrow")


func ch_tarn() -> void:
	await star_hop("tarn")


func ch_hollow() -> void:
	await star_hop("hollow")


## Landed: the drills and refineries on until the tanks are full (or as
## full as two days on this world's ore make them), warping meanwhile.
func refuel() -> void:
	var t := tel()
	if int(t.get("drills", 0)) <= 0:
		return
	var l0 := float(t.get("liquid", 0.0))
	scene.computer.sh("ship mining on")
	var t0 := s.time()
	var full := await until(func(): return float(tel().get("liquid", 0.0)) >= float(tel().get("liquid_capacity", 1.0)) * 0.99, 2.0 * 86400.0, 1.0, 5)
	scene.computer.sh("ship mining off")
	# (out of the warp the legs take the weight again: on the ground before it goes)
	await until(func(): return tel().get("landed", false), 30.0)
	t = tel()
	say("refuelled on %s (ore %.2f): %.0f kg to %.0f of %.0f kg liquid in %s, %s; drills touching %s" % [t.get("body", "?"), float(t.get("ore_abundance", 0.0)), l0,
		float(t.get("liquid", 0.0)), float(t.get("liquid_capacity", 0.0)), FlightPrograms.span(s.time() - t0), "full" if full else "not full", t.get("drills_touching", "?")])


## The ship (wherever it is: on a world, or round one) on to the next star
## of the thread and down by its relic.
func star_hop(key: String) -> void:
	var star: String = THREAD[key][0]
	var world: String = THREAD[key][1]
	var relic: String = THREAD[key][2]
	var word: String = THREAD[key][3]
	if scene == null or not is_instance_valid(scene):
		await resume_flight()
	var before := Career.done.duplicate()
	# (on its legs, if they bounce it a hair off the ground just now)
	var on_ground := func() -> bool:
		var tt := tel()
		return tt.get("landed", false) or (int(tt.get("contacts", 0)) > 0 and float(tt.get("surface_speed", 99.0)) < 2.0)
	await until(func(): return on_ground.call() or float(tel().get("altitude_terrain", 0.0)) > 100.0, 10.0)
	var t := tel()
	if on_ground.call():
		await refuel()
		var r := {}
		for attempt in 2:
			await until(on_ground, 30.0)
			var tb := tel()
			r = await script_run("/root/scripts/up.py", "launch()\n", 3000)
			say("up from %s: %s (before it: %s, landed %s, contacts %s, %.2f m/s)" % [t.body, " / ".join(r.said.slice(-2)), tb.get("situation", "?"), tb.get("landed"),
				tb.get("contacts"), float(tb.get("surface_speed", 0))])
			if not on_ground.call():
				break
		if on_ground.call():
			stuck = "couldn't launch from %s (%s)" % [t.body, tel().get("situation", "?")]
			return
	# out of this world's pull first (straight at the star could be straight
	# into the world), along the orbit on the drive
	var core: TrsSim = s.core
	var star_here := int(tel().get("star", -1))
	if int(tel().body_index) != star_here:
		core.vessel_set_sas(s.active, "prograde", PackedFloat64Array())
		await until(func(): return Frame.vec_arr(tel().nose).angle_to(Frame.vec_arr(tel().velocity)) < deg_to_rad(5.0), 120)
		core.vessel_set_drive(s.active, 1.0)
		var from_body := int(tel().body_index)
		var free := await until(func():
			var tt := tel()
			if not tt.get("alive", false):
				return true
			# (a moon in the way: straight out from it, not along round it)
			core.vessel_set_sas(s.active, "prograde" if int(tt.body_index) == from_body else "radial_out", PackedFloat64Array())
			return int(tt.body_index) == star_here, 1.0e6, 1.0, 5)
		core.vessel_set_drive(s.active, 0.0)
		say("out of %s's pull on the drive: %s" % [t.get("body", "?"), free])
	# out past this star's heliopause on the drive, under warp
	var si := core.body_find(star)
	var sg: Array = JSON.parse_string(core.body_info(si)).galactic
	var to := func() -> Vector3:
		var g: Array = tel().galactic
		return (Vector3(sg[0], sg[1], sg[2]) - Vector3(g[0], g[1], g[2])).normalized()
	var aim: Vector3 = to.call()
	core.vessel_set_sas(s.active, "target", PackedFloat64Array([aim.x, aim.y, aim.z]))
	await until(func(): return Frame.vec_arr(tel().nose).angle_to(aim) < deg_to_rad(3.0), 120)
	core.vessel_set_drive(s.active, 1.0)
	var out := await until(func(): return tel().get("interstellar", false), 1.0e7, 1.0, 7)
	core.vessel_set_drive(s.active, 0.0)
	say("out past %s's heliopause: %s (%.0f km/s)" % [tel().get("star_name", "?"), out, Frame.vec_arr(tel().get("velocity_galactic", [0, 0, 0])).length() / 1000.0])
	if not out:
		stuck = "the drive didn't take the ship out past the heliopause"
		return
	# the COURSE panel's button for the star
	await frames(3)
	var btn: Button = null
	for b in scene._course.get_child(0).get_children():
		if b is Button and b.text.to_lower().contains(star):
			btn = b
	if btn == null:
		stuck = "no COURSE button for %s" % star
		return
	btn.pressed.emit()
	await frames(2)
	say("course: %s" % scene.hud._toast.text)
	var arrived := await until(func(): return not tel().has("course"), 1.0e10, 1.0, 7)
	t = tel()
	say("course done (%s): %s, ship clock behind by %.2f yr" % [arrived, scene.hud._clock.text, float(t.get("tau_lag", 0)) / 31557600.0])
	# a push toward the star, and drift in through its heliopause
	aim = to.call()
	core.vessel_set_sas(s.active, "target", PackedFloat64Array([aim.x, aim.y, aim.z]))
	await until(func(): return Frame.vec_arr(tel().nose).angle_to(aim) < deg_to_rad(3.0), 120)
	core.vessel_set_drive(s.active, 1.0)
	# (no faster than the drive can take off again inside: 1 g there; at
	# 100 g out here that is under a minute of push)
	var push_t := s.time()
	await until(func(): return float(tel().get("proper_speed", 0.0)) > 50000.0 or not tel().get("interstellar", false) or s.time() - push_t > 60.0, 120, 1.0)
	core.vessel_set_drive(s.active, 0.0)
	say("pushed in: %.1f km/s toward %s" % [float(tel().get("proper_speed", 0.0)) / 1000.0, star])
	var inside := await until(func(): return not tel().get("interstellar", false), 1.0e8, 1.0, 7)
	say("in through %s's heliopause: %s, %.2f Gm out at %.1f km/s; missions %s" % [star, inside, Frame.vec_arr(tel().position).length() / 1e9,
		Frame.vec_arr(tel().velocity).length() / 1000.0, news(before)])
	if not inside:
		stuck = "never drifted in through %s's heliopause" % star
		return
	# (a little way in before the drive goes on: on the line itself, any
	# push outward is out again, where the drive pushes 100 g)
	var hp: float = float(JSON.parse_string(core.body_info(si)).soi)
	var deep := await until(func(): return Frame.vec_arr(tel().position).length() < hp * 0.95, 1.0e8, 1.0, 7)
	t = tel()
	say("a way in (%s): %.2f Gm from %s (heliopause %.1f Gm), %.1f km/s against it, %s, interstellar %s" % [deep, Frame.vec_arr(t.position).length() / 1e9,
		t.get("body", "?"), hp / 1e9, Frame.vec_arr(t.velocity).length() / 1000.0, t.get("situation", ""), t.get("interstellar", false)])
	if await program("drive_to " + world, 1.0e8) != "done":
		stuck = "drive_to %s didn't get there" % world
		return
	t = tel()
	var t_listen := s.time()
	var heard := relic in Career.heard or await until(func(): return relic in Career.heard, float(t.orbit.period) * 40.0, 4.0, 4)
	say("%s heard: %s, after %s round %s; missions %s" % [relic, heard, FlightPrograms.span(s.time() - t_listen), world, news(before)])
	if not heard:
		stuck = "forty times round %s and never heard %s" % [world, relic]
		return
	if not await land_by(word, relic):
		return
	t = tel()
	say("down on %s %.0f m from %s, %.0f m/s left" % [world, float(Career.relic_in(t, relic).get("surface_distance", -1.0)), relic, float(t.get("dv_total", 0))])
	await eva_walkabout(true, relic)
	say("%s: missions %s" % [relic, news(before)])


## Back aboard the craft a chapter left flying (or standing) somewhere.
func resume_flight() -> void:
	await _leave_scene()
	var best := -1
	for v in Sim.vessels():
		if not (v.crew_names as Array).is_empty():
			best = int(v.id)
	Sim.pending_vessel = best
	scene = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	tree.current_scene = scene
	await frames(5)
	scene.hud.hide_help()
	s = scene.session
	if not s.sim_event.is_connected(_on_event):
		s.sim_event.connect(_on_event)
	say("back aboard %s: %s over %s" % [tel().get("name", "?"), tel().get("situation", ""), tel().get("body", "")])


## `land near <word>`, the way a careful player flies it: quicksaved
## first (F5), and on a bad landing loaded again (F9) and flown again.
func land_by(word: String, relic: String) -> bool:
	await key(KEY_F5)
	await frames(3)
	for attempt in 3:
		if await program("land near " + word, 1.0e6) == "done":
			# (down, but on its side or far off counts as gone wrong too)
			var off := float(Career.relic_in(tel(), relic).get("surface_distance", 1e9))
			var upright: bool = not scene.computer.programs.note.contains("NOT upright")
			if upright and off < 500.0 and tel().get("alive", false):
				return true
			say("   down %.0f m from %s, %s" % [off, relic, "upright" if upright else "on its side"])
		say("   the landing by %s went wrong (try %d): back to the quicksave" % [relic, attempt + 1])
		await key(KEY_F9)
		await frames(10)
	stuck = "couldn't land by %s in three tries" % relic
	return false


## Runs a script on the ship's computer to its end (as the Script Editor's
## Run would), the flight going on meanwhile.
func script_run(path: String, text: String, max_s: float) -> Dictionary:
	var comp: ShipComputer = scene.computer
	if text != "":
		comp.machine.write_file(path, text)
	comp.log.clear()
	var why := str(comp.machine.start(path))
	if why != "":
		say("script %s won't start: %s" % [path, why])
		return {"said": [why]}
	await frames(30)
	await until(func(): return _scripts(comp).is_empty(), max_s)
	var said := []
	for line in comp.log:
		if not str(line).begins_with(">"):
			said.append(str(line))
	return {"said": said}


## The pids of scripts still running on the computer.
static func _scripts(comp: ShipComputer) -> Array:
	var live := {}
	for row in str(comp.machine.scripts()).split("\n", false):
		var g := row.split("\t")
		if g.size() >= 4 and g[1] == "1" and g[3] != "-i":
			live[g[0]] = true
	var pids := []
	for row in str(comp.sh("ps")).split("\n", false):
		var f := row.strip_edges().split(" ", false)
		if f.size() >= 6 and f[2] == "running" and f[f.size() - 1] in ["/bin/py", "/bin/sh"] and f[1] == "1" and live.has(f[0]):
			pids.append(f[0])
	return pids


## What a map of the wild would show: how far the nearest herd of each
## kind of animal is from (lat, lon), looking out to `range` m.
func map_life(bi: int, lat: float, lon: float, range: float) -> void:
	var info: Dictionary = JSON.parse_string(s.core.body_info(bi))
	var R: float = info.radius
	var la := deg_to_rad(lat)
	var lo := deg_to_rad(lon)
	var dir := Vector3(cos(la) * cos(lo), cos(la) * sin(lo), sin(la))
	var kinds: Array = Sim.life(str(info.id))
	var mask := 0
	for k in kinds.size():
		mask |= (1 << k) if kinds[k].get("animal", false) else 0
	var best := {}
	# (life_cells keeps 512 cells, nearest not first: small looks, tiled)
	var cell := R * PI / 2.0 / pow(2.0, round(log(R * PI / 2.0 / 400.0) / log(2.0)))
	var q := cell * 6.0
	var e := Vector3(0, 0, 1).cross(dir).normalized()
	var nn := dir.cross(e)
	var done_cells := {}
	var steps := int(ceil(range / q))
	for gx in range(-steps, steps + 1):
		for gy in range(-steps, steps + 1):
			var off := Vector2(gx, gy) * q * 1.4
			if off.length() > range + q:
				continue
			var cdir := (dir + (e * off.x + nn * off.y) / R).normalized()
			var cells: PackedFloat64Array = s.core.life_cells(bi, PackedFloat64Array([cdir.x * R, cdir.y * R, cdir.z * R]), q)
			for c in cells.size() / 4:
				var ck := "%d/%d/%d/%d" % [cells[c * 4], cells[c * 4 + 1], cells[c * 4 + 2], cells[c * 4 + 3]]
				if done_cells.has(ck):
					continue
				done_cells[ck] = true
				var d: PackedFloat64Array = s.core.life_cell(bi, PackedFloat64Array([cells[c * 4], cells[c * 4 + 1], cells[c * 4 + 2], cells[c * 4 + 3]]), mask)
				var cen := Vector3(d[0], d[1], d[2])
				var k := 4
				for i in int(d[3]):
					var kind := int(d[k])
					var at := cen + Vector3(d[k + 1], d[k + 2], d[k + 3])
					var dist := R * at.normalized().angle_to(dir)
					var id := str(kinds[kind].id)
					if dist < float(best.get(id, INF)):
						best[id] = dist
					k += 10 + 3 * int(d[k + 9])
	var out := []
	for k in kinds:
		if k.get("animal", false):
			out.append("%s %s" % [k.id, "%.1f km" % (float(best[k.id]) / 1000.0) if best.has(k.id) else "none in %.0f km" % (range / 1000.0)])
	say("map of the wild here: nearest herds: " + ", ".join(out))


static func _angle(la1: float, lo1: float, la2: float, lo2: float) -> float:
	var a := Vector3(cos(deg_to_rad(la1)) * cos(deg_to_rad(lo1)), cos(deg_to_rad(la1)) * sin(deg_to_rad(lo1)), sin(deg_to_rad(la1)))
	var b := Vector3(cos(deg_to_rad(la2)) * cos(deg_to_rad(lo2)), cos(deg_to_rad(la2)) * sin(deg_to_rad(lo2)), sin(deg_to_rad(la2)))
	return rad_to_deg(a.angle_to(b))
