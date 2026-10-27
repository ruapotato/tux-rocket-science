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
## Inside the ship: the rooms' walls (trs_core cuts them where rooms meet
## and where the windows are), warm lamps, and the penguin walking or
## floating about in them. The node sits in the craft's own frame, so it
## turns with the ship; everything under it is in craft-local metres.
## It all lives on its own render layer, which the sun doesn't light.
class_name InteriorView
extends Node3D

const LAYER := 1 << 2
const CELL := 0.2
const WALL_SHADER := """
shader_type spatial;
render_mode cull_disabled;
varying vec3 lp;
varying float kind;
void vertex() { lp = VERTEX; kind = COLOR.r * 4.0 + 0.5; }
void fragment() {
	int k = int(kind);
	vec3 g = abs(fract(lp / 0.6) - 0.5);
	float seam = smoothstep(0.47, 0.5, max(max(g.x, g.y), g.z));
	vec3 col = vec3(0.86, 0.83, 0.76);                 // warm cream panels
	float glow = 0.0;
	if (k == 2) {                                      // a ring's deck
		col = vec3(0.33, 0.4, 0.5);
		float s = abs(fract(lp.y / 0.75) - 0.5);
		col = mix(col, vec3(1.0, 0.55, 0.15), smoothstep(0.46, 0.5, s) * 0.8);
	} else if (k == 3) {                               // its ceiling, lamps along it
		col = vec3(0.92, 0.92, 0.95);
		glow = smoothstep(0.42, 0.5, abs(fract(atan(lp.z, lp.x) * 24.0 / 6.2831853) - 0.5));
	} else if (k == 1) {                               // end walls and boxes
		col = vec3(0.74, 0.78, 0.84);
	}
	ALBEDO = col * (1.0 - seam * 0.35);
	ROUGHNESS = 0.7;
	EMISSION = vec3(1.0, 0.9, 0.7) * glow * 1.4 + col * 0.06;
}
"""

var core: TrsSim
var vessel_id := -1
var walls: MeshInstance3D
var penguin: Node3D
var _facing: Node3D
var _anim: TuxAnimator
var _built_for := ""
var up := Vector3.UP       # the penguin's up, craft-local
var _fwd := Vector3.BACK   # where it faces, craft-local
var _air := false
var windows: Array = []  # craft-local centres of the capsule windows (a cockpit view looks out of the first)
var screen: MeshInstance3D  # the ship's computer's screen (on a capsule's console, or a hab's wall)
var _screen_tex: Texture2D
var _screen_mat: StandardMaterial3D


func setup(c: TrsSim, id: int) -> void:
	core = c
	vessel_id = id
	name = "Interior"
	penguin = Node3D.new()
	penguin.name = "Penguin"
	add_child(penguin)
	_facing = Node3D.new()
	_facing.position.y = -0.45
	penguin.add_child(_facing)
	var h: float = Sim.part("eva_penguin").get("shape", {}).get("height", 0.9)
	var rig: Node3D = load("res://scenes/tux_rig.tscn").instantiate()
	rig.scale = Vector3.ONE * (h / 1.05)
	_facing.add_child(rig)
	# its feet at the bottom of the ball it takes up indoors (it hunches in a capsule)
	_facing.position.y = -float(Sim.part("eva_penguin").get("eva", {}).get("room_radius", h * 0.5))
	_anim = TuxAnimator.new()
	_anim.setup(rig)
	_layer(penguin)


