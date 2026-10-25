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
## Procedural visuals for parts, built from each part's "shape" data so
## new parts in the JSON catalog get a body without any art pipeline.
## Used by the hangar and by flight. Views only: no gameplay state here.
##
## Surfaces use one hull shader (panel seams, ring seams, grime, hazard
## stripes) and a bell shader for nozzles, all computed from object-space
## position so no UV layout is needed.
class_name PartVisual
extends RefCounted

const PICK_LAYER := 1 << 1

# Palette: PASA white, ice blue, signal orange, graphite.
const WHITE := Color(0.93, 0.94, 0.96)
const ICE := Color(0.55, 0.78, 0.95)
const ORANGE := Color(1.0, 0.52, 0.12)
const GRAPHITE := Color(0.16, 0.17, 0.2)
const STEEL := Color(0.55, 0.57, 0.6)
const YELLOW := Color(0.98, 0.8, 0.15)
const RED := Color(0.86, 0.2, 0.18)
const MONO := Color(0.2, 0.72, 0.66)  # monopropellant / RCS teal

static var _mats := {}
static var _hull_shader: Shader
static var _bell_shader: Shader

const HULL_SHADER := """
shader_type spatial;
render_mode cull_back;
uniform vec4 base_color : source_color = vec4(0.93, 0.94, 0.96, 1.0);
uniform vec4 stripe_color : source_color = vec4(0.1, 0.1, 0.12, 1.0);
uniform float panels = 8.0;        // seams around the circumference
uniform float ring_spacing = 0.9;  // metres between ring seams (0 = none)
uniform float seam_depth = 0.35;
uniform float hazard = 0.0;        // 1 = diagonal hazard stripes
uniform float checker = 0.0;       // 1 = roll-pattern quarters (classic test-rocket paint)
uniform float roughness_base = 0.55;
uniform float metal = 0.0;
uniform float grime = 0.35;
uniform float y_offset = 0.0;
varying vec3 lp;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}

void vertex() { lp = VERTEX; lp.y += y_offset; }

void fragment() {
	float ang = atan(lp.x, lp.z) / 6.2831853 + 0.5;
	vec3 col = base_color.rgb;
	if (checker > 0.5) {
		bool q = fract(ang * 2.0) < 0.5;
		bool h = fract(lp.y / max(ring_spacing, 0.01) * 0.5) < 0.5;
		if (q != h) col = stripe_color.rgb;
	}
	if (hazard > 0.5) {
		float s = fract((ang * 18.0) + lp.y * 3.0);
		col = s < 0.5 ? base_color.rgb : stripe_color.rgb;
	}
	// panel seams
	float sa = abs(fract(ang * panels) - 0.5) * 2.0;
	float seam = smoothstep(0.985, 1.0, sa);
	if (ring_spacing > 0.0) {
		float sr = abs(fract(lp.y / ring_spacing) - 0.5) * 2.0;
		seam = max(seam, smoothstep(0.96, 1.0, sr));
	}
	// grime and weathering, stronger toward the bottom
	float n = vnoise(vec2(ang * 40.0, lp.y * 6.0)) * 0.6 + vnoise(vec2(ang * 140.0, lp.y * 22.0)) * 0.4;
	float g = grime * (0.35 + 0.65 * smoothstep(0.6, -0.8, lp.y)) * n;
	col *= 1.0 - g * 0.22;
	col *= 1.0 - seam * seam_depth;
	ALBEDO = col;
	ROUGHNESS = clamp(roughness_base + n * 0.2 - seam * 0.1, 0.05, 1.0);
	METALLIC = metal;
	SPECULAR = 0.5;
	// a little rim light so silhouettes read against black space
	float rim = pow(1.0 - clamp(dot(NORMAL, VIEW), 0.0, 1.0), 4.0);
	EMISSION = col * rim * 0.08;
}
"""

const BELL_SHADER := """
shader_type spatial;
render_mode cull_disabled;
uniform float height = 1.0;
varying vec3 lp;
void vertex() { lp = VERTEX; }
void fragment() {
	float t = clamp(0.5 - lp.y / height, 0.0, 1.0);    // 0 at the throat, 1 at the lip
	vec3 throat = vec3(0.18, 0.17, 0.17);
	vec3 mid = vec3(0.46, 0.36, 0.28);                  // heat-tinted bronze
	vec3 lip = vec3(0.30, 0.33, 0.42);                  // blued steel at the lip
	vec3 col = t < 0.6 ? mix(throat, mid, t / 0.6) : mix(mid, lip, (t - 0.6) / 0.4);
	float bands = 0.92 + 0.08 * step(0.5, fract(lp.y * 14.0));
	ALBEDO = col * bands;
	METALLIC = 0.85;
	ROUGHNESS = 0.35 + 0.2 * t;
	if (!FRONT_FACING) { ALBEDO *= 0.35; ROUGHNESS = 0.9; }
}
"""


static func mat(key: String, color: Color, metal := 0.0, rough := 0.6, emission := Color.BLACK) -> StandardMaterial3D:
	if _mats.has(key):
		return _mats[key]
	var m := StandardMaterial3D.new()
	m.albedo_color = color
	m.metallic = metal
	m.roughness = rough
	if emission != Color.BLACK:
		m.emission_enabled = true
		m.emission = emission
	_mats[key] = m
	return m


