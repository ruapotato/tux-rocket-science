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


func _ready() -> void:
	theme = HangarTheme.make()
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	for c in get_children():
		c.queue_free()
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
	box.add_theme_constant_override("separation", 10)
	add_child(box)
	Sim.tutorial = false
	for item in [["First flight (tutorial)", _first_flight], ["Assembly Hangar", _hangar], ["Quick launch: Orbiter", _quick], ["Astronaut Corps", _toggle_corps], ["Quit", func(): get_tree().quit()]]:
		var b := Button.new()
		b.text = item[0]
		b.custom_minimum_size = Vector2(340, 50)
		b.add_theme_font_size_override("font_size", 19)
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
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	get_tree().change_scene_to_file("res://scenes/flight.tscn")


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