## Rebuilds the walls and lamps when the craft's parts change.
func rebuild(parts: Array) -> void:
	var key := str(parts.size()) + ":" + ",".join(parts.map(func(p): return str(p.uid)))
	if key == _built_for:
		return
	_built_for = key
	if walls:
		walls.queue_free()
	for c in get_children():
		if c is OmniLight3D or c.name.begins_with("Fit"):
			c.queue_free()
	windows.clear()
	screen = null
	var feed: PackedFloat64Array = core.room_mesh(vessel_id, CELL)
	if feed.is_empty():
		return
	var n := int(feed[0])
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var gst := SurfaceTool.new()
	gst.begin(Mesh.PRIMITIVE_TRIANGLES)
	var glass_n := 0
	for t in n:
		var o := 1 + t * 9
		var a := Vector3(feed[o], feed[o + 1], feed[o + 2])
		var b := Vector3(feed[o + 3], feed[o + 4], feed[o + 5])
		var c := Vector3(feed[o + 6], feed[o + 7], feed[o + 8])
		var kind := int(feed[1 + 9 * n + t])
		# (b - a) x (c - a) faces into the room: drawn 2 cm in from the
		# wall's true place, clear of the hull of the part next door
		var nrm := (b - a).cross(c - a).normalized()
		var s := gst if kind == 4 else st
		glass_n += 1 if kind == 4 else 0
		s.set_color(Color(kind / 4.0, 0, 0))
		s.set_normal(nrm)
		# Godot's front faces wind clockwise: a, c, b faces the room
		s.add_vertex(a + nrm * 0.02)
		s.add_vertex(c + nrm * 0.02)
		s.add_vertex(b + nrm * 0.02)
	var mesh := st.commit()
	var wm := ShaderMaterial.new()
	wm.shader = Shader.new()
	wm.shader.code = WALL_SHADER
	mesh.surface_set_material(0, wm)
	if glass_n > 0:
		gst.commit(mesh)
	if mesh.get_surface_count() > 1:
		var glass := StandardMaterial3D.new()
		glass.albedo_color = Color(0.6, 0.8, 1.0, 0.08)
		glass.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		glass.cull_mode = BaseMaterial3D.CULL_DISABLED
		glass.roughness = 0.05
		glass.metallic_specular = 1.0
		mesh.surface_set_material(1, glass)
	walls = MeshInstance3D.new()
	walls.name = "Walls"
	walls.mesh = mesh
	walls.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(walls)
	_layer(walls)
	# lamps: one in the middle of each room, a ring of them round a deck
	for p in parts:
		var d: Dictionary = Sim.part(p.part)
		var xf := PartVisual.xform(p.pos, p.rot)
		for v in d.get("interior", {}).get("volumes", []):
			match v.get("type", "cylinder"):
				"cylinder":
					var r: float = v.get("radius", 1.0)
					var y0: float = v.get("y0", 0.0)
					var y1: float = v.get("y1", 0.0)
					_lamp(xf * Vector3(0, (y0 + y1) * 0.5, 0), maxf(r, y1 - y0) * 2.2, 1.4 if r > 0.8 else 0.6)
					if v.has("radius_top"):
						_cockpit(xf, v, CraftModel.size_of(p))
					elif screen == null:
						# a hab: the computer on the wall, at a penguin's eye
						var sz := CraftModel.size_of(p)
						var rw: float = r * (float(sz[0]) + float(sz[2])) * 0.5 - 0.06
						var yc: float = clampf((y0 + y1) * 0.5 * float(sz[1]) + 0.1, y0 * float(sz[1]) + 0.3, y1 * float(sz[1]) - 0.3)
						var wsz := Vector2(minf(0.8, rw * 0.9), minf(0.8, rw * 0.9) * 0.5625)
						_add_screen(xf * Transform3D(Basis(Vector3.UP, PI), Vector3(0, yc, rw)), wsz)
				"annulus":
					var rm: float = (float(v.get("r0", 0.0)) + float(v.get("r1", 0.0))) * 0.5
					var y: float = (float(v.get("y0", 0.0)) + float(v.get("y1", 0.0))) * 0.5
					for k in 12:
						var a := TAU * k / 12.0
						_lamp(xf * Vector3(cos(a) * rm, y, sin(a) * rm), 7.0)
				"box":
					var lo := DMath.to_v3(v.get("min", [0, 0, 0]))
					var hi := DMath.to_v3(v.get("max", [0, 0, 0]))
					_lamp(xf * ((lo + hi) * 0.5), (hi - lo).length() * 1.2)


## The computer's screen: a quad showing the desktop, facing along its +Z.
func _add_screen(xf: Transform3D, size: Vector2) -> void:
	screen = _fit(MeshInstance3D.new(), xf)
	screen.name = "FitScreen"
	var qm := QuadMesh.new()
	qm.size = size
	screen.mesh = qm
	_screen_mat = StandardMaterial3D.new()
	_screen_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_screen_mat.albedo_color = Color(0.05, 0.06, 0.08)
	screen.material_override = _screen_mat
	if _screen_tex:
		_screen_mat.albedo_texture = _screen_tex
		_screen_mat.albedo_color = Color.WHITE


## What the screen shows (the desktop's viewport).
func set_screen_texture(tex: Texture2D) -> void:
	_screen_tex = tex
	if _screen_mat and _screen_mat.albedo_texture != tex:
		_screen_mat.albedo_texture = tex
		_screen_mat.albedo_color = Color.WHITE


## Whether a click at `pos` on the camera's view lands on the screen.
func screen_hit(cam: Camera3D, pos: Vector2) -> bool:
	if screen == null or not is_instance_valid(screen) or cam == null:
		return false
	var o := cam.project_ray_origin(pos)
	var d := cam.project_ray_normal(pos)
	var g := screen.global_transform
	var n := g.basis.z.normalized()
	var den := d.dot(n)
	if absf(den) < 1e-6:
		return false
	var k := (g.origin - o).dot(n) / den
	if k <= 0.0 or k > 6.0:
		return false
	var l := g.affine_inverse() * (o + d * k)
	var half := (screen.mesh as QuadMesh).size * 0.5
	return absf(l.x) <= half.x * 1.1 and absf(l.y) <= half.y * 1.1


