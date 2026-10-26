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
## PASA main menu: Floe turning in the sun, Krill swinging past, the insignia,
## and the way into the hangar, a quick launch, or the astronaut corps.
extends Control

var _floe: MeshInstance3D
var _krill: MeshInstance3D
var _t := 0.0
var _corps: PanelContainer
var _atmo: MeshInstance3D
var _sun_node: DirectionalLight3D
var _flights: PanelContainer


func _ready() -> void:
	theme = HangarTheme.make()
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	for c in get_children():
		c.queue_free()
	_flights = null
	_corps = null
	_build_space()
	var logo := PasaLogo.new()
	logo.custom_minimum_size = Vector2(250, 250)
	logo.set_anchors_and_offsets_preset(Control.PRESET_TOP_LEFT)
	logo.position = Vector2(80, 70)
	logo.size = Vector2(250, 250)
	add_child(logo)
	var title := Label.new()
	title.text = "TUX ROCKET SCIENCE"
	title.position = Vector2(84, 335)
	title.add_theme_font_size_override("font_size", 54)
	title.add_theme_color_override("font_color", Color(1, 1, 1))
	title.add_theme_color_override("font_outline_color", Color(0.02, 0.03, 0.08))
	title.add_theme_constant_override("outline_size", 10)
	add_child(title)
	var sub := Label.new()
	sub.text = "Penguin Aeronautics & Space Administration"
	sub.position = Vector2(88, 405)
	sub.add_theme_font_size_override("font_size", 20)
	sub.add_theme_color_override("font_color", PartVisual.ORANGE)
	add_child(sub)
	var box := VBoxContainer.new()
	box.position = Vector2(88, 460)
	box.add_theme_constant_override("separation", 6)
	add_child(box)
	Sim.tutorial = false
	if not Sim.program_chosen:
		Sim.use_program(Sim.last_program())
	var in_flight := Sim.game_session().core.vessel_list() != "[]"
	var other := "sandbox" if Career.is_career() else "career"
	var items := [["Game: %s  (switch to %s)" % [Career.mode.capitalize(), other.capitalize()], func(): _switch_program(other)],
		["First flight (tutorial)", _first_flight], ["Assembly Hangar", _hangar], ["Quick launch: Orbiter", _quick]]
	if in_flight:
		items.append(["Craft in flight (%d)" % Sim.vessels().size(), _toggle_flights])
	items.append_array([["Mission Control", func(): _show_program("missions")], ["R&D", func(): _show_program("rnd")],
		["Astronaut Corps", _toggle_corps], ["Quit", func(): get_tree().quit()]])
	for item in items:
		var b := Button.new()
		b.text = item[0]
		b.custom_minimum_size = Vector2(340, 38)
		b.add_theme_font_size_override("font_size", 17)
		b.pressed.connect(item[1])
		box.add_child(b)
	var foot := Label.new()
	foot.text = "trs_core %s · AGPL-3.0 · Tux lineage: Larry Ewing (our penguins are our own) · Made with Godot" % Sim.core.version()
	foot.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_LEFT)
	foot.offset_left = 20
	foot.offset_top = -30
	foot.offset_bottom = -10
	foot.add_theme_font_size_override("font_size", 12)
	foot.add_theme_color_override("font_color", Color(0.6, 0.68, 0.8))
	add_child(foot)
	if Sim.last_mission_note != "":
		var note := Label.new()
		note.text = Sim.last_mission_note
		note.position = Vector2(88, 790)
		note.add_theme_color_override("font_color", Color(0.7, 1.0, 0.75))
		add_child(note)