## Hull material with seams/paint options. Cached by its parameters.
static func hull(color: Color, opts := {}) -> ShaderMaterial:
	var key := "hull:%s:%s" % [color.to_html(), str(opts)]
	if _mats.has(key):
		return _mats[key]
	if _hull_shader == null:
		_hull_shader = Shader.new()
		_hull_shader.code = HULL_SHADER
	var m := ShaderMaterial.new()
	m.shader = _hull_shader
	m.set_shader_parameter("base_color", color)
	for k in opts:
		m.set_shader_parameter(k, opts[k])
	_mats[key] = m
	return m


static func bell(h: float) -> ShaderMaterial:
	var key := "bell:%.3f" % h
	if _mats.has(key):
		return _mats[key]
	if _bell_shader == null:
		_bell_shader = Shader.new()
		_bell_shader.code = BELL_SHADER
	var m := ShaderMaterial.new()
	m.shader = _bell_shader
	m.set_shader_parameter("height", h)
	_mats[key] = m
	return m


static func ghost_mat(ok: bool) -> StandardMaterial3D:
	var key := "ghost_ok" if ok else "ghost_bad"
	if _mats.has(key):
		return _mats[key]
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(0.45, 0.9, 1.0, 0.45) if ok else Color(1.0, 0.4, 0.3, 0.45)
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.cull_mode = BaseMaterial3D.CULL_DISABLED
	_mats[key] = m
	return m


static func highlight_mat() -> StandardMaterial3D:
	if _mats.has("highlight"):
		return _mats["highlight"]
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	m.albedo_color = Color(1.0, 0.6, 0.15, 0.35)
	_mats["highlight"] = m
	return m


## opts: {"shroud": metres of interstage fairing above a decoupler,
##        "legs": 0..1 deployment for landing legs, "decals": bool}
static func build(part_id: String, with_pick := true, opts := {}) -> Node3D:
	var d: Dictionary = Sim.part(part_id)
	var sh: Dictionary = d.get("shape", {})
	var root := Node3D.new()
	root.name = part_id
	match sh.get("type", "cylinder"):
		"capsule":
			_capsule(root, sh, d.get("crew", 1), opts.get("decals", true))
		"cylinder":
			_cylinder_part(root, d, sh, opts)
		"engine":
			_engine(root, sh)
		"booster":
			_booster(root, sh)
		"cone":
			_cone(root, d, sh)
		"pylon":
			_pylon(root, sh)
		"hose":
			_hose(root, sh)
		"fin":
			_fin(root, sh)
		"shield":
			_shield(root, sh)
		"probe":
			_probe(root, sh)
		"adapter":
			_adapter(root, sh)
		"mount":
			_mount(root, d, sh)
		"leg":
			_leg(root, d)
			pose_leg(root, d, opts.get("legs", 0.0))
		"rcs":
			_rcs(root, d)
		"sphere":
			_sphere_tank(root, sh)
		"dock":
			_dock(root, sh)
		_:
			_cyl(root, sh.get("radius", 0.5), sh.get("radius", 0.5), sh.get("height", 1.0), 0.0, hull(WHITE))
	if with_pick:
		_add_pick(root, sh)
	return root


static func _mesh(parent: Node3D, mesh: Mesh, m: Material, xf := Transform3D.IDENTITY) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.material_override = m
	mi.transform = xf
	parent.add_child(mi)
	return mi


static func _cyl(parent: Node3D, r_bottom: float, r_top: float, h: float, y: float, m: Material, segs := 48) -> MeshInstance3D:
	var c := CylinderMesh.new()
	c.bottom_radius = r_bottom
	c.top_radius = r_top
	c.height = h
	c.radial_segments = segs
	c.rings = 1
	return _mesh(parent, c, m, Transform3D(Basis.IDENTITY, Vector3(0, y, 0)))


static func _ring(parent: Node3D, r: float, y: float, thick: float, m: Material) -> void:
	var t := TorusMesh.new()
	t.inner_radius = r - thick
	t.outer_radius = r + thick * 0.4
	t.rings = 48
	t.ring_segments = 8
	_mesh(parent, t, m, Transform3D(Basis.IDENTITY.scaled(Vector3(1, 0.6, 1)), Vector3(0, y, 0)))


static func _decal(parent: Node3D, at: Vector3, outward: Vector3, size: float) -> void:
	var dc := Decal.new()
	dc.texture_albedo = PasaLogo.decal_texture()
	dc.size = Vector3(size, 0.5, size)
	# a decal projects along its local -Y: point that into the hull
	dc.transform = Transform3D(Basis(Quaternion(Vector3.DOWN, -outward.normalized())), at)
	dc.cull_mask = 1
	dc.upper_fade = 0.0
	dc.lower_fade = 0.0
	parent.add_child(dc)


