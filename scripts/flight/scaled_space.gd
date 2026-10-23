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
## Scaled space: every body drawn in its own World3D at 1/SCALE size, seen by
## a camera that copies the main camera's orientation. The result is the
## background behind the real-scale world (terrain, vessels). Positions are
## relative to the floating origin and divided down, so float32 never sees
## planetary distances.
class_name ScaledSpace
extends SubViewport

const SCALE := 1.0e-4
## Bodies farther than this (scaled units) are drawn as impostors pulled in to
## this distance at the same angular size, keeping depth ranges sane.
const IMPOSTOR_DIST := 2.0e4

var core: TrsSim
var cam: Camera3D
var _bodies := []  # [{node, atmo, info}]
var _env: Environment
var _sun_light: DirectionalLight3D


func setup(c: TrsSim) -> void:
	core = c
	own_world_3d = true
	transparent_bg = false
	render_target_update_mode = SubViewport.UPDATE_ALWAYS
	var we := WorldEnvironment.new()
	_env = Environment.new()
	_env.background_mode = Environment.BG_COLOR
	_env.background_color = Color(0.0, 0.0, 0.0)
	_env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	_env.ambient_light_color = Color(0.08, 0.09, 0.12)
	_env.glow_enabled = true
	_env.glow_intensity = 1.2
	_env.glow_bloom = 0.25
	_env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	we.environment = _env
	add_child(we)
	_sun_light = DirectionalLight3D.new()
	_sun_light.light_energy = 1.4
	add_child(_sun_light)
	cam = Camera3D.new()
	cam.near = 0.002
	cam.far = IMPOSTOR_DIST * 2.5
	add_child(cam)
	add_child(_starfield())
	for i in core.body_count():
		var info: Dictionary = JSON.parse_string(core.body_info(i))
		var node := MeshInstance3D.new()
		var sm := SphereMesh.new()
		sm.radius = 1.0
		sm.height = 2.0
		sm.radial_segments = 96
		sm.rings = 48
		node.mesh = sm
		var m := StandardMaterial3D.new()
		var atmo: MeshInstance3D = null
		match info.id:
			"austra":
				m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
				m.albedo_color = Color(1.0, 0.9, 0.7)
				m.emission_enabled = true
				m.emission = Color(1.0, 0.85, 0.6)
				m.emission_energy_multiplier = 6.0
			"floe":
				m.albedo_texture = _floe_texture()
				m.roughness = 0.6
				atmo = _atmosphere(Color(0.45, 0.7, 1.0), info.radius + info.atmosphere_height)
			"krill":
				m.albedo_texture = _krill_texture()
				m.roughness = 0.95
			_:
				m.albedo_color = Color(0.6, 0.6, 0.6)
		node.material_override = m
		add_child(node)
		if atmo:
			add_child(atmo)
		_bodies.append({"node": node, "atmo": atmo, "info": info})


func _atmosphere(col: Color, radius: float) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 1.0
	sm.height = 2.0
	sm.radial_segments = 64
	sm.rings = 32
	mi.mesh = sm
	var sh := Shader.new()
	sh.code = """
shader_type spatial;
render_mode unshaded, blend_add, cull_front, depth_draw_never;
uniform vec4 tint : source_color;
void fragment() {
	float rim = pow(1.0 - abs(dot(NORMAL, VIEW)), 2.5);
	ALBEDO = tint.rgb * rim * 1.6;
	ALPHA = rim;
}
"""
	var mat := ShaderMaterial.new()
	mat.shader = sh
	mat.set_shader_parameter("tint", col)
	mi.material_override = mat
	mi.set_meta("radius", radius)
	return mi


