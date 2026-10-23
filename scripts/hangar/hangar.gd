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
## PASA Assembly Hangar: the rocket editor UI. All editing goes through
## CraftModel; all numbers shown come from trs_core via CraftModel.stats().
extends Node3D

const SYMMETRY_STEPS := [1, 2, 3, 4, 6]
const SNAP_PX := 46.0
const ROT_STEP := deg_to_rad(15.0)
const CATEGORIES := ["pods", "tanks", "engines", "structural", "aero", "utility", "control"]
const CATEGORY_NAMES := {
	"pods": "Pods", "tanks": "Tanks", "engines": "Engines", "structural": "Structure",
	"aero": "Aero", "utility": "Utility", "control": "Control",
}

var model := CraftModel.new()
var symmetry := 1

var _craft_root: Node3D
var _part_nodes: Array[Node3D] = []
var _cam: Camera3D
var _cam_yaw := 0.6
var _cam_pitch := 0.25
var _cam_dist := 22.0
var _cam_target := Vector3(0, 6, 0)
var _orbiting := false
var _panning := false

var _held: Dictionary = {}
var _held_spin := 0.0
var _held_extra: Array = DMath.q_identity()
var _ghosts: Array[Node3D] = []
var _snap: Dictionary = {}
var _hover := -1
var _stats: Dictionary = {}

# UI
var _ui: Control
var _name_edit: LineEdit
var _sym_button: Button
var _undo_button: Button
var _redo_button: Button
var _summary: RichTextLabel
var _stage_box: VBoxContainer
var _hint: Label
var _toast: Label
var _load_menu: PopupMenu
var _parts_tabs: TabContainer