static func _capsule(root: Node3D, sh: Dictionary, crew: int, decals: bool) -> void:
	var rb: float = sh.radius_bottom
	var rt: float = sh.radius_top
	var h: float = sh.height
	_cyl(root, rb, rt, h, 0.0, hull(WHITE, {"panels": 12.0, "ring_spacing": h * 0.34, "grime": 0.25, "y_offset": 0.0}), 64)
	# heat shield and docking ring
	_cyl(root, rb * 1.015, rb * 0.98, h * 0.07, -h * 0.475, hull(Color(0.22, 0.17, 0.14), {"panels": 24.0, "ring_spacing": 0.0, "grime": 0.9, "roughness_base": 0.85}))
	_cyl(root, rt * 1.06, rt * 1.06, h * 0.07, h * 0.47, mat("steel", STEEL, 0.75, 0.3))
	_ring(root, rt * 1.03, h * 0.5, 0.035, mat("graphite", GRAPHITE, 0.4, 0.5))
	var tilt := atan2(rb - rt, h)
	var win := mat("window", Color(0.06, 0.16, 0.28), 0.3, 0.05, Color(0.03, 0.08, 0.14))
	var frame := mat("graphite", GRAPHITE, 0.4, 0.5)
	for i in crew:
		var a := (float(i) - (crew - 1) * 0.5) * 0.55
		var y := 0.08 * h
		var rr := lerpf(rb, rt, (y + h * 0.5) / h)
		var b := Basis(Vector3.UP, a) * Basis(Vector3.RIGHT, -tilt)
		var pos := Basis(Vector3.UP, a) * Vector3(0, y, rr)
		var fbox := BoxMesh.new()
		fbox.size = Vector3(0.27, 0.25, 0.04)
		_mesh(root, fbox, frame, Transform3D(b, pos))
		var box := BoxMesh.new()
		box.size = Vector3(0.21, 0.19, 0.05)
		_mesh(root, box, win, Transform3D(b, pos + Basis(Vector3.UP, a) * Vector3(0, 0, 0.005)))
	# hatch on the back, RCS blocks at the corners
	var hatch := BoxMesh.new()
	hatch.size = Vector3(0.34, 0.42, 0.03)
	var hb := Basis(Vector3.UP, PI) * Basis(Vector3.RIGHT, -tilt)
	_mesh(root, hatch, hull(Color(0.85, 0.86, 0.88), {"panels": 1.0, "ring_spacing": 0.0}), Transform3D(hb, Basis(Vector3.UP, PI) * Vector3(0, -0.02 * h, lerpf(rb, rt, 0.48))))
	for k in 4:
		var a := k * PI / 2 + PI / 4
		var rcs := BoxMesh.new()
		rcs.size = Vector3(0.1, 0.1, 0.06)
		var y := h * 0.3
		_mesh(root, rcs, frame, Transform3D(Basis(Vector3.UP, a) * Basis(Vector3.RIGHT, -tilt), Basis(Vector3.UP, a) * Vector3(0, y, lerpf(rb, rt, (y + h * 0.5) / h) + 0.02)))
	_cyl(root, rb * 0.985 + 0.012, rb * 0.985 + 0.012, h * 0.045, -h * 0.34, mat("orange", ORANGE, 0.0, 0.45))
	if decals:
		var y2 := -h * 0.12
		_decal(root, Basis(Vector3.UP, PI / 2) * Vector3(0, y2, lerpf(rb, rt, (y2 + h * 0.5) / h)), Basis(Vector3.UP, PI / 2) * Vector3(0, 0, 1), 0.42)


## Probe core: an eight-sided avionics drum in gold foil between two
## mounting rings, with a whip antenna and a status light.
static func _probe(root: Node3D, sh: Dictionary) -> void:
	var r: float = sh.radius
	var h: float = sh.height
	var foil := mat("foil", Color(0.86, 0.66, 0.26), 0.9, 0.35)
	_cyl(root, r * 0.86, r * 0.86, h * 0.7, 0.0, foil, 8)
	var ring := hull(GRAPHITE, {"panels": 16.0, "ring_spacing": 0.0, "metal": 0.4, "roughness_base": 0.45})
	_cyl(root, r, r * 0.97, h * 0.15, h * 0.425, ring)
	_cyl(root, r * 0.97, r, h * 0.15, -h * 0.425, ring)
	_ring(root, r * 0.99, h * 0.35, 0.02, mat("orange", ORANGE, 0.0, 0.45))
	# avionics boxes on four faces
	for k in 4:
		var a := TAU * k / 4.0 + PI / 8
		var box := BoxMesh.new()
		box.size = Vector3(0.26, h * 0.42, 0.05)
		_mesh(root, box, mat("graphite", GRAPHITE, 0.4, 0.5), Transform3D(Basis(Vector3.UP, a), Basis(Vector3.UP, a) * Vector3(0, 0, r * 0.84)))
	var whip := CylinderMesh.new()
	whip.top_radius = 0.008
	whip.bottom_radius = 0.012
	whip.height = 0.55
	_mesh(root, whip, mat("steel", STEEL, 0.75, 0.3), Transform3D(Basis.IDENTITY, Vector3(r * 0.7, h * 0.5 + 0.27, 0)))
	var lamp := SphereMesh.new()
	lamp.radius = 0.035
	lamp.height = 0.07
	_mesh(root, lamp, mat("lamp", Color(0.3, 1.0, 0.45), 0.0, 0.3, Color(0.2, 0.9, 0.35)), Transform3D(Basis.IDENTITY, Vector3(0, 0.02, r * 0.88)))


## Adapter: a white frustum between two stack sizes, steel rims at both ends.
static func _adapter(root: Node3D, sh: Dictionary) -> void:
	var rb: float = sh.radius_bottom
	var rt: float = sh.radius_top
	var h: float = sh.height
	_cyl(root, rb, rt, h, 0.0, hull(WHITE, {"panels": 12.0, "ring_spacing": 0.0, "grime": 0.3}), 64)
	_ring(root, rt * 1.01, h * 0.5 - 0.01, 0.03, mat("steel", STEEL, 0.75, 0.3))
	_ring(root, rb * 1.005, -h * 0.5 + 0.01, 0.03, mat("steel", STEEL, 0.75, 0.3))
	_cyl(root, rb * 1.004, lerpf(rb, rt, 0.12) * 1.004, h * 0.12, -h * 0.44, mat("orange", ORANGE, 0.0, 0.45), 64)


