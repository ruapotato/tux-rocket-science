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
## A part going up: white flash, billowing fireball that cools to smoke, a
## shock ring, burning sparks, tumbling debris and a lingering smoke column.
## Purely visual. `size` is the fireball radius in metres (bigger for parts
## that still had propellant); `up` is the local vertical.
class_name Explosion
extends Node3D

const FIRE_SHADER := """
shader_type spatial;
render_mode unshaded, cull_back, depth_draw_never, shadows_disabled;
uniform float age = 0.0;     // 0 (bang) .. 1 (burnt out)
uniform float seed = 0.0;
varying vec3 lp;
float h3(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
float noise(vec3 x) {
	vec3 i = floor(x); vec3 f = fract(x); f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(h3(i), h3(i + vec3(1,0,0)), f.x), mix(h3(i + vec3(0,1,0)), h3(i + vec3(1,1,0)), f.x), f.y),
		mix(mix(h3(i + vec3(0,0,1)), h3(i + vec3(1,0,1)), f.x), mix(h3(i + vec3(0,1,1)), h3(i + vec3(1,1,1)), f.x), f.y), f.z);
}
float fbm(vec3 p) { return noise(p) * 0.55 + noise(p * 2.1) * 0.3 + noise(p * 4.3) * 0.15; }
void vertex() {
	lp = VERTEX;
	float n = fbm(VERTEX * 2.2 + vec3(seed, seed * 1.7, TIME * 0.6));
	VERTEX += NORMAL * (n - 0.45) * 0.55;
}
void fragment() {
	float n = fbm(lp * 3.1 + vec3(seed, -TIME * 0.9, seed * 0.5));
	float facing = clamp(dot(NORMAL, VIEW), 0.0, 1.0);
	float heat = clamp(1.15 - age * 1.7 + (n - 0.5) * 0.9 - (1.0 - facing) * 0.5, 0.0, 1.0);
	vec3 fire = mix(vec3(0.75, 0.13, 0.01), vec3(1.0, 0.55, 0.12), heat * heat) * (0.9 + 1.4 * heat * heat);
	vec3 smoke = mix(vec3(0.06, 0.05, 0.045), vec3(0.25, 0.23, 0.21), n);
	ALBEDO = mix(smoke, fire, smoothstep(0.1, 0.5, heat));
	ALPHA = clamp((1.0 - age * age) * (0.7 + 0.6 * n) * smoothstep(0.0, 0.35, facing) * 1.8, 0.0, 1.0);
}
"""

const RING_SHADER := """
shader_type spatial;
render_mode unshaded, blend_add, cull_disabled, depth_draw_never, shadows_disabled;
uniform float age = 0.0;
varying vec2 uvp;
void vertex() { uvp = VERTEX.xz; }
void fragment() {
	float r = length(uvp) * 2.0;           // 0 centre .. 1 rim of the quad
	float band = smoothstep(0.72, 0.95, r) * (1.0 - smoothstep(0.95, 1.0, r));
	ALBEDO = vec3(1.0, 0.8, 0.55) * band * (1.0 - age) * 1.6;
}
"""

static var _fire_shader: Shader
static var _ring_shader: Shader
static var _dot: Texture2D

var size := 4.0
var up := Vector3.UP
var _t := 0.0
var _balls: Array = []   # {mesh, mat, offset: Vector3, grow}
var _ring: MeshInstance3D
var _ring_mat: ShaderMaterial
var _light: OmniLight3D
var _debris: Array = []  # {node, v, w}
var _rng := RandomNumberGenerator.new()

const LIFE := 14.0


func _init(s := 4.0, up_dir := Vector3.UP) -> void:
	size = s
	up = up_dir.normalized() if up_dir.length() > 0.5 else Vector3.UP