## A capsule's cabin: a console under each window, its screens lit, and the
## couch the penguin sits in to fly.
func _cockpit(xf: Transform3D, v: Dictionary, size: Array) -> void:
	var r0: float = float(v.radius) * (float(size[0]) + float(size[2])) * 0.5
	var r1: float = float(v.radius_top) * (float(size[0]) + float(size[2])) * 0.5
	var y0: float = float(v.y0) * float(size[1])
	var y1: float = float(v.y1) * float(size[1])
	var dark := PartVisual.mat("console", Color(0.12, 0.13, 0.16), 0.3, 0.5)
	for w in v.get("windows", []):
		var y: float = float(w.get("at", 0.0)) * float(size[1])
		var rr := lerpf(r0, r1, (y - y0) / (y1 - y0)) - 0.05
		for a_deg in w.get("angles_deg", []):
			var a := deg_to_rad(float(a_deg))
			var out := Vector3(sin(a), 0, cos(a))
			windows.append(xf * (out * rr + Vector3(0, y, 0)))
			var b := Basis(Vector3.UP, a)
			var ry := y - 0.2
			var rc := lerpf(r0, r1, (ry - y0) / (y1 - y0)) - 0.09
			var con := _fit(MeshInstance3D.new(), xf * Transform3D(b * Basis(Vector3.RIGHT, -0.5), b * Vector3(0, ry, rc)))
			var bm := BoxMesh.new()
			bm.size = Vector3(0.3, 0.16, 0.05)
			con.mesh = bm
			con.material_override = dark
			if screen == null:
				# the first console carries the ship's computer
				bm.size = Vector3(0.3, 0.19, 0.05)
				_add_screen(con.transform * Transform3D(Basis(Vector3.UP, PI), Vector3(0, 0.0, -0.028)), Vector2(0.27, 0.152))
				continue
			for k in 3:
				var scr := _fit(MeshInstance3D.new(), con.transform * Transform3D(Basis.IDENTITY, Vector3(-0.09 + 0.09 * k, 0.01, -0.03)))
				var qm := QuadMesh.new()
				qm.size = Vector2(0.06, 0.05)
				scr.mesh = qm
				scr.rotation = con.rotation
				scr.rotate_object_local(Vector3.UP, PI)
				var glow: Color = [Color(0.3, 1.0, 0.55), Color(1.0, 0.6, 0.2), Color(0.4, 0.75, 1.0)][k]
				scr.material_override = PartVisual.mat("screen%d" % k, glow * 0.3, 0.0, 0.4, glow)
	# the couch: a padded seat and back on the floor
	var seat := _fit(MeshInstance3D.new(), xf * Transform3D(Basis(Vector3.RIGHT, 0.25), Vector3(0, y0 + 0.08, -0.08)))
	var sm := BoxMesh.new()
	sm.size = Vector3(0.42, 0.08, 0.5)
	seat.mesh = sm
	seat.material_override = PartVisual.mat("couch", Color(0.85, 0.45, 0.15), 0.0, 0.8)


func _fit(n: Node3D, xf: Transform3D) -> Node3D:
	n.name = "Fit%d" % get_child_count()
	n.transform = xf
	add_child(n)
	_layer(n)
	return n


func _lamp(at: Vector3, reach: float, energy := 1.4) -> void:
	var l := OmniLight3D.new()
	l.position = at
	l.omni_range = reach
	l.light_energy = energy
	l.light_color = Color(1.0, 0.9, 0.75)
	l.light_cull_mask = LAYER
	l.shadow_enabled = false
	add_child(l)


func _layer(n: Node) -> void:
	if n is VisualInstance3D:
		(n as VisualInstance3D).layers = LAYER
	for c in n.get_children():
		_layer(c)


## Places the room in the craft's frame (render space) and the penguin in
## it from the walker telemetry.
func update(frame: PackedFloat64Array, wk: Dictionary, dt: float) -> void:
	if frame.size() < 7:
		visible = false
		return
	visible = true
	transform = Transform3D(Frame.local_basis(frame, 3), Frame.vec(frame, 0))
	var x := DMath.to_v3(wk.x)
	var v := DMath.to_v3(wk.v)
	var felt := DMath.to_v3(wk.felt)
	var standing: bool = wk.get("standing", false)
	# up is away from what it feels; with nothing to feel it keeps its own
	if felt.length() > 0.3:
		up = up.slerp(-felt.normalized(), clampf(dt * 5.0, 0.0, 1.0)).normalized()
	var flat := v - up * v.dot(up)
	if flat.length() > 0.2:
		_fwd = _fwd.slerp(flat.normalized(), clampf(dt * 8.0, 0.0, 1.0))
	_fwd = (_fwd - up * _fwd.dot(up))
	if _fwd.length() < 1e-3:
		_fwd = up.cross(Vector3.RIGHT if absf(up.x) < 0.9 else Vector3.BACK)
	_fwd = _fwd.normalized()
	var right := up.cross(_fwd).normalized()
	penguin.transform = Transform3D(Basis(right, up, _fwd), x)
	if standing:
		if _air:
			_anim.play("land")
		elif not _anim.busy():
			var sp := flat.length()
			_anim.play("run" if sp > 1.2 else ("walk" if sp > 0.15 else "idle"), clampf(sp / 1.2, 0.6, 1.4) if sp > 0.15 else 1.0)
	elif felt.length() < 0.5:
		_anim.play("float")
	else:
		_anim.play("jump" if v.dot(up) > 0.3 else "fall")
	_air = not standing
	_anim.tick(dt)