## Engine mount: a graphite thrust plate with a collar under each node.
static func _mount(root: Node3D, d: Dictionary, sh: Dictionary) -> void:
	var r: float = sh.radius
	var h: float = sh.height
	_cyl(root, r, r, h * 0.7, h * 0.15, hull(GRAPHITE, {"panels": 12.0, "ring_spacing": 0.0, "metal": 0.5, "roughness_base": 0.45}), 64)
	_cyl(root, r * 1.006, r * 1.006, h * 0.25, h * 0.2, mat("orange", ORANGE, 0.0, 0.45), 64)
	for n in d.get("nodes", []):
		if n.dir[1] > -0.5:
			continue
		var at := Vector3(n.pos[0], -h * 0.35, n.pos[2])
		_cyl(root, n.radius * 0.9, n.radius * 0.95, h * 0.3, at.y, mat("steel", STEEL, 0.75, 0.3)).position = at


## Ablative heat shield: a squat dish with a domed, charred face below.
static func _shield(root: Node3D, sh: Dictionary) -> void:
	var r: float = sh.radius
	var h: float = sh.height
	var abl := hull(Color(0.3, 0.22, 0.16), {"panels": 20.0, "ring_spacing": 0.0, "grime": 1.0, "roughness_base": 0.9})
	_cyl(root, r, r * 0.97, h * 0.55, h * 0.225, mat("graphite", GRAPHITE, 0.4, 0.5), 64)
	_cyl(root, r * 0.985, r, h * 0.45, -h * 0.275, abl, 64)
	var dome := SphereMesh.new()
	dome.radius = r * 0.985
	dome.height = h * 0.9
	dome.is_hemisphere = true
	dome.radial_segments = 64
	dome.rings = 8
	# the hemisphere opens upward: flip it so the dome bulges down
	_mesh(root, dome, abl, Transform3D(Basis(Vector3.RIGHT, PI), Vector3(0, -h * 0.5, 0)))
	_ring(root, r * 1.0, -h * 0.05, 0.025, mat("steel", STEEL, 0.75, 0.3))


static func _cylinder_part(root: Node3D, d: Dictionary, sh: Dictionary, opts: Dictionary) -> void:
	var r: float = sh.radius
	var h: float = sh.height
	if d.has("decoupler"):
		_cyl(root, r, r, h * 0.6, h * 0.2, hull(YELLOW, {"hazard": 1.0, "stripe_color": Color(0.08, 0.08, 0.1), "ring_spacing": 0.0, "panels": 1.0}))
		_cyl(root, r * 0.98, r * 0.98, h * 0.4, -h * 0.3, mat("graphite", GRAPHITE, 0.4, 0.5))
		var shroud: float = opts.get("shroud", 0.0)
		if shroud > 0.0:
			_shroud(root, r, h * 0.5, shroud)
	elif d.has("fuel_mono"):
		# monopropellant drum: pale teal with graphite end rings
		_cyl(root, r, r, h, 0.0, hull(Color(0.78, 0.9, 0.88), {"panels": 6.0, "ring_spacing": 0.0, "grime": 0.25}), 64)
		_cyl(root, r * 1.004, r * 1.004, h * 0.18, 0.0, mat("mono", MONO, 0.0, 0.4))
		for y in [h * 0.5 - 0.03, -h * 0.5 + 0.03]:
			_cyl(root, r * 1.008, r * 1.008, 0.06, y, mat("graphite", GRAPHITE, 0.4, 0.5))
	elif d.has("fuel_liquid"):
		_cyl(root, r, r, h, 0.0, hull(WHITE, {"panels": 8.0, "ring_spacing": 0.47, "grime": 0.3}), 64)
		# roll-pattern band on bigger tanks, PASA stripes on all
		if h > 3.0:
			_cyl(root, r * 1.003, r * 1.003, 0.9, 0.6, hull(WHITE, {"checker": 1.0, "ring_spacing": 0.9, "stripe_color": GRAPHITE, "panels": 8.0}), 64)
		_cyl(root, r * 1.006, r * 1.006, minf(0.14, h * 0.08), h * 0.5 - 0.22, mat("orange", ORANGE, 0.0, 0.45))
		_ring(root, r * 1.005, h * 0.5 - 0.01, 0.03, mat("steel", STEEL, 0.75, 0.3))
		_ring(root, r * 1.005, -h * 0.5 + 0.01, 0.03, mat("steel", STEEL, 0.75, 0.3))
		if opts.get("decals", true) and h >= 3.0:
			_decal(root, Vector3(0, -h * 0.08, r), Vector3(0, 0, 1), minf(0.9, h * 0.35))
	else:  # reaction wheel and other short cylinders
		_cyl(root, r, r, h, 0.0, hull(GRAPHITE, {"panels": 16.0, "ring_spacing": 0.0, "metal": 0.4, "roughness_base": 0.45}))
		_cyl(root, r * 1.01, r * 1.01, h * 0.22, 0.0, mat("orange", ORANGE, 0.0, 0.45))
		for k in 12:
			var a := TAU * k / 12.0
			var vent := BoxMesh.new()
			vent.size = Vector3(0.12, h * 0.5, 0.02)
			_mesh(root, vent, mat("vent", Color(0.07, 0.07, 0.08), 0.2, 0.8), Transform3D(Basis(Vector3.UP, a), Basis(Vector3.UP, a) * Vector3(0, 0, r + 0.004)))