func _ready() -> void:
	_rng.randomize()
	if _fire_shader == null:
		_fire_shader = Shader.new()
		_fire_shader.code = FIRE_SHADER
		_ring_shader = Shader.new()
		_ring_shader.code = RING_SHADER
		_dot = ReentryFX._soft_dot()
	# billowing fireball: several noisy spheres with their own drift
	for k in 6:
		var mi := MeshInstance3D.new()
		var sm := SphereMesh.new()
		sm.radius = 1.0
		sm.height = 2.0
		sm.radial_segments = 32
		sm.rings = 16
		mi.mesh = sm
		var m := ShaderMaterial.new()
		m.shader = _fire_shader
		m.set_shader_parameter("seed", _rng.randf() * 50.0)
		mi.material_override = m
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		var off := Vector3(_rng.randf_range(-1, 1), _rng.randf_range(-0.4, 1), _rng.randf_range(-1, 1)) * 0.55
		if k == 0:
			off = Vector3.ZERO
		add_child(mi)
		_balls.append({"mesh": mi, "mat": m, "offset": off, "grow": _rng.randf_range(0.7, 1.15), "delay": k * 0.04})
	# shock ring, flat in the local horizontal
	_ring = MeshInstance3D.new()
	var q := QuadMesh.new()
	q.orientation = PlaneMesh.FACE_Y
	q.size = Vector2.ONE
	_ring.mesh = q
	_ring_mat = ShaderMaterial.new()
	_ring_mat.shader = _ring_shader
	_ring.material_override = _ring_mat
	_ring.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_ring.basis = _up_basis()
	add_child(_ring)
	_light = OmniLight3D.new()
	_light.light_color = Color(1.0, 0.7, 0.4)
	_light.omni_range = size * 9.0
	_light.light_energy = 24.0
	_light.shadow_enabled = false
	add_child(_light)
	add_child(_sparks())
	# the lingering cloud is laid into the scene's SmokeTrail (burst()), which
	# drifts with the air in sim time; this is just the first dark billow
	var sm := _smoke()
	sm.amount = int(clampf(size * 4.0, 12.0, 40.0))
	sm.lifetime = 4.0
	add_child(sm)
	for k in int(clampf(size * 2.0, 6.0, 22.0)):
		_add_debris()


## Another part went up right here at the same moment: one bigger blast.
func merge(other_size: float) -> void:
	size = pow(pow(size, 3.0) + pow(other_size, 3.0), 1.0 / 3.0)
	_light.omni_range = size * 9.0
	for k in 3:
		_add_debris()


func young() -> bool:
	return _t < 0.25


func _up_basis() -> Basis:
	var x := up.cross(Vector3.FORWARD if absf(up.dot(Vector3.FORWARD)) < 0.9 else Vector3.RIGHT).normalized()
	return Basis(x, up, x.cross(up))


func _sparks() -> GPUParticles3D:
	var p := GPUParticles3D.new()
	p.amount = int(clampf(size * 40.0, 120.0, 500.0))
	p.lifetime = 2.6
	p.one_shot = true
	p.explosiveness = 0.95
	p.local_coords = true
	var pm := ParticleProcessMaterial.new()
	pm.direction = up
	pm.spread = 180.0
	pm.initial_velocity_min = 12.0 * sqrt(size)
	pm.initial_velocity_max = 34.0 * sqrt(size)
	pm.gravity = -up * 9.8
	pm.damping_min = 2.0
	pm.damping_max = 6.0
	pm.particle_flag_align_y = true
	pm.scale_min = 0.6
	pm.scale_max = 1.4
	var g := Gradient.new()
	g.set_color(0, Color(1.0, 0.95, 0.75, 1.0))
	g.add_point(0.4, Color(1.0, 0.55, 0.15, 1.0))
	g.set_color(g.get_point_count() - 1, Color(0.4, 0.08, 0.02, 0.0))
	var gt := GradientTexture1D.new()
	gt.gradient = g
	pm.color_ramp = gt
	p.process_material = pm
	var quad := QuadMesh.new()
	quad.size = Vector2(0.18, 1.3)
	var qm := StandardMaterial3D.new()
	qm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	qm.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	qm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	qm.vertex_color_use_as_albedo = true
	qm.billboard_mode = BaseMaterial3D.BILLBOARD_FIXED_Y
	qm.billboard_keep_scale = true
	qm.albedo_texture = _dot
	quad.material = qm
	p.draw_pass_1 = quad
	p.emitting = true
	return p