func _build_space() -> void:
	var svc := SubViewportContainer.new()
	svc.stretch = true
	svc.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	svc.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(svc)
	var vp := SubViewport.new()
	vp.own_world_3d = true
	vp.msaa_3d = Viewport.MSAA_4X
	svc.add_child(vp)
	var we := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.01, 0.012, 0.03)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.08, 0.09, 0.12)
	env.glow_enabled = true
	env.glow_intensity = 0.25
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	env.glow_hdr_threshold = 1.4
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	we.environment = env
	vp.add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation = Vector3(-0.3, -1.1, 0)
	sun.light_energy = 1.05
	vp.add_child(sun)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 0, 14)
	cam.fov = 40
	cam.far = 2000
	vp.add_child(cam)
	_floe = MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 4.2
	sm.height = 8.4
	sm.radial_segments = 128
	sm.rings = 64
	_floe.mesh = sm
	_floe.material_override = PlanetBake.planet_material(Sim.core, Sim.core.body_find("floe"))
	var clouds := MeshInstance3D.new()
	var cmesh := SphereMesh.new()
	cmesh.radius = 4.2 * (607.0 / 600.0)
	cmesh.height = cmesh.radius * 2.0
	cmesh.radial_segments = 128
	cmesh.rings = 64
	clouds.mesh = cmesh
	clouds.material_override = PlanetBake.cloud_material()
	_floe.add_child(clouds)
	_floe.position = Vector3(4.5, -1.6, 0)
	vp.add_child(_floe)
	var atmo := Atmosphere.make(600000.0, 70000.0, 5800.0)
	atmo.scale = Vector3.ONE * 4.2 * (670.0 / 600.0)
	atmo.position = _floe.position
	Atmosphere.set_sun(atmo, sun.global_transform.basis.z if sun.is_inside_tree() else Vector3(0.6, 0.3, 0.75).normalized())
	vp.add_child(atmo)
	Atmosphere.set_intensity(atmo, 0.75)
	_atmo = atmo
	_sun_node = sun
	_krill = MeshInstance3D.new()
	var km := SphereMesh.new()
	km.radius = 0.7
	km.height = 1.4
	_krill.mesh = km
	_krill.material_override = PlanetBake.planet_material(Sim.core, Sim.core.body_find("krill"))
	vp.add_child(_krill)
	var stars := ScaledSpace._starfield()
	stars.scale = Vector3.ONE * 0.012
	vp.add_child(stars)


func _process(delta: float) -> void:
	_t += delta
	if _floe:
		_floe.rotation.y = _t * 0.05
		_krill.position = _floe.position + Vector3(cos(_t * 0.12) * 8.0, 1.4 + sin(_t * 0.12) * 0.6, sin(_t * 0.12) * 5.0)
		_krill.rotation.y = _t * 0.12
		Atmosphere.set_sun(_atmo, _sun_node.global_transform.basis.z)


func _hangar() -> void:
	get_tree().change_scene_to_file("res://scenes/hangar.tscn")


func _first_flight() -> void:
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "hopper.craft.json")
	Sim.tutorial = true
	get_tree().change_scene_to_file("res://scenes/flight.tscn")


func _quick() -> void:
	var text := FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	var why := Career.launch_refusal(text)
	if why != "":
		_note("Can't launch the Orbiter: " + why)
		return
	Sim.pending_craft_text = text
	get_tree().change_scene_to_file("res://scenes/flight.tscn")


func _switch_program(kind: String) -> void:
	Sim.use_program(kind)
	_ready()


var _program: PanelContainer
var _program_kind := ""


## Mission Control or R&D (pressing it again closes it).
func _show_program(kind: String) -> void:
	if _program:
		_program.queue_free()
		_program = null
		if _program_kind == kind:
			_program_kind = ""
			return
	if _corps:
		_toggle_corps()
	if _flights:
		_toggle_flights()
	_program_kind = kind
	_program = ProgramPanels.missions() if kind == "missions" else ProgramPanels.rnd(func(): _program_kind = ""; _show_program(kind))
	_program.position = Vector2(470, 80)
	add_child(_program)


func _note(text: String) -> void:
	var l := Label.new()
	l.text = text
	l.position = Vector2(88, 820)
	l.add_theme_color_override("font_color", Color(1.0, 0.6, 0.5))
	add_child(l)
	get_tree().create_timer(4.0).timeout.connect(l.queue_free)