## Interstage fairing: covers the engine above a stack decoupler. It is part
## of the decoupler, so it falls away with the spent stage at separation.
static func _shroud(root: Node3D, r: float, y0: float, h: float) -> void:
	var m := hull(WHITE, {"panels": 16.0, "ring_spacing": h * 0.5, "grime": 0.45, "seam_depth": 0.5})
	var c := CylinderMesh.new()
	c.top_radius = r * 1.012
	c.bottom_radius = r * 1.02
	c.height = h
	c.radial_segments = 64
	c.rings = 1
	c.cap_top = false
	c.cap_bottom = false
	var mi := _mesh(root, c, m, Transform3D(Basis.IDENTITY, Vector3(0, y0 + h * 0.5, 0)))
	mi.name = "Shroud"
	var inside := _mesh(root, c, mat("shroud_in", Color(0.2, 0.21, 0.24), 0.2, 0.8), mi.transform)
	inside.material_override.cull_mode = BaseMaterial3D.CULL_FRONT
	_cyl(root, r * 1.03, r * 1.03, 0.08, y0 + h - 0.04, mat("orange", ORANGE, 0.0, 0.45))


static func _engine(root: Node3D, sh: Dictionary) -> void:
	var r: float = sh.radius
	var rb: float = sh.radius_bell
	var h: float = sh.height
	var mount_h := h * 0.22
	# thrust plate and frame
	_cyl(root, r * 0.92, r, mount_h, h * 0.5 - mount_h * 0.5, hull(GRAPHITE, {"panels": 12.0, "ring_spacing": 0.0, "metal": 0.5, "roughness_base": 0.45}))
	for k in 4:
		var a := TAU * k / 4.0 + PI / 4
		var strut := CylinderMesh.new()
		strut.top_radius = 0.025
		strut.bottom_radius = 0.025
		strut.height = h * 0.3
		var top := Vector3(cos(a) * r * 0.7, h * 0.5 - mount_h, sin(a) * r * 0.7)
		var bot := Vector3(cos(a) * r * 0.25, h * 0.5 - mount_h - h * 0.28, sin(a) * r * 0.25)
		_mesh(root, strut, mat("steel", STEEL, 0.75, 0.3), Transform3D(Basis(Quaternion(Vector3.UP, (top - bot).normalized())), (top + bot) * 0.5))
	# combustion chamber, turbopump and plumbing
	var ch_y := h * 0.5 - mount_h - h * 0.12
	_cyl(root, r * 0.26, r * 0.3, h * 0.2, ch_y, mat("chamber", Color(0.5, 0.52, 0.55), 0.85, 0.3))
	var pump := CylinderMesh.new()
	pump.top_radius = r * 0.12
	pump.bottom_radius = r * 0.12
	pump.height = h * 0.16
	_mesh(root, pump, mat("pump", Color(0.62, 0.58, 0.5), 0.8, 0.35), Transform3D(Basis.IDENTITY, Vector3(r * 0.42, ch_y + h * 0.04, 0)))
	_ring(root, r * 0.3, ch_y - h * 0.05, 0.02, mat("pipe", Color(0.72, 0.55, 0.3), 0.9, 0.3))
	# bell
	var bell_h := h * 0.52
	var bell_mesh := CylinderMesh.new()
	bell_mesh.top_radius = rb * 0.38
	bell_mesh.bottom_radius = rb
	bell_mesh.height = bell_h
	bell_mesh.radial_segments = 48
	bell_mesh.cap_top = false
	bell_mesh.cap_bottom = false
	var bmi := _mesh(root, bell_mesh, bell(bell_h), Transform3D(Basis.IDENTITY, Vector3(0, -h * 0.5 + bell_h * 0.5, 0)))
	bmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	_ring(root, rb, -h * 0.5 + 0.01, 0.025, mat("lip", Color(0.32, 0.35, 0.44), 0.9, 0.3))


static func _booster(root: Node3D, sh: Dictionary) -> void:
	var r: float = sh.radius
	var h: float = sh.height
	_cyl(root, r, r, h, 0.0, hull(WHITE, {"panels": 6.0, "ring_spacing": h / 5.0, "grime": 0.4, "seam_depth": 0.5}), 64)
	for i in 4:
		_cyl(root, r * 1.008, r * 1.008, 0.16, -h * 0.4 + i * h * 0.27, mat("red", RED, 0.0, 0.5))
	for i in 5:
		_ring(root, r * 1.004, -h * 0.5 + i * h / 5.0 + 0.02, 0.03, mat("steel", STEEL, 0.75, 0.3))
	var noz := CylinderMesh.new()
	noz.top_radius = r * 0.42
	noz.bottom_radius = r * 0.72
	noz.height = 0.55
	noz.cap_top = false
	noz.cap_bottom = false
	_mesh(root, noz, bell(0.55), Transform3D(Basis.IDENTITY, Vector3(0, -h * 0.5 - 0.2, 0)))