## Procedural textures keep the art pipeline-free (and original).
func _floe_texture() -> Texture2D:
	var img := Image.create(512, 256, false, Image.FORMAT_RGB8)
	var noise := FastNoiseLite.new()
	noise.seed = 7
	noise.frequency = 0.012
	noise.fractal_octaves = 5
	for y in 256:
		var lat := absf(float(y) / 255.0 - 0.5) * 2.0
		for x in 512:
			var n := noise.get_noise_3d(cos(x / 512.0 * TAU) * 80.0, sin(x / 512.0 * TAU) * 80.0, y * 1.0)
			var ice := Color(0.78, 0.86, 0.95).lerp(Color(0.97, 0.98, 1.0), clampf(n * 1.4 + 0.5, 0.0, 1.0))
			if n < -0.18 and lat < 0.8:
				ice = ice.lerp(Color(0.42, 0.6, 0.82), clampf(-n * 2.0 - 0.3, 0.0, 0.8))
			if lat > 0.82:
				ice = Color(0.98, 0.99, 1.0)
			img.set_pixel(x, y, ice)
	return ImageTexture.create_from_image(img)


func _krill_texture() -> Texture2D:
	var img := Image.create(512, 256, false, Image.FORMAT_RGB8)
	var noise := FastNoiseLite.new()
	noise.seed = 41
	noise.noise_type = FastNoiseLite.TYPE_CELLULAR
	noise.frequency = 0.03
	var base := FastNoiseLite.new()
	base.seed = 42
	base.frequency = 0.01
	for y in 256:
		for x in 512:
			var px := cos(x / 512.0 * TAU) * 80.0
			var pz := sin(x / 512.0 * TAU) * 80.0
			var c := noise.get_noise_3d(px, pz, y * 1.0)
			var b := base.get_noise_3d(px, pz, y * 1.0)
			var v := 0.48 + 0.18 * b + 0.12 * c
			img.set_pixel(x, y, Color(v, v * 0.98, v * 0.95))
	return ImageTexture.create_from_image(img)


func _starfield() -> Node3D:
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	var q := QuadMesh.new()
	q.size = Vector2(1, 1)
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.vertex_color_use_as_albedo = true
	m.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	q.material = m
	mm.mesh = q
	mm.instance_count = 2500
	var rng := RandomNumberGenerator.new()
	rng.seed = 2026
	for k in mm.instance_count:
		var d := Vector3(rng.randfn(), rng.randfn(), rng.randfn()).normalized() * IMPOSTOR_DIST * 2.0
		var s := rng.randf_range(25.0, 70.0)
		mm.set_instance_transform(k, Transform3D(Basis().scaled(Vector3(s, s, s)), d))
		var b := rng.randf_range(0.4, 1.0)
		mm.set_instance_color(k, Color(b, b, b * rng.randf_range(0.85, 1.1)))
	var mi := MultiMeshInstance3D.new()
	mi.multimesh = mm
	mi.name = "Stars"
	return mi


## Mirrors the main camera and places bodies around the scaled camera.
## sky: 0 = space, 1 = full daylight sky (inside an atmosphere).
func sync(main_cam: Camera3D, origin_id: int, cam_offset: Vector3, sun_dir: Vector3, sky: Color) -> void:
	size = main_cam.get_viewport().size
	cam.fov = main_cam.fov
	cam.global_transform = Transform3D(main_cam.global_transform.basis, Vector3.ZERO)
	var feed: PackedFloat64Array = core.body_render(origin_id)
	for i in _bodies.size():
		var o := i * 5
		# body centre relative to the camera, in scaled units
		var p := Frame.vec(feed, o) - cam_offset
		var b: Dictionary = _bodies[i]
		var r: float = feed[o + 4]
		var pos := p * SCALE
		var k := 1.0
		if pos.length() > IMPOSTOR_DIST:
			k = IMPOSTOR_DIST / pos.length()
		b.node.transform = Transform3D(Frame.spin(feed[o + 3]).scaled(Vector3.ONE * r * SCALE * k), pos * k)
		if b.atmo:
			b.atmo.position = pos * k
			b.atmo.scale = Vector3.ONE * float(b.atmo.get_meta("radius")) * SCALE * k
	_sun_light.global_transform = Transform3D(Basis.looking_at(-sun_dir, Vector3.UP if absf(sun_dir.y) < 0.99 else Vector3.RIGHT), Vector3.ZERO)
	_env.background_color = sky
	var stars: Node3D = get_node("Stars")
	stars.visible = sky.v < 0.35