func _ready() -> void:
	_build_world()
	_build_ui()
	model.changed.connect(_on_model_changed)
	if Sim.pending_craft_text != "":
		model.load_text(Sim.pending_craft_text)
	else:
		model.load_file(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	_on_model_changed()
	_frame_craft()


## Points the camera at the whole craft.
func _frame_craft() -> void:
	var lo := INF
	var hi := -INF
	for v in _part_nodes:
		lo = minf(lo, v.global_position.y)
		hi = maxf(hi, v.global_position.y)
	if lo == INF:
		lo = 0.0
		hi = 2.0
	_cam_target = Vector3(0, (lo + hi) * 0.5, 0)
	_cam_dist = maxf(8.0, (hi - lo) * 1.1 + 5.0)
	_update_camera()


# ---- world --------------------------------------------------------------------

func _build_world() -> void:
	var env := WorldEnvironment.new()
	var e := Environment.new()
	e.background_mode = Environment.BG_COLOR
	e.background_color = Color(0.05, 0.07, 0.11)
	e.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	e.ambient_light_color = Color(0.55, 0.62, 0.75)
	e.ambient_light_energy = 0.55
	e.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	e.ssao_enabled = true
	e.glow_enabled = true
	env.environment = e
	add_child(env)

	var sun := DirectionalLight3D.new()
	sun.rotation = Vector3(deg_to_rad(-55), deg_to_rad(35), 0)
	sun.light_energy = 1.25
	sun.shadow_enabled = true
	add_child(sun)
	var fill := OmniLight3D.new()
	fill.position = Vector3(-10, 14, 12)
	fill.omni_range = 60
	fill.light_energy = 0.8
	fill.light_color = Color(0.75, 0.85, 1.0)
	add_child(fill)

	# hangar floor with painted pad markings
	var floor_mesh := PlaneMesh.new()
	floor_mesh.size = Vector2(80, 80)
	var floor_mi := MeshInstance3D.new()
	floor_mi.mesh = floor_mesh
	floor_mi.material_override = PartVisual.mat("floor", Color(0.16, 0.18, 0.21), 0.0, 0.85)
	add_child(floor_mi)
	var ring := TorusMesh.new()
	ring.inner_radius = 3.6
	ring.outer_radius = 3.9
	var ring_mi := MeshInstance3D.new()
	ring_mi.mesh = ring
	ring_mi.scale = Vector3(1, 0.02, 1)
	ring_mi.material_override = PartVisual.mat("pad_ring", PartVisual.ORANGE, 0.0, 0.6)
	add_child(ring_mi)

	# back wall and banner
	var wall := MeshInstance3D.new()
	var wall_mesh := BoxMesh.new()
	wall_mesh.size = Vector3(80, 40, 1)
	wall.mesh = wall_mesh
	wall.position = Vector3(0, 20, -22)
	wall.material_override = PartVisual.mat("wall", Color(0.11, 0.13, 0.17), 0.1, 0.8)
	add_child(wall)
	var banner := Label3D.new()
	banner.text = "PASA  ASSEMBLY  HANGAR"
	banner.font_size = 160
	banner.pixel_size = 0.01
	banner.modulate = Color(0.85, 0.9, 1.0, 0.9)
	banner.position = Vector3(0, 13, -21.4)
	add_child(banner)
	var motto := Label3D.new()
	motto.text = "Penguin Aeronautics & Space Administration  —  ad astra per glaciem"
	motto.font_size = 56
	motto.pixel_size = 0.01
	motto.modulate = Color(1.0, 0.6, 0.25, 0.9)
	motto.position = Vector3(0, 11.2, -21.4)
	add_child(motto)

	_craft_root = Node3D.new()
	_craft_root.name = "Craft"
	add_child(_craft_root)

	_cam = Camera3D.new()
	_cam.fov = 55
	_cam.far = 500
	add_child(_cam)
	_update_camera()


func _update_camera() -> void:
	_cam_pitch = clampf(_cam_pitch, -0.2, 1.35)
	_cam_dist = clampf(_cam_dist, 4.0, 120.0)
	var off := Vector3(sin(_cam_yaw) * cos(_cam_pitch), sin(_cam_pitch), cos(_cam_yaw) * cos(_cam_pitch)) * _cam_dist
	_cam.position = _cam_target + off
	_cam.look_at(_cam_target, Vector3.UP)


func _rebuild_craft() -> void:
	for c in _craft_root.get_children():
		c.queue_free()
	_part_nodes.clear()
	var min_y := 0.0
	for i in model.parts.size():
		var p: Dictionary = model.parts[i]
		var v := PartVisual.build(p.part)
		v.transform = PartVisual.xform(p.pos, p.rot)
		v.get_node("Pick").set_meta("part_index", i)
		_craft_root.add_child(v)
		_part_nodes.append(v)
		var h := CraftModel.part_height(p.part)
		min_y = minf(min_y, p.pos[1] - h * 0.5 - (0.6 if Sim.part(p.part).get("shape", {}).get("type", "") in ["booster", "engine"] else 0.0))
	# stand the craft on the floor
	_craft_root.position = Vector3(0, -min_y + 0.2, 0)
	_hover = -1


# ---- UI -------------------------------------------------------------------------

func _build_ui() -> void:
	var layer := CanvasLayer.new()
	add_child(layer)
	_ui = Control.new()
	_ui.set_anchors_preset(Control.PRESET_FULL_RECT)
	_ui.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_ui.theme = HangarTheme.make()
	layer.add_child(_ui)

	var top := PanelContainer.new()
	top.set_anchors_preset(Control.PRESET_TOP_WIDE)
	top.offset_bottom = 52
	_ui.add_child(top)
	var bar := HBoxContainer.new()
	bar.add_theme_constant_override("separation", 8)
	top.add_child(bar)
	var title := Label.new()
	title.text = "  PASA ASSEMBLY HANGAR  "
	title.add_theme_color_override("font_color", PartVisual.ORANGE)
	bar.add_child(title)
	_name_edit = LineEdit.new()
	_name_edit.custom_minimum_size = Vector2(240, 0)
	_name_edit.placeholder_text = "Craft name"
	_name_edit.text_submitted.connect(func(t): model.set_name_text(t); _name_edit.release_focus())
	_name_edit.focus_exited.connect(func(): if _name_edit.text != model.craft_name: model.set_name_text(_name_edit.text))
	bar.add_child(_name_edit)
	bar.add_child(_button("New", _on_new, "Start a new craft"))
	var load_btn := _button("Load", func(): _show_load_menu(), "Load a saved or stock craft")
	bar.add_child(load_btn)
	bar.add_child(_button("Save", _on_save, "Save craft (Ctrl+S)"))
	_undo_button = _button("Undo", func(): model.undo(), "Ctrl+Z")
	_redo_button = _button("Redo", func(): model.redo(), "Ctrl+Y")
	bar.add_child(_undo_button)
	bar.add_child(_redo_button)
	_sym_button = _button("Symmetry: 1x", func(): _cycle_symmetry(1), "Radial symmetry (X / Shift+X)")
	bar.add_child(_sym_button)
	bar.add_child(_button("Reset staging", func(): model.reset_staging(), "Rebuild the staging sequence automatically"))
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	spacer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	bar.add_child(spacer)
	var launch := _button("  LAUNCH  ", _on_launch, "Roll out to the Glacier Shelf launch pad")
	launch.add_theme_color_override("font_color", Color(1, 1, 1))
	launch.add_theme_stylebox_override("normal", HangarTheme.box(PartVisual.ORANGE.darkened(0.15), 6))
	launch.add_theme_stylebox_override("hover", HangarTheme.box(PartVisual.ORANGE, 6))
	bar.add_child(launch)

	_load_menu = PopupMenu.new()
	_load_menu.id_pressed.connect(_on_load_pick)
	_ui.add_child(_load_menu)

	# parts palette
	var left := PanelContainer.new()
	left.anchor_top = 0
	left.anchor_bottom = 1
	left.offset_top = 60
	left.offset_bottom = -40
	left.offset_left = 8
	left.offset_right = 300
	_ui.add_child(left)
	var lv := VBoxContainer.new()
	left.add_child(lv)
	var lt := Label.new()
	lt.text = "PARTS"
	lt.add_theme_color_override("font_color", PartVisual.ICE)
	lv.add_child(lt)
	_parts_tabs = TabContainer.new()
	_parts_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	lv.add_child(_parts_tabs)
	for cat in CATEGORIES:
		var scroll := ScrollContainer.new()
		scroll.name = CATEGORY_NAMES[cat].substr(0, 5)
		scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
		var list := VBoxContainer.new()
		list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		scroll.add_child(list)
		var any := false
		for id in Sim.catalog_order:
			var d: Dictionary = Sim.part(id)
			if d.category != cat:
				continue
			any = true
			var b := _button(d.name, _on_palette.bind(id), _part_tooltip(d))
			b.alignment = HORIZONTAL_ALIGNMENT_LEFT
			b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			b.text = "%s\n   %.2f t" % [d.name, (d.mass + d.get("fuel_liquid", 0) + d.get("fuel_solid", 0)) / 1000.0]
			list.add_child(b)
		if any:
			_parts_tabs.add_child(scroll)
			_parts_tabs.set_tab_title(_parts_tabs.get_tab_count() - 1, CATEGORY_NAMES[cat])
	_parts_tabs.tab_alignment = TabBar.ALIGNMENT_LEFT
	_parts_tabs.clip_tabs = true

	# readouts + staging
	var right := PanelContainer.new()
	right.anchor_left = 1
	right.anchor_right = 1
	right.anchor_bottom = 1
	right.offset_left = -372
	right.offset_right = -8
	right.offset_top = 60
	right.offset_bottom = -40
	_ui.add_child(right)
	var rv := VBoxContainer.new()
	rv.add_theme_constant_override("separation", 6)
	right.add_child(rv)
	_summary = RichTextLabel.new()
	_summary.custom_minimum_size = Vector2(320, 0)
	_summary.bbcode_enabled = true
	_summary.fit_content = true
	_summary.scroll_active = false
	rv.add_child(_summary)
	var st := Label.new()
	st.text = "STAGING — bottom fires first; drag parts between stages"
	st.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	st.custom_minimum_size = Vector2(320, 0)
	st.add_theme_color_override("font_color", PartVisual.ICE)
	st.add_theme_font_size_override("font_size", 13)
	rv.add_child(st)
	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	rv.add_child(scroll)
	_stage_box = VBoxContainer.new()
	_stage_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_stage_box.add_theme_constant_override("separation", 2)
	scroll.add_child(_stage_box)

	_hint = Label.new()
	_hint.set_anchors_preset(Control.PRESET_BOTTOM_WIDE)
	_hint.offset_top = -34
	_hint.offset_left = 280
	_hint.add_theme_font_size_override("font_size", 13)
	_hint.add_theme_color_override("font_color", Color(0.75, 0.8, 0.9))
	_ui.add_child(_hint)

	_toast = Label.new()
	_toast.set_anchors_preset(Control.PRESET_CENTER_TOP)
	_toast.offset_top = 64
	_toast.add_theme_color_override("font_color", PartVisual.ORANGE)
	_toast.add_theme_font_size_override("font_size", 18)
	_toast.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_ui.add_child(_toast)


func _button(text: String, cb: Callable, tip := "") -> Button:
	var b := Button.new()
	b.text = text
	b.tooltip_text = tip
	b.focus_mode = Control.FOCUS_NONE
	b.pressed.connect(cb)
	return b


func _part_tooltip(d: Dictionary) -> String:
	var t: String = "%s\n%s\n\nDry mass: %.0f kg" % [d.name, d.get("description", ""), d.mass]
	if d.has("fuel_liquid"):
		t += "\nLiquid propellant: %.0f kg" % d.fuel_liquid
	if d.has("fuel_solid"):
		t += "\nSolid propellant: %.0f kg" % d.fuel_solid
	if d.has("engine"):
		var e: Dictionary = d.engine
		t += "\nThrust: %.0f kN (vac)\nIsp: %.0f s vac / %.0f s sea level" % [e.thrust_vac / 1000.0, e.isp_vac, e.isp_sl]
	if d.get("crew", 0) > 0:
		t += "\nCrew: %d" % d.crew
	if d.get("torque", 0) > 0:
		t += "\nReaction torque: %.0f kN·m" % (d.torque / 1000.0)
	return t


func toast(msg: String) -> void:
	_toast.text = msg
	var tw := create_tween()
	_toast.modulate.a = 1.0
	tw.tween_interval(1.8)
	tw.tween_property(_toast, "modulate:a", 0.0, 0.6)


# ---- model events ------------------------------------------------------------

func _on_model_changed() -> void:
	_rebuild_craft()
	_stats = model.stats()
	if not _name_edit.has_focus():
		_name_edit.text = model.craft_name
	_undo_button.disabled = not model.can_undo()
	_redo_button.disabled = not model.can_redo()
	_refresh_readouts()
	_refresh_staging()


func _refresh_readouts() -> void:
	if _stats.is_empty() or _stats.has("error"):
		_summary.text = "[color=#8fb8e8]Pick a pod from the parts list to start.[/color]" if model.parts.is_empty() else "[color=red]%s[/color]" % _stats.get("error", "")
		return
	var t := "[font_size=20][b]%s[/b][/font_size]\n" % _stats.name
	t += "[color=#9fb4cc]Parts[/color] %d    [color=#9fb4cc]Crew[/color] %d    [color=#9fb4cc]Mass[/color] %.2f t (dry %.2f t)\n" % [
		_stats.parts, _stats.crew, _stats.mass / 1000.0, _stats.dry_mass / 1000.0]
	t += "[color=#ff8a2a][b]Δv  %.0f m/s[/b][/color] vacuum    [color=#ffb070]%.0f m/s[/color] sea level\n" % [_stats.dv_vac, _stats.dv_sl]
	var launch_twr := 0.0
	for s in _stats.stages:
		if s.thrust_sl > 0:
			launch_twr = s.twr_sl
			break
	var col := "#7fe08a" if launch_twr > 1.0 else "#ff6a5a"
	t += "[color=#9fb4cc]Liftoff TWR (Floe)[/color] [color=%s]%.2f[/color]" % [col, launch_twr]
	_summary.text = t


func _refresh_staging() -> void:
	for c in _stage_box.get_children():
		c.queue_free()
	var by_stage := {}
	for s in (_stats.get("stages", []) if not _stats.is_empty() else []):
		by_stage[int(s.stage)] = s
	var count := model.stage_count()
	# KSP convention: stage 0 at the top, the first stage to fire at the bottom.
	# A gap above card s inserts a new stage numbered s.
	_stage_box.add_child(_stage_gap(0))
	for s in range(0, count):
		_stage_box.add_child(_stage_card(s, by_stage.get(s, {})))
		_stage_box.add_child(_stage_gap(s + 1))


func _stage_card(s: int, st: Dictionary) -> Control:
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", HangarTheme.box(Color(0.1, 0.15, 0.24, 0.95), 6, Color(0.3, 0.45, 0.65)))
	var v := VBoxContainer.new()
	panel.add_child(v)
	var head := Label.new()
	var info := ""
	if not st.is_empty() and st.dv_vac > 0.05:
		info = "   Δv %.0f m/s (SL %.0f)   TWR %.2f   %.0f s" % [st.dv_vac, st.dv_sl, st.twr_sl if st.twr_sl > 0 else st.twr_vac, st.burn_time]
	head.text = "Stage %d%s" % [s, info]
	head.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	head.custom_minimum_size = Vector2(300, 0)
	head.add_theme_font_size_override("font_size", 13)
	head.add_theme_color_override("font_color", PartVisual.ORANGE if s == model.stage_count() - 1 else Color(0.85, 0.9, 1.0))
	v.add_child(head)
	var flow := HFlowContainer.new()
	v.add_child(flow)
	var seen_sym := {}
	for i in model.parts.size():
		var p: Dictionary = model.parts[i]
		if p.stage != s:
			continue
		if p.sym != 0:
			if seen_sym.has(p.sym):
				continue
			seen_sym[p.sym] = true
		var n := model.counterparts(i).size()
		var chip := Button.new()
		chip.text = Sim.part(p.part).name.get_slice(" ", 0) + ("  x%d" % n if n > 1 else "")
		chip.tooltip_text = Sim.part(p.part).name
		chip.add_theme_font_size_override("font_size", 12)
		chip.focus_mode = Control.FOCUS_NONE
		chip.set_drag_forwarding(func(_pos): return _drag_chip(i, chip.text), Callable(), Callable())
		chip.mouse_entered.connect(func(): _highlight([i]))
		chip.mouse_exited.connect(func(): _highlight([]))
		flow.add_child(chip)
	panel.set_drag_forwarding(Callable(), _can_drop_chip, func(_pos, data): model.set_stage(data.index, s))
	return panel


## Thin drop zone between cards: dropping here inserts a new stage.
func _stage_gap(s: int) -> Control:
	var gap := PanelContainer.new()
	gap.custom_minimum_size = Vector2(0, 10)
	gap.add_theme_stylebox_override("panel", StyleBoxEmpty.new())
	gap.set_drag_forwarding(Callable(), _can_drop_chip, func(_pos, data): model.insert_stage(data.index, s))
	gap.tooltip_text = "Drop here to make a new stage"
	return gap


func _drag_chip(i: int, label: String) -> Variant:
	var preview := Label.new()
	preview.text = label
	preview.add_theme_color_override("font_color", PartVisual.ORANGE)
	_ui.set_drag_preview(preview)
	return {"kind": "stage_part", "index": i}


func _can_drop_chip(_pos: Vector2, data: Variant) -> bool:
	return typeof(data) == TYPE_DICTIONARY and data.get("kind", "") == "stage_part"


func _highlight(ids: Array) -> void:
	for i in _part_nodes.size():
		var lit := ids.has(i)
		for mi in _part_nodes[i].find_children("*", "MeshInstance3D", true, false):
			(mi as MeshInstance3D).material_overlay = PartVisual.highlight_mat() if lit else null


# ---- buttons -------------------------------------------------------------------

func _on_new() -> void:
	model.checkpoint()
	model.parts.clear()
	model.craft_name = "Untitled"
	model.description = ""
	model.manual_staging = false
	model.changed.emit()


func _on_save() -> void:
	var fname := model.craft_name.to_snake_case().validate_filename()
	if fname == "":
		fname = "untitled"
	var path := Sim.USER_CRAFT_DIR + fname + ".craft.json"
	var err := model.save_file(path)
	toast("Saved %s" % path.get_file() if err == OK else "Save failed (%s)" % error_string(err))


func _show_load_menu() -> void:
	_load_menu.clear()
	var files := Sim.craft_files()
	for k in files.size():
		var tag := "stock" if files[k].begins_with(Sim.STOCK_CRAFT_DIR) else "saved"
		_load_menu.add_item("%s   (%s)" % [files[k].get_file().trim_suffix(".craft.json"), tag], k)
		_load_menu.set_item_metadata(k, files[k])
	_load_menu.position = Vector2i(get_viewport().get_mouse_position()) + Vector2i(0, 10)
	_load_menu.popup()


func _on_load_pick(id: int) -> void:
	var path: String = _load_menu.get_item_metadata(_load_menu.get_item_index(id))
	if model.load_file(path):
		toast("Loaded %s" % model.craft_name)
		_frame_craft()


func _on_launch() -> void:
	if model.parts.is_empty():
		toast("Nothing to launch yet")
		return
	Sim.pending_craft_text = model.save_text()
	if ResourceLoader.exists("res://scenes/flight.tscn"):
		get_tree().change_scene_to_file("res://scenes/flight.tscn")
	else:
		toast("The launch pad is still being built (M4)")


func _on_palette(id: String) -> void:
	_drop_held()
	_held = CraftModel.held_single(id)
	_held_spin = 0.0
	_held_extra = DMath.q_identity()


func _cycle_symmetry(dir: int) -> void:
	var k := SYMMETRY_STEPS.find(symmetry)
	symmetry = SYMMETRY_STEPS[(k + dir + SYMMETRY_STEPS.size()) % SYMMETRY_STEPS.size()]
	_sym_button.text = "Symmetry: %dx" % symmetry


# ---- 3D interaction --------------------------------------------------------------

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		match mb.button_index:
			MOUSE_BUTTON_RIGHT:
				if mb.pressed and not _held.is_empty():
					_drop_held()
				else:
					_orbiting = mb.pressed
			MOUSE_BUTTON_MIDDLE:
				_panning = mb.pressed
			MOUSE_BUTTON_WHEEL_UP:
				if mb.pressed:
					_cam_dist *= 0.9
					_update_camera()
			MOUSE_BUTTON_WHEEL_DOWN:
				if mb.pressed:
					_cam_dist *= 1.1
					_update_camera()
			MOUSE_BUTTON_LEFT:
				if mb.pressed:
					_click()
	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if _orbiting:
			_cam_yaw -= mm.relative.x * 0.008
			_cam_pitch += mm.relative.y * 0.006
			_update_camera()
		elif _panning:
			_cam_target.y = clampf(_cam_target.y + mm.relative.y * 0.03 * _cam_dist / 20.0, 0.5, 80.0)
			_update_camera()
	elif event is InputEventKey and event.pressed:
		_key(event as InputEventKey)


func _key(k: InputEventKey) -> void:
	var ctrl := k.ctrl_pressed or k.meta_pressed
	match k.keycode:
		KEY_Z:
			if ctrl:
				if k.shift_pressed: model.redo()
				else: model.undo()
		KEY_Y:
			if ctrl: model.redo()
		KEY_S:
			if ctrl:
				_on_save()
			else:
				_rotate_input([1.0, 0.0, 0.0], ROT_STEP)
		KEY_W:
			_rotate_input([1.0, 0.0, 0.0], -ROT_STEP)
		KEY_A:
			_rotate_input([0.0, 1.0, 0.0], ROT_STEP)
		KEY_D:
			_rotate_input([0.0, 1.0, 0.0], -ROT_STEP)
		KEY_Q:
			_rotate_input([0.0, 0.0, 1.0], ROT_STEP)
		KEY_E:
			_rotate_input([0.0, 0.0, 1.0], -ROT_STEP)
		KEY_X:
			_cycle_symmetry(-1 if k.shift_pressed else 1)
		KEY_DELETE, KEY_BACKSPACE:
			if not _held.is_empty():
				_drop_held()
			elif _hover >= 0:
				model.remove(_hover)
		KEY_ESCAPE:
			_drop_held()
		KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_PAGEUP, KEY_PAGEDOWN:
			if _hover > 0 and _held.is_empty():
				var step := 0.25 if k.shift_pressed else 0.05
				var d := {KEY_UP: [0.0, step, 0.0], KEY_DOWN: [0.0, -step, 0.0], KEY_LEFT: [-step, 0.0, 0.0],
					KEY_RIGHT: [step, 0.0, 0.0], KEY_PAGEUP: [0.0, 0.0, step], KEY_PAGEDOWN: [0.0, 0.0, -step]}
				model.offset_part(_hover, d[k.keycode])


## Q/E roll, W/S pitch, A/D yaw: for the held part, or the hovered part.
func _rotate_input(axis: Array, angle: float) -> void:
	if not _held.is_empty():
		if axis[2] != 0.0:
			_held_spin += angle
		_held_extra = DMath.q_normalized(DMath.q_mul(_held_extra, DMath.q_axis_angle(axis, angle)))
	elif _hover > 0:
		model.rotate_part(_hover, axis, angle)


func _click() -> void:
	if _held.is_empty():
		if _hover > 0:
			_held = model.pick_up(_hover)
			_held_spin = 0.0
			_held_extra = DMath.q_identity()
		return
	if model.parts.is_empty():
		model.place_root(_held)
		_clear_held()
		return
	if _snap.is_empty():
		toast("Attach it to a node or a surface")
		return
	if _snap.kind == "stack":
		model.attach_stack(_held, _snap.target, _snap.node, _held_spin)
	else:
		model._commit(_held, model.surface_placements(_held, _snap.target, _snap.angle, _snap.y, symmetry, _held_extra))
	if not Input.is_key_pressed(KEY_SHIFT):
		_clear_held()


func _clear_held() -> void:
	_held = {}
	_snap = {}
	_clear_ghosts()


## Drops the held assembly back into the parts bin (deletes it; undoable).
func _drop_held() -> void:
	_clear_held()


func _clear_ghosts() -> void:
	for g in _ghosts:
		g.queue_free()
	_ghosts.clear()


func _process(_dt: float) -> void:
	_update_hover_and_snap()
	_update_hint()


func _mouse_ray() -> Dictionary:
	var mp := get_viewport().get_mouse_position()
	var from := _cam.project_ray_origin(mp)
	var dir := _cam.project_ray_normal(mp)
	var q := PhysicsRayQueryParameters3D.create(from, from + dir * 500.0, PartVisual.PICK_LAYER)
	var hit := get_world_3d().direct_space_state.intersect_ray(q)
	return {"from": from, "dir": dir, "hit": hit}


func _update_hover_and_snap() -> void:
	var ray := _mouse_ray()
	var hit: Dictionary = ray.hit
	var over_ui := _ui.get_viewport().gui_get_hovered_control() != null
	var idx := -1
	if not hit.is_empty() and hit.collider.has_meta("part_index") and not over_ui:
		idx = hit.collider.get_meta("part_index")
	if _held.is_empty():
		if idx != _hover:
			_hover = idx
			_highlight([] if idx < 0 else model.counterparts(idx))
		_clear_ghosts()
		return
	if _hover >= 0:
		_hover = -1
		_highlight([])
	_snap = {}
	var placements := []
	if model.parts.is_empty():
		placements = [{"pos": [0.0, 0.0, 0.0], "rot": DMath.q_identity()}]
	else:
		# stack nodes within SNAP_PX of the cursor win over surfaces
		var mp := get_viewport().get_mouse_position()
		var best := SNAP_PX
		for n in model.free_nodes():
			var wp := _craft_root.global_transform * DMath.to_v3(n.pos)
			if _cam.is_position_behind(wp):
				continue
			var d := _cam.unproject_position(wp).distance_to(mp)
			if d < best:
				var pl := model.stack_placements(_held, n.index, n.node, _held_spin)
				if not pl.is_empty():
					best = d
					placements = pl
					_snap = {"kind": "stack", "target": n.index, "node": n.node}
		if _snap.is_empty() and idx >= 0 and Sim.part(_held.parts[0].part).get("surface_attach", false):
			var tp: Dictionary = model.parts[idx]
			var local_hit := (_craft_root.global_transform * PartVisual.xform(tp.pos, tp.rot)).affine_inverse() * (hit.position as Vector3)
			var angle := atan2(local_hit.x, local_hit.z)
			var pl := model.surface_placements(_held, idx, angle, local_hit.y, symmetry, _held_extra)
			if not pl.is_empty():
				placements = pl
				_snap = {"kind": "surface", "target": idx, "angle": angle, "y": local_hit.y}
	var ok := not _snap.is_empty() or model.parts.is_empty()
	if placements.is_empty():
		# float in front of the camera
		var p: Vector3 = ray.from + ray.dir * _cam_dist * 0.7 - _craft_root.global_position
		placements = [{"pos": [p.x, p.y, p.z], "rot": DMath.q_identity()}]
	_show_ghosts(placements, ok)


func _show_ghosts(placements: Array, ok: bool) -> void:
	while _ghosts.size() < placements.size():
		var g := Node3D.new()
		for h in _held.parts:
			var v := PartVisual.build(h.part, false)
			v.transform = PartVisual.xform(h.pos, h.rot)
			PartVisual.paint(v, PartVisual.ghost_mat(true))
			g.add_child(v)
		_craft_root.add_child(g)
		_ghosts.append(g)
	while _ghosts.size() > placements.size():
		_ghosts.pop_back().queue_free()
	for k in placements.size():
		_ghosts[k].transform = PartVisual.xform(placements[k].pos, placements[k].rot)
		PartVisual.paint(_ghosts[k], PartVisual.ghost_mat(ok))


func _update_hint() -> void:
	if not _held.is_empty():
		var name: String = Sim.part(_held.parts[0].part).name
		_hint.text = "Holding %s%s — click to attach · Q/E spin · W/S/A/D tilt · X symmetry (%dx) · Shift+click keeps placing · right-click/Esc drops it" % [
			name, "" if _held.parts.size() == 1 else " (+%d)" % (_held.parts.size() - 1), symmetry]
	elif _hover >= 0:
		var p: Dictionary = model.parts[_hover]
		_hint.text = "%s — click to pick up · Del removes · Q/E/W/S/A/D rotate · arrows/PgUp/PgDn offset (Shift: bigger steps)" % Sim.part(p.part).name
	else:
		_hint.text = "Right-drag orbit · wheel zoom · middle-drag up/down · pick a part on the left · Ctrl+Z/Y undo/redo · Ctrl+S save"