static func _cone(root: Node3D, d: Dictionary, sh: Dictionary) -> void:
	var rb: float = sh.radius_bottom
	var rt: float = sh.radius_top
	var h: float = sh.height
	if d.has("parachute"):
		_cyl(root, rb, rt * 1.2, h * 0.75, -h * 0.125, hull(WHITE, {"panels": 6.0, "ring_spacing": 0.0}))
		_cyl(root, rt * 1.25, rt * 0.6, h * 0.3, h * 0.35, mat("chute", Color(0.95, 0.35, 0.25), 0.0, 0.5))
		_ring(root, rb * 0.98, -h * 0.48, 0.025, mat("orange", ORANGE, 0.0, 0.45))
	else:
		# ogive nosecone: stacked frusta approximate the curve
		var seg := 8
		for k in seg:
			var t0 := float(k) / seg
			var t1 := float(k + 1) / seg
			var r0 := rb * sqrt(1.0 - t0 * t0 * 0.98)
			var r1 := maxf(rb * sqrt(1.0 - t1 * t1 * 0.98), 0.01)
			_cyl(root, r0, r1, h / seg, -h * 0.5 + h * (t0 + t1) * 0.5, hull(WHITE, {"panels": 6.0, "ring_spacing": 0.0, "grime": 0.15}), 64)
		_cyl(root, rb * 1.006, rb * 0.95, h * 0.1, -h * 0.45, mat("orange", ORANGE, 0.0, 0.45))


static func _pylon(root: Node3D, sh: Dictionary) -> void:
	var box := BoxMesh.new()
	var depth: float = sh.get("depth", 0.3)
	box.size = Vector3(sh.radius * 2.0, sh.height, depth)
	_mesh(root, box, hull(YELLOW, {"hazard": 1.0, "stripe_color": Color(0.08, 0.08, 0.1), "panels": 1.0, "ring_spacing": 0.0}))
	var arm := BoxMesh.new()
	arm.size = Vector3(sh.radius * 1.2, sh.height * 0.2, depth * 1.2)
	_mesh(root, arm, mat("graphite", GRAPHITE, 0.4, 0.5), Transform3D(Basis.IDENTITY, Vector3(0, sh.height * 0.3, 0)))
	_mesh(root, arm, mat("graphite", GRAPHITE, 0.4, 0.5), Transform3D(Basis.IDENTITY, Vector3(0, -sh.height * 0.3, 0)))


## Feed line: a short pipe from the surface it sits on out along +Z to the
## part it feeds, with a clamp flange at each end.
## RCS block: a graphite housing on a short pylon with a nozzle per jet;
## each nozzle carries a hidden "Jet<k>" puff the flight view shows while
## that jet fires.
static func _rcs(root: Node3D, d: Dictionary) -> void:
	var box := BoxMesh.new()
	box.size = Vector3(0.16, 0.16, 0.1)
	_mesh(root, box, hull(GRAPHITE, {"panels": 2.0, "ring_spacing": 0.0, "metal": 0.4, "roughness_base": 0.45}))
	var stem := BoxMesh.new()
	stem.size = Vector3(0.06, 0.1, 0.08)
	_mesh(root, stem, mat("steel", STEEL, 0.75, 0.3), Transform3D(Basis.IDENTITY, Vector3(0, 0, -0.07)))
	var band := BoxMesh.new()
	band.size = Vector3(0.165, 0.035, 0.105)
	_mesh(root, band, mat("mono", MONO, 0.0, 0.4))
	var rcs: Dictionary = d.get("rcs", {})
	var noz := mat("nozzle", Color(0.32, 0.33, 0.36), 0.8, 0.35)
	var puff := StandardMaterial3D.new()
	puff.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	puff.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	puff.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	puff.albedo_color = Color(0.75, 0.85, 1.0, 0.55)
	puff.cull_mode = BaseMaterial3D.CULL_DISABLED
	var k := 0
	for n in rcs.get("nozzles", []):
		var dir := Vector3(n.dir[0], n.dir[1], n.dir[2]).normalized()
		var at := Vector3(n.pos[0], n.pos[1], n.pos[2])
		var b := Basis(Quaternion(Vector3.UP, dir))
		var bell := CylinderMesh.new()
		bell.top_radius = 0.028
		bell.bottom_radius = 0.016
		bell.height = 0.06
		bell.radial_segments = 12
		_mesh(root, bell, noz, Transform3D(b, at - dir * 0.03))
		var jet := Node3D.new()
		jet.name = "Jet%d" % k
		jet.transform = Transform3D(b, at)
		jet.visible = false
		var cone := CylinderMesh.new()
		cone.top_radius = 0.16
		cone.bottom_radius = 0.02
		cone.height = 0.7
		cone.radial_segments = 12
		cone.cap_top = false
		cone.cap_bottom = false
		var ci := MeshInstance3D.new()
		ci.mesh = cone
		ci.material_override = puff
		ci.position = Vector3(0, 0.35, 0)
		ci.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		jet.add_child(ci)
		root.add_child(jet)
		k += 1


## Docking port: a short graphite collar under a raised steel ring, with a
## soft seal on the face and three orange guide petals (you line those up).
static func _dock(root: Node3D, sh: Dictionary) -> void:
	var r: float = sh.radius
	var h: float = sh.height
	_cyl(root, r, r, h * 0.55, -h * 0.225, hull(GRAPHITE, {"panels": 12.0, "ring_spacing": 0.0, "metal": 0.4, "roughness_base": 0.45}))
	_cyl(root, r * 0.8, r * 0.86, h * 0.45, h * 0.275, mat("steel", STEEL, 0.75, 0.3))
	_ring(root, r * 0.72, h * 0.5, 0.03, mat("seal", Color(0.12, 0.12, 0.13), 0.0, 0.9))
	_ring(root, r * 1.0, -h * 0.02, 0.03, mat("orange", ORANGE, 0.0, 0.45))
	for k in 3:
		var a := TAU * k / 3.0
		var petal := BoxMesh.new()
		petal.size = Vector3(0.16, h * 0.55, 0.05)
		var b := Basis(Vector3.UP, a) * Basis(Vector3.RIGHT, 0.35)
		_mesh(root, petal, mat("orange", ORANGE, 0.0, 0.45), Transform3D(b, Basis(Vector3.UP, a) * Vector3(0, h * 0.55, r * 0.74)))


