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
## Reentry plasma. The glow is a sheath that hugs each part's own shape
## (its meshes, inflated a little and drawn with SHEATH below): bright where
## a face meets the oncoming air, a thin film streaming down the sides, and
## nothing on faces another part shields. Each exposed leading tip (the
## nose, a side booster's cone) streams its own trail. Scaled by the
## convective heating proxy rho * v^3. Purely visual; the heating itself is
## simulated per part in trs_core.
class_name ReentryFX
extends Node3D

const SHEATH := """
shader_type spatial;
render_mode unshaded, blend_add, cull_back, depth_draw_never, shadows_disabled;
uniform float intensity = 0.0;
uniform vec3 flow = vec3(0.0, -1.0, 0.0);   // world: the way the air streams past
uniform float inflate = 0.08;               // m the sheath stands off the hull
uniform float stretch = 1.5;                // m the lee side is dragged downstream
instance uniform float exposure = 1.0;      // 0: another part takes the air first
varying float face;
varying vec3 wpos;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void vertex() {
	vec3 wn = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	face = dot(wn, -flow);
	vec3 d = wn * inflate * (0.5 + exposure * max(face, 0.0)) + flow * max(-face, 0.0) * stretch;
	VERTEX += inverse(mat3(MODEL_MATRIX)) * d;
	wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}
void fragment() {
	float cap = pow(clamp(face, 0.0, 1.0), 1.5) * exposure;
	float film = (1.0 - abs(face)) * 0.3;
	float edge = pow(1.0 - abs(dot(NORMAL, VIEW)), 2.0);
	// streaks: noise across the flow, scrolling down it
	vec3 side = normalize(cross(flow, vec3(0.31, 0.83, 0.47)));
	float lane = hash(vec2(floor(dot(wpos, side) * 9.0), floor(dot(wpos, flow) * 2.0 - TIME * 40.0)));
	float flick = 0.8 + 0.2 * hash(vec2(floor(TIME * 40.0), 3.0));
	float g = (cap * (0.6 + edge) + film * (0.4 + lane)) * flick;
	vec3 col = mix(vec3(1.0, 0.38, 0.08), vec3(1.0, 0.85, 0.6), clamp(cap, 0.0, 1.0));
	ALBEDO = col * g * pow(intensity, 0.6) * 2.4;
}
"""

const MAX_TRAILS := 8

static var _sheath_mat: ShaderMaterial

var _light: OmniLight3D
var _trails: Array = []  # GPUParticles3D, one per exposed tip
var level := 0.0


## The one material every part's sheath shares (flow and level set here).
static func sheath_material() -> ShaderMaterial:
	if _sheath_mat == null:
		_sheath_mat = ShaderMaterial.new()
		var sh := Shader.new()
		sh.code = SHEATH
		_sheath_mat.shader = sh
	return _sheath_mat


func _ready() -> void:
	_light = OmniLight3D.new()
	_light.light_color = Color(1.0, 0.55, 0.25)
	_light.omni_range = 30.0
	add_child(_light)
	visible = false


## Streaming plasma trail from one tip: render space is centred on the
## vessel (floating origin), so particles just stream downstream.
func _make_trail() -> GPUParticles3D:
	var trail := GPUParticles3D.new()
	trail.amount = 900
	trail.lifetime = 0.35
	trail.local_coords = false
	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE_SURFACE
	pm.emission_sphere_radius = 1.0
	pm.spread = 8.0
	pm.gravity = Vector3.ZERO
	pm.scale_min = 0.9
	pm.scale_max = 1.8
	pm.particle_flag_align_y = true
	var grad := Gradient.new()
	grad.set_color(0, Color(1.0, 0.9, 0.7, 1.0))
	grad.add_point(0.35, Color(1.0, 0.5, 0.15, 0.8))
	grad.set_color(grad.get_point_count() - 1, Color(0.8, 0.2, 0.05, 0.0))
	var gt := GradientTexture1D.new()
	gt.gradient = grad
	pm.color_ramp = gt
	var sc := Curve.new()  # 0..1 growth into the scale range
	sc.add_point(Vector2(0.0, 0.28))
	sc.add_point(Vector2(0.3, 0.55))
	sc.add_point(Vector2(1.0, 1.0))
	var sct := CurveTexture.new()
	sct.curve = sc
	pm.scale_curve = sct
	trail.process_material = pm
	var quad := QuadMesh.new()
	quad.size = Vector2(0.32, 2.4)
	var qm := StandardMaterial3D.new()
	qm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	qm.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	qm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	qm.vertex_color_use_as_albedo = true
	qm.billboard_mode = BaseMaterial3D.BILLBOARD_FIXED_Y
	qm.billboard_keep_scale = true
	qm.albedo_texture = _soft_dot()
	quad.material = qm
	trail.draw_pass_1 = quad
	trail.emitting = false
	add_child(trail)
	return trail


## air_vel: velocity of the vessel relative to the air (Godot axes, m/s);
## rho: air density; fronts: the exposed leading tips, [[point, hull radius
## there], ...] in render space, the most windward first.
func update_fx(air_vel: Vector3, rho: float, fronts: Array, delta: float) -> void:
	var v := air_vel.length()
	# rho*v^3 in W/m^2-ish units; visible from ~1.5 km/s in the upper air
	var q := rho * v * v * v
	var target := clampf((q - 2.0e6) / 1.2e8, 0.0, 1.0)
	level = move_toward(level, target, delta * 1.5)
	visible = level > 0.01
	var mat := sheath_material()
	mat.set_shader_parameter("intensity", level if visible else 0.0)
	# the air streams past opposite to our motion through it
	var flow := -air_vel.normalized() if v > 1.0 else Vector3.DOWN
	mat.set_shader_parameter("flow", flow)
	mat.set_shader_parameter("inflate", 0.05 + 0.12 * level)
	mat.set_shader_parameter("stretch", 0.5 + 2.5 * level)
	for k in MAX_TRAILS:
		var on := visible and level > 0.05 and k < fronts.size()
		if k >= _trails.size():
			if not on:
				continue
			_trails.append(_make_trail())
		var trail: GPUParticles3D = _trails[k]
		trail.emitting = on
		if not on:
			continue
		var at: Vector3 = fronts[k][0]
		var r: float = fronts[k][1]
		var pm: ParticleProcessMaterial = trail.process_material
		trail.position = at + flow * r * 0.3
		pm.direction = flow
		pm.emission_sphere_radius = r * 0.9
		trail.amount_ratio = clampf(level * 1.5 * clampf(r / 0.6, 0.3, 1.0), 0.05, 1.0)
		pm.initial_velocity_min = 25.0 + 30.0 * level
		pm.initial_velocity_max = 45.0 + 50.0 * level
	if not visible or fronts.is_empty():
		_light.light_energy = 0.0
		return
	_light.position = fronts[0][0] - flow * fronts[0][1] * 0.5
	_light.light_energy = 6.0 * level


static func _soft_dot() -> Texture2D:
	var img := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	for y in 32:
		for x in 32:
			var d := Vector2(x - 15.5, y - 15.5).length() / 16.0
			var a := clampf(1.0 - d, 0.0, 1.0)
			img.set_pixel(x, y, Color(1, 1, 1, a * a))
	return ImageTexture.create_from_image(img)