func _smoke() -> GPUParticles3D:
	var p := GPUParticles3D.new()
	p.amount = int(clampf(size * 10.0, 30.0, 120.0))
	p.lifetime = 11.0
	p.one_shot = true
	p.explosiveness = 0.55
	p.local_coords = true
	p.visibility_aabb = AABB(Vector3.ONE * -size * 12.0, Vector3.ONE * size * 24.0)
	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE
	pm.emission_sphere_radius = size * 0.6
	pm.direction = up
	pm.spread = 50.0
	pm.initial_velocity_min = 1.0 * sqrt(size)
	pm.initial_velocity_max = 4.0 * sqrt(size)
	pm.gravity = up * 1.6   # hot smoke rises
	pm.damping_min = 0.4
	pm.damping_max = 1.0
	pm.scale_min = size * 2.0
	pm.scale_max = size * 3.8
	var sc := Curve.new()  # 0..1 growth into the size range
	sc.add_point(Vector2(0.0, 0.15))
	sc.add_point(Vector2(0.25, 0.4))
	sc.add_point(Vector2(1.0, 1.0))
	var sct := CurveTexture.new()
	sct.curve = sc
	pm.scale_curve = sct
	var g := Gradient.new()
	g.set_color(0, Color(0.45, 0.22, 0.08, 0.0))
	g.add_point(0.06, Color(0.22, 0.17, 0.14, 0.85))
	g.add_point(0.5, Color(0.3, 0.29, 0.29, 0.55))
	g.set_color(g.get_point_count() - 1, Color(0.42, 0.42, 0.44, 0.0))
	var gt := GradientTexture1D.new()
	gt.gradient = g
	pm.color_ramp = gt
	p.process_material = pm
	var quad := QuadMesh.new()
	quad.size = Vector2.ONE
	var qm := StandardMaterial3D.new()
	qm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	qm.vertex_color_use_as_albedo = true
	qm.billboard_mode = BaseMaterial3D.BILLBOARD_PARTICLES
	qm.billboard_keep_scale = true  # or the particle's own scale is dropped
	qm.albedo_texture = _dot
	qm.roughness = 1.0
	qm.shading_mode = BaseMaterial3D.SHADING_MODE_PER_VERTEX
	quad.material = qm
	p.draw_pass_1 = quad
	p.emitting = true
	return p


func _add_debris() -> void:
	var mi := MeshInstance3D.new()
	var b := BoxMesh.new()
	var s := _rng.randf_range(0.15, 0.6) * clampf(size / 5.0, 0.5, 1.6)
	b.size = Vector3(s, s * _rng.randf_range(0.2, 1.0), s * _rng.randf_range(0.5, 1.5))
	mi.mesh = b
	var light: bool = _rng.randf() < 0.5
	mi.material_override = PartVisual.mat("debris_w" if light else "debris_d",
		Color(0.85, 0.85, 0.87) if light else Color(0.12, 0.12, 0.13), 0.2, 0.6)
	add_child(mi)
	var dir := (Vector3(_rng.randf_range(-1, 1), _rng.randf_range(-1, 1), _rng.randf_range(-1, 1)) + up * 0.8).normalized()
	_debris.append({"node": mi, "v": dir * _rng.randf_range(8.0, 30.0) * sqrt(size / 4.0),
		"w": Vector3(_rng.randf_range(-1, 1), _rng.randf_range(-1, 1), _rng.randf_range(-1, 1)) * 8.0, "rest": false})


func _process(delta: float) -> void:
	_t += delta
	var fire_life := 1.6 + size * 0.12
	for b in _balls:
		var tt: float = maxf(_t - b.delay, 0.0)
		var age := clampf(tt / fire_life, 0.0, 1.0)
		var r: float = size * b.grow * (0.25 + 1.1 * (1.0 - exp(-tt * 3.5)) + 0.25 * age)
		var mi: MeshInstance3D = b.mesh
		mi.visible = age < 1.0
		mi.scale = Vector3.ONE * r
		# the fireball lifts as it burns, like a hot bubble
		mi.position = (b.offset as Vector3) * size + up * size * 0.9 * age * age
		(b.mat as ShaderMaterial).set_shader_parameter("age", age)
	var ra := clampf(_t / 0.7, 0.0, 1.0)
	_ring.scale = Vector3.ONE * size * (1.0 + 9.0 * ra)
	_ring.visible = ra < 1.0
	_ring_mat.set_shader_parameter("age", ra)
	_light.light_energy = 24.0 * exp(-_t * 2.2)
	_light.visible = _light.light_energy > 0.05
	for d in _debris:
		if d.rest:
			continue
		var n: MeshInstance3D = d.node
		d.v = (d.v as Vector3) - up * 9.8 * delta
		n.position += (d.v as Vector3) * delta
		n.rotate((d.w as Vector3).normalized(), (d.w as Vector3).length() * delta)
		# crude ground: stop once it falls back below the blast point
		if n.position.dot(up) < -0.5 and (d.v as Vector3).dot(up) < 0.0:
			d.rest = true
	if _t > LIFE:
		queue_free()