## Radial monopropellant tank: a ball on a strap.
static func _sphere_tank(root: Node3D, sh: Dictionary) -> void:
	var r: float = sh.radius
	var ball := SphereMesh.new()
	ball.radius = r
	ball.height = r * 2.0
	ball.radial_segments = 32
	ball.rings = 16
	_mesh(root, ball, hull(Color(0.78, 0.9, 0.88), {"panels": 4.0, "ring_spacing": 0.0, "grime": 0.2}))
	_ring(root, r * 1.0, 0.0, 0.02, mat("mono", MONO, 0.0, 0.4))
	var strap := BoxMesh.new()
	strap.size = Vector3(0.08, r * 1.4, 0.06)
	_mesh(root, strap, mat("graphite", GRAPHITE, 0.4, 0.5), Transform3D(Basis.IDENTITY, Vector3(0, 0, -r * 0.92)))


static func _hose(root: Node3D, sh: Dictionary) -> void:
	var length: float = sh.get("length", 0.3)
	var along := Basis(Vector3.RIGHT, PI / 2)
	var pipe := CylinderMesh.new()
	pipe.top_radius = 0.045
	pipe.bottom_radius = 0.045
	pipe.height = length
	pipe.radial_segments = 16
	_mesh(root, pipe, mat("hose", ORANGE, 0.2, 0.5), Transform3D(along, Vector3(0, 0, length * 0.5)))
	var flange := CylinderMesh.new()
	flange.top_radius = 0.09
	flange.bottom_radius = 0.09
	flange.height = 0.04
	flange.radial_segments = 16
	for z in [0.02, length - 0.02]:
		_mesh(root, flange, mat("graphite", GRAPHITE, 0.4, 0.5), Transform3D(along, Vector3(0, 0, z)))


static func _fin(root: Node3D, sh: Dictionary) -> void:
	# swept trapezoid in the local YZ plane: root chord along Y at z=0, span +Z
	var span: float = sh.span
	var rc: float = sh.root
	var tc: float = sh.tip
	var t: float = sh.thickness * 0.5
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var p := [Vector3(0, rc * 0.5, 0), Vector3(0, -rc * 0.5, 0), Vector3(0, -rc * 0.5, span), Vector3(0, -rc * 0.5 + tc, span)]
	for side in [-1.0, 1.0]:
		var o := Vector3(t * side, 0, 0)
		var tri := [p[0], p[1], p[2], p[0], p[2], p[3]] if side > 0 else [p[0], p[2], p[1], p[0], p[3], p[2]]
		for v in tri:
			st.set_normal(Vector3(side, 0, 0))
			st.add_vertex(v + o)
	for e in 4:
		var a: Vector3 = p[e]
		var b: Vector3 = p[(e + 1) % 4]
		var n := (b - a).cross(Vector3.RIGHT).normalized()
		for v in [a + Vector3(t, 0, 0), b + Vector3(t, 0, 0), b - Vector3(t, 0, 0), a + Vector3(t, 0, 0), b - Vector3(t, 0, 0), a - Vector3(t, 0, 0)]:
			st.set_normal(n)
			st.add_vertex(v)
	var mi := _mesh(root, st.commit(), mat("fin", ORANGE, 0.1, 0.45))
	mi.material_override.cull_mode = BaseMaterial3D.CULL_DISABLED
	# root fillet
	var base := BoxMesh.new()
	base.size = Vector3(t * 4.0, rc * 1.02, 0.06)
	_mesh(root, base, mat("graphite", GRAPHITE, 0.4, 0.5), Transform3D(Basis.IDENTITY, Vector3(0, 0, 0.03)))


static func _leg(root: Node3D, d: Dictionary) -> void:
	var lg: Dictionary = d.get("leg", {})
	var hinge := Vector3(0, 0.1, 0.14)
	if lg.has("hinge"):
		hinge = Vector3(lg.hinge[0], lg.hinge[1], lg.hinge[2])
	var steel := mat("steel", STEEL, 0.75, 0.3)
	var dark := mat("graphite", GRAPHITE, 0.4, 0.5)
	# housing on the tank wall
	var housing := BoxMesh.new()
	housing.size = Vector3(0.26, 0.72, 0.12)
	_mesh(root, housing, hull(YELLOW, {"panels": 1.0, "ring_spacing": 0.0, "grime": 0.5}), Transform3D(Basis.IDENTITY, Vector3(0, -0.2, 0.06)))
	var pivot := Node3D.new()
	pivot.name = "Pivot"
	pivot.position = hinge
	root.add_child(pivot)
	var outer := CylinderMesh.new()
	outer.top_radius = 0.07
	outer.bottom_radius = 0.065
	outer.height = 1.0
	var o := _mesh(pivot, outer, steel)
	o.name = "Outer"
	var inner := CylinderMesh.new()
	inner.top_radius = 0.05
	inner.bottom_radius = 0.05
	inner.height = 1.0
	var i := _mesh(pivot, inner, mat("chrome", Color(0.8, 0.82, 0.85), 0.95, 0.15))
	i.name = "Inner"
	var foot := CylinderMesh.new()
	foot.top_radius = 0.12
	foot.bottom_radius = float(lg.get("foot_radius", 0.2))
	foot.height = 0.06
	var f := _mesh(pivot, foot, dark)
	f.name = "Foot"
	var brace := CylinderMesh.new()
	brace.top_radius = 0.03
	brace.bottom_radius = 0.03
	brace.height = 1.0
	var b := _mesh(root, brace, steel)
	b.name = "Brace"