## Craft left flying (or landed) in the world: take one over, bring one
## home, or clear away debris.
func _toggle_flights() -> void:
	if _flights:
		_flights.queue_free()
		_flights = null
		return
	if _corps:
		_toggle_corps()
	_flights = PanelContainer.new()
	_flights.position = Vector2(470, 120)
	_flights.custom_minimum_size = Vector2(620, 0)
	add_child(_flights)
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 6)
	_flights.add_child(v)
	var title := Label.new()
	title.text = "CRAFT IN FLIGHT"
	title.add_theme_color_override("font_color", PartVisual.ORANGE)
	title.add_theme_font_size_override("font_size", 18)
	v.add_child(title)
	var all := Sim.vessels()
	var debris := all.filter(func(x): return not x.get("control", true) and (x.crew_names as Array).is_empty() and not x.get("anchored", false))
	if debris.size() >= 2:
		var sweep := Button.new()
		sweep.text = "Scrap all debris (%d)" % debris.size()
		sweep.tooltip_text = "Spent stages and wreckage: nothing aboard that can steer and nobody aboard."
		sweep.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
		sweep.pressed.connect(func():
			for x in debris:
				Sim.game.core.vessel_remove(int(x.id))
			Sim.save_game()
			_toggle_flights()
			_toggle_flights())
		v.add_child(sweep)
	# a long list scrolls instead of running off the screen
	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.custom_minimum_size = Vector2(600, minf(all.size() * 52.0, 520.0))
	v.add_child(scroll)
	var list := VBoxContainer.new()
	list.add_theme_constant_override("separation", 6)
	list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(list)
	for vs in all:
		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 8)
		var l := Label.new()
		var crew: Array = vs.crew_names
		l.text = "%s\n%s · %s%s" % [vs.name, str(vs.situation).replace("_", " "), str(vs.body).capitalize(),
			("   crew: " + ", ".join(crew)) if not crew.is_empty() else ("   %d part%s" % [int(vs.get("parts", 0)), "" if int(vs.get("parts", 0)) == 1 else "s"])]
		l.custom_minimum_size = Vector2(380, 0)
		l.add_theme_font_size_override("font_size", 14)
		row.add_child(l)
		var id := int(vs.id)
		var fly := Button.new()
		fly.text = "Fly"
		fly.pressed.connect(func():
			Sim.pending_vessel = id
			get_tree().change_scene_to_file("res://scenes/flight.tscn"))
		row.add_child(fly)
		var home: bool = vs.body == "floe" and vs.situation in ["landed", "prelaunch"]
		if home:
			var rec := Button.new()
			rec.text = "Recover"
			rec.tooltip_text = "Bring it home: the crew are back in the corps."
			rec.pressed.connect(func():
				var names := Sim.recover_vessel(id, {})
				Sim.save_game()
				Sim.last_mission_note = "Recovered %s." % (", ".join(names) if not names.is_empty() else vs.name)
				_ready())
			row.add_child(rec)
		elif crew.is_empty():
			var scrap := Button.new()
			scrap.text = "Scrap"
			scrap.tooltip_text = "Nobody aboard: remove it from the world."
			scrap.pressed.connect(func():
				Sim.game.core.vessel_remove(id)
				Sim.save_game()
				_toggle_flights()
				_toggle_flights())
			row.add_child(scrap)
		list.add_child(row)


func _toggle_corps() -> void:
	if _corps:
		_corps.queue_free()
		_corps = null
		return
	_corps = PanelContainer.new()
	_corps.set_anchors_and_offsets_preset(Control.PRESET_CENTER_RIGHT)
	_corps.position = Vector2(size.x - 760, 60)
	_corps.custom_minimum_size = Vector2(720, 0)
	add_child(_corps)
	var v := VBoxContainer.new()
	_corps.add_child(v)
	var h := Label.new()
	h.text = "PASA ASTRONAUT CORPS"
	h.add_theme_color_override("font_color", PartVisual.ORANGE)
	v.add_child(h)
	var grid := GridContainer.new()
	grid.columns = 4
	grid.add_theme_constant_override("h_separation", 10)
	grid.add_theme_constant_override("v_separation", 10)
	v.add_child(grid)
	for c in Crew.crew:
		var cell := VBoxContainer.new()
		var p := PenguinPortrait.new()
		p.custom_minimum_size = Vector2(160, 170)
		p.set_member(c)
		cell.add_child(p)
		var info := Label.new()
		var r: Dictionary = c.records
		info.text = "%s\nflights %d · best %s\nKrill landings %d\nnerve %d%% · glee %d%%" % [
			c.status.capitalize(), c.flights, FlightHUD.fmt_dist(r.max_altitude), r.krill_landings, int(c.nerve * 100), int(c.glee * 100)]
		info.add_theme_font_size_override("font_size", 11)
		cell.add_child(info)
		grid.add_child(cell)
		var idle := {"g_force": 1.0, "landed": true, "altitude": 0.0, "atmosphere_height": 70000.0}
		p.set_process(true)
		p.ready.connect(func(): p.update_state(idle, 0.0, 0.016))
		var timer := Timer.new()
		timer.wait_time = 1.0 / 30.0
		timer.autostart = true
		timer.timeout.connect(func(): p.update_state(idle, 0.0, 1.0 / 30.0))
		cell.add_child(timer)
	var rec := Button.new()
	rec.text = "Recruit a new penguin"
	rec.pressed.connect(func(): Crew.recruit_random(); _toggle_corps(); _toggle_corps())
	v.add_child(rec)