## Poses a leg visual for deployment k (0 stowed .. 1 deployed), using the
## same geometry as trs_leg_foot() in trs_core.
static func pose_leg(root: Node3D, d: Dictionary, k: float) -> void:
	var lg: Dictionary = d.get("leg", {})
	var L: float = lg.get("length", 1.6)
	var ang := deg_to_rad(float(lg.get("deploy_angle_deg", 20.0))) * k
	var frac: float = lg.get("stowed_fraction", 0.55)
	var len := L * (frac + (1.0 - frac) * k)
	var pivot: Node3D = root.get_node_or_null("Pivot")
	if pivot == null:
		return
	# strut points down and swings out (+Z) by `ang`, matching trs_leg_foot()
	pivot.basis = Basis(Vector3.RIGHT, -ang)
	var outer: MeshInstance3D = pivot.get_node("Outer")
	var outer_len := L * frac * 0.95
	outer.scale = Vector3(1, outer_len, 1)
	outer.position = Vector3(0, -outer_len * 0.5, 0)
	var inner: MeshInstance3D = pivot.get_node("Inner")
	inner.scale = Vector3(1, len - 0.05, 1)
	inner.position = Vector3(0, -(len - 0.05) * 0.5, 0)
	var foot: MeshInstance3D = pivot.get_node("Foot")
	foot.position = Vector3(0, -len + 0.03, lerpf(0.06, 0.0, k))
	# stowed the pad folds flat against the hull; deployed it lies level
	foot.rotation = Vector3(lerpf(PI / 2, ang, k), 0, 0)
	# brace from the housing's lower end to mid-strut
	var a := Vector3(0, -0.4, 0.1)
	var m := pivot.position + pivot.basis * Vector3(0, -len * 0.6, 0)
	var brace: MeshInstance3D = root.get_node("Brace")
	var dir := m - a
	brace.transform = Transform3D(Basis(Quaternion(Vector3.UP, dir.normalized())).scaled(Vector3(1, dir.length(), 1)), (a + m) * 0.5)
	brace.visible = k > 0.05


static func _add_pick(root: Node3D, sh: Dictionary) -> void:
	var body := StaticBody3D.new()
	body.name = "Pick"
	body.collision_layer = PICK_LAYER
	body.collision_mask = 0
	var cs := CollisionShape3D.new()
	match sh.get("type", ""):
		"fin":
			var b := BoxShape3D.new()
			b.size = Vector3(0.08, sh.root, sh.span)
			cs.shape = b
			cs.position = Vector3(0, 0, sh.span * 0.5)
		"leg":
			var b := BoxShape3D.new()
			b.size = Vector3(0.3, 1.0, 0.3)
			cs.shape = b
			cs.position = Vector3(0, -0.3, 0.12)
		"pylon":
			var b := BoxShape3D.new()
			b.size = Vector3(sh.radius * 2.0, sh.height, sh.get("depth", 0.3))
			cs.shape = b
		"hose":
			var b := BoxShape3D.new()
			b.size = Vector3(0.2, 0.2, sh.get("length", 0.3))
			cs.shape = b
			cs.position = Vector3(0, 0, sh.get("length", 0.3) * 0.5)
		_:
			var c := CylinderShape3D.new()
			c.radius = sh.get("radius", sh.get("radius_bottom", 0.5))
			c.height = sh.get("height", 1.0)
			cs.shape = c
	body.add_child(cs)
	root.add_child(body)


## Interstage fairing heights per part index: a stack decoupler whose "top"
## node mates with an engine gets a shroud the height of that engine.
## parts: [{part, parent, node, parent_node}] (craft order).
static func shroud_heights(parts: Array) -> Dictionary:
	var out := {}
	for i in parts.size():
		var d: Dictionary = Sim.part(parts[i].part)
		if not d.has("decoupler") or d.decoupler.get("kind", "stack") != "stack":
			continue
		var nb := -1
		if int(parts[i].parent) >= 0 and parts[i].node == "top":
			nb = int(parts[i].parent)
		else:
			for k in range(i + 1, parts.size()):
				if int(parts[k].parent) == i and parts[k].parent_node == "top":
					nb = k
		if nb >= 0 and Sim.part(parts[nb].part).has("engine") and Sim.part(parts[nb].part).shape.type == "engine":
			out[i] = float(Sim.part(parts[nb].part).shape.height)
	return out


## Applies a material to every mesh under a node (ghosts, highlights).
static func paint(n: Node, m: Material) -> void:
	for c in n.get_children():
		if c is MeshInstance3D:
			(c as MeshInstance3D).material_override = m
		elif c is Decal:
			(c as Decal).visible = false
		paint(c, m)


static func xform(pos: Array, rot: Array) -> Transform3D:
	return Transform3D(Basis(DMath.to_quat(rot)), DMath.to_v3(pos))
