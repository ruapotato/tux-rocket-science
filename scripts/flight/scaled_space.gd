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
## Bodies further than this are drawn shrunk to it (same look, nearer);
## the stars sit just behind, at STAR_DIST. With the near plane at 0.002
## the depth buffer can't tell anything much past 2e4 from the cleared sky
## (the stars there failed the depth test and vanished), so both stay well
## inside that.
const IMPOSTOR_DIST := 8.0e3
const STAR_DIST := 1.0e4

var core: TrsSim
var cam: Camera3D
var _bodies := []  # [{node, atmo, info}]
var _env: Environment
var _sun_light: DirectionalLight3D
var _sky_mat: ShaderMaterial

const SKY_SHADER := """
shader_type sky;
uniform vec3 up_dir = vec3(0.0, 1.0, 0.0);
uniform vec3 sun_dir = vec3(0.0, 1.0, 0.0);
uniform float density = 0.0;   // 1 at sea level, 0 in space
uniform vec4 zenith : source_color = vec4(0.10, 0.30, 0.74, 1.0);
uniform vec4 horizon : source_color = vec4(0.60, 0.76, 0.95, 1.0);
void sky() {
	float h = dot(EYEDIR, up_dir);
	float sun_h = dot(sun_dir, up_dir);
	float day = smoothstep(-0.18, 0.12, sun_h);
	vec3 col = mix(horizon.rgb, zenith.rgb, pow(clamp(h, 0.0, 1.0), 0.45));
	col = h < 0.0 ? horizon.rgb * 0.85 : col;
	// warm band near the horizon when the sun is low
	float low = 1.0 - smoothstep(0.0, 0.35, abs(sun_h));
	float toward = pow(max(dot(normalize(EYEDIR - up_dir * h), normalize(sun_dir - up_dir * sun_h)), 0.0), 2.0);
	col = mix(col, vec3(1.0, 0.55, 0.28), low * toward * (1.0 - smoothstep(0.0, 0.3, abs(h))) * 0.75);
	col *= mix(0.03, 1.0, day);
	float sd = dot(EYEDIR, sun_dir);
	vec3 glow = vec3(1.0, 0.9, 0.75) * (pow(max(sd, 0.0), 300.0) * 1.5 + pow(max(sd, 0.0), 12.0) * 0.25 * density);
	vec3 disk = vec3(1.0, 0.95, 0.85) * smoothstep(0.99985, 0.99995, sd) * 8.0;
	COLOR = col * density + glow + disk;
}
"""


func setup(c: TrsSim) -> void:
	core = c
	own_world_3d = true
	transparent_bg = false
	render_target_update_mode = SubViewport.UPDATE_ALWAYS
	var we := WorldEnvironment.new()
	_env = Environment.new()
	_env.background_mode = Environment.BG_SKY
	# the sky is a backdrop, not a light source: no mirrored sky on planets
	_env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var sky := Sky.new()
	_sky_mat = ShaderMaterial.new()
	var ssh := Shader.new()
	ssh.code = SKY_SHADER
	_sky_mat.shader = ssh
	sky.sky_material = _sky_mat
	_env.sky = sky
	_env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	_env.ambient_light_color = Color(0.03, 0.035, 0.05)
	_env.glow_enabled = true
	_env.glow_intensity = 1.2
	_env.glow_bloom = 0.0
	_env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	we.environment = _env
	add_child(we)
	_sun_light = DirectionalLight3D.new()
	_sun_light.light_energy = 1.4
	add_child(_sun_light)
	cam = Camera3D.new()
	cam.near = 0.002
	cam.far = STAR_DIST * 1.5
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
			_:
				m.albedo_color = Color(0.6, 0.6, 0.6)
				if float(info.get("atmosphere_height", 0.0)) > 0.0:
					atmo = Atmosphere.make(info.radius, info.atmosphere_height, info.get("scale_height", 5800.0), Sim.look(info.id).get("tau", []))
		node.material_override = m if info.id == "austra" else PlanetBake.planet_material(core, i)
		add_child(node)
		if atmo:
			add_child(atmo)
		var clouds: MeshInstance3D = null
		if Sim.look(info.id).get("clouds", false):
			clouds = MeshInstance3D.new()
			var cm := SphereMesh.new()
			cm.radius = 1.0
			cm.height = 2.0
			cm.radial_segments = 96
			cm.rings = 48
			clouds.mesh = cm
			clouds.material_override = PlanetBake.cloud_material()
			clouds.set_meta("radius", info.radius + 7000.0)
			add_child(clouds)
		_bodies.append({"node": node, "atmo": atmo, "clouds": clouds, "info": info})


static func _atmosphere(col: Color, radius: float) -> MeshInstance3D:
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
static func _floe_texture() -> Texture2D:
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


static func _krill_texture() -> Texture2D:
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


## The stars, as light from far away seen by a moving ship: each keeps its
## true (rest-frame) direction and temperature, and the shader shows it
## where and how a ship moving at beta (a fraction of light speed) would
## see it. Aberration crowds the sky toward the motion; the Doppler factor
## D = gamma (1 + beta cos theta) shifts each star's light, whose visible
## flux (per colour, for a blackbody point source) is D^3 B(nu/D) / B(nu)
## = (e^x - 1) / (e^(x/D) - 1), x = h nu / k T: ahead stars turn blue and
## brighten, behind they redden and fade out. Instances 0.. are the real
## stars (placed each frame); the rest are a fixed backdrop.
const STAR_SHADER := """
shader_type spatial;
render_mode unshaded, world_vertex_coords, cull_disabled, blend_add, depth_draw_never, fog_disabled, shadows_disabled;
uniform vec3 motion_dir = vec3(0.0, 0.0, -1.0);
uniform float beta = 0.0;
uniform float gamma = 1.0;
uniform float radius = 40000.0;
uniform float fade = 1.0;
uniform vec4 real_dir[8];   // xyz: direction to a real star, w: its temperature
uniform vec4 real_col[8];   // rgb: its rest colour x brightness, a: dot size
varying vec3 col;
void vertex() {
	vec4 cu = CUSTOM0;
	vec4 base = COLOR;
	int real = -1;
	if (cu.w < 0.0) {  // one of the real stars, placed each frame
		real = int(-cu.w + 0.5) - 1;
		cu = real_dir[real];
		base = vec4(real_col[real].rgb, 1.0);
	}
	vec3 n = normalize(cu.xyz);
	float T = max(cu.w, 500.0);
	vec3 dir = n;
	float D = 1.0;
	if (beta > 1e-9) {
		float c = dot(n, motion_dir);
		float c2 = (c + beta) / (1.0 + beta * c);
		vec3 perp = n - c * motion_dir;
		float pl = length(perp);
		vec3 pu = pl > 1e-9 ? perp / pl : vec3(0.0);
		dir = motion_dir * c2 + pu * sqrt(max(1.0 - c2 * c2, 0.0));
		D = gamma * (1.0 + beta * c);
	}
	// h c / (lambda k) for 610, 550 and 465 nm, in kelvin
	vec3 x = vec3(23586.0, 26159.0, 30941.0) / T;
	vec3 ratio = (exp(min(x, vec3(80.0))) - 1.0) / max(exp(min(x / max(D, 1e-4), vec3(80.0))) - 1.0, vec3(1e-30));
	vec3 c3 = base.rgb * ratio;
	float lum = dot(c3, vec3(0.3, 0.59, 0.11));
	float lum0 = max(dot(base.rgb, vec3(0.3, 0.59, 0.11)), 1e-4);
	// the dot's size rides in UV2 (vertex colours are 8-bit); a real star's in its uniform
	float gain = max(lum, 0.0) / lum0;  // how much brighter (or dimmer) the shift made it
	float size = (real >= 0 ? real_col[real].a : 1.0) * clamp(pow(gain, 0.15), 0.5, 2.2);
	float m = max(c3.r, max(c3.g, c3.b));
	// its hue, at a brightness that saturates gently (so a crowd of them
	// ahead keeps its blue instead of burning white)
	col = (m > 0.0 ? c3 / m : c3) * (lum / (0.6 + lum)) * fade;
	vec3 right = INV_VIEW_MATRIX[0].xyz;
	vec3 up = INV_VIEW_MATRIX[1].xyz;
	// sized in pixels (UV2 holds the corner times the star's size in px),
	// so a star is a dot however far the backdrop sphere is
	float px = 2.0 * radius / (PROJECTION_MATRIX[1][1] * VIEWPORT_SIZE.y);
	VERTEX = dir * radius + (right * UV2.x + up * UV2.y) * size * px;
}
void fragment() {
	float r = length(UV - vec2(0.5)) * 2.0;
	float a = smoothstep(1.0, 0.0, r);
	ALBEDO = col * a * a * 2.0;
}
"""
const BACKDROP := 2600
const STAR_TEMP_BY_ID := {"austra": 5800.0, "lantern": 4600.0}

var _star_mat: ShaderMaterial
var _real_stars: Array = []  # [{index, temp}] bodies that are stars


## A still backdrop of stars (the main menu).
static func backdrop_stars() -> Node3D:
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



## The rest-frame colour of a blackbody at T (rough), for the backdrop.
static func _bb_color(T: float) -> Color:
	var x := Vector3(23586.0, 26159.0, 30941.0) / T
	var r := 1.0 / (exp(x.x) - 1.0) * pow(610.0, -3.0)
	var g := 1.0 / (exp(x.y) - 1.0) * pow(550.0, -3.0)
	var b := 1.0 / (exp(x.z) - 1.0) * pow(465.0, -3.0)
	var m := maxf(r, maxf(g, b))
	return Color(r / m, g / m, b / m)


func _starfield() -> Node3D:
	_star_mat = ShaderMaterial.new()
	var sh := Shader.new()
	sh.code = STAR_SHADER
	_star_mat.shader = sh
	_star_mat.set_shader_parameter("radius", STAR_DIST)
	for i in core.body_count():
		var info: Dictionary = JSON.parse_string(core.body_info(i))
		if int(info.parent) < 0 and _real_stars.size() < 8:
			_real_stars.append({"index": i, "temp": float(STAR_TEMP_BY_ID.get(info.id, 5500.0))})
	# one quad per star: corners in UV2, the star in CUSTOM0 (direction,
	# temperature; w < 0 picks a real star from the uniforms) and COLOR
	var verts := PackedVector3Array()
	var uv := PackedVector2Array()
	var uv2 := PackedVector2Array()
	var cols := PackedColorArray()
	var cust := PackedFloat32Array()
	var idx := PackedInt32Array()
	var rng := RandomNumberGenerator.new()
	rng.seed = 2026
	var n := _real_stars.size() + BACKDROP
	for k in n:
		var c: Color
		var cu: Color
		var size := 1.0
		if k < _real_stars.size():
			c = Color(0, 0, 0, 0)
			cu = Color(0, 0, -1, -(k + 1))
		else:
			var d := Vector3(rng.randfn(), rng.randfn(), rng.randfn()).normalized()
			var T := 2800.0 + pow(rng.randf(), 2.2) * 14000.0
			var b := pow(rng.randf(), 2.0) * 0.8 + 0.2
			c = _bb_color(T) * b
			c.a = 1.0
			cu = Color(d.x, d.y, d.z, T)
			size = rng.randf_range(2.0, 3.2) * (0.6 + 0.6 * b)  # px
		var base := verts.size()
		for corner in [Vector2(-0.5, -0.5), Vector2(0.5, -0.5), Vector2(0.5, 0.5), Vector2(-0.5, 0.5)]:
			verts.append(Vector3.ZERO)
			uv.append(corner + Vector2(0.5, 0.5))
			uv2.append(corner * size)
			cols.append(c)
			cust.append_array([cu.r, cu.g, cu.b, cu.a])
		idx.append_array([base, base + 1, base + 2, base, base + 2, base + 3])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_TEX_UV] = uv
	arrays[Mesh.ARRAY_TEX_UV2] = uv2
	arrays[Mesh.ARRAY_COLOR] = cols
	arrays[Mesh.ARRAY_CUSTOM0] = cust
	arrays[Mesh.ARRAY_INDEX] = idx
	var am := ArrayMesh.new()
	am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM0_SHIFT)
	am.surface_set_material(0, _star_mat)
	var mi := MeshInstance3D.new()
	mi.mesh = am
	mi.name = "Stars"
	mi.custom_aabb = AABB(Vector3.ONE * -1e6, Vector3.ONE * 2e6)  # the shader moves them
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return mi


## The ship's motion through the stars' frame (unit direction in Godot
## axes, speed as a fraction of light, gamma) and where the real stars are
## from it (the body_render feed): far ones show as points of light.
func set_motion(dir: Vector3, b: float, g: float, feed: PackedFloat64Array) -> void:
	_star_mat.set_shader_parameter("motion_dir", dir if dir.length() > 0.5 else Vector3(0, 0, -1))
	_star_mat.set_shader_parameter("beta", b)
	_star_mat.set_shader_parameter("gamma", g)
	var dirs: Array[Vector4] = []
	var cols: Array[Vector4] = []
	for k in 8:
		if k >= _real_stars.size():
			dirs.append(Vector4(0, 0, -1, 5000))
			cols.append(Vector4.ZERO)
			continue
		var st: Dictionary = _real_stars[k]
		var dd: Array = Frame.dist_dir(feed, int(st.index) * 5)
		var dist: float = dd[0]
		# within about 1e12 m the star's own sphere takes over
		var seen := clampf(log(dist / 1.0e12) / log(10.0) / 2.0, 0.0, 1.0)
		var bright := clampf(pow(9.46e15 / maxf(dist, 1.0), 0.5), 0.6, 3.0)
		var c := _bb_color(float(st.temp)) * (seen * minf(bright, 1.0))
		var dv: Vector3 = dd[1]
		dirs.append(Vector4(dv.x, dv.y, dv.z, float(st.temp)))
		cols.append(Vector4(c.r, c.g, c.b, 4.0 * bright * seen))  # px
	_star_mat.set_shader_parameter("real_dir", dirs)
	_star_mat.set_shader_parameter("real_col", cols)


## Mirrors the main camera and places bodies around the scaled camera.
## sky: 0 = space, 1 = full daylight sky (inside an atmosphere).
## density: 1 at the ground of an atmosphere, 0 in space. up: local up.
func sync(main_cam: Camera3D, origin_id: int, cam_offset: Vector3, sun_dir: Vector3, density: float, up := Vector3.UP) -> void:
	size = main_cam.get_viewport().size
	cam.fov = main_cam.fov
	cam.global_transform = Transform3D(main_cam.global_transform.basis, Vector3.ZERO)
	var feed: PackedFloat64Array = core.body_render(origin_id)
	for i in _bodies.size():
		var o := i * 5
		# body centre relative to the camera, in scaled units (distance in
		# doubles: between the stars it is far past what float32 can square)
		var dd: Array = Frame.dist_dir(feed, o, cam_offset)
		var b: Dictionary = _bodies[i]
		var r: float = feed[o + 4]
		var dist: float = dd[0] * SCALE
		var k := 1.0
		if dist > IMPOSTOR_DIST:
			k = IMPOSTOR_DIST / dist
		var pos: Vector3 = dd[1] * (dist * k)
		var size: float = r * SCALE * k
		b.node.visible = size > 1e-6  # far past seeing: a body light-years off
		b.node.transform = Transform3D(Frame.spin(feed[o + 3]).scaled(Vector3.ONE * maxf(size, 1e-6)), pos)
		if b.atmo:
			b.atmo.visible = b.node.visible
			b.atmo.position = pos
			b.atmo.scale = Vector3.ONE * maxf(float(b.atmo.get_meta("radius")) * SCALE * k, 1e-6)
			Atmosphere.set_sun(b.atmo, sun_dir)
			# deep inside the air the sky shader takes over
			Atmosphere.set_intensity(b.atmo, 1.0 - smoothstep(0.25, 0.7, density))
		if b.clouds:
			# clouds are for looking down on: fade them out once below the deck
			var op := 1.0 - smoothstep(0.42, 0.62, density)
			b.clouds.visible = op > 0.01
			(b.clouds.material_override as ShaderMaterial).set_shader_parameter("opacity", op)
			b.clouds.visible = b.clouds.visible and b.node.visible
			b.clouds.transform = Transform3D(Frame.spin(feed[o + 3]).scaled(Vector3.ONE * maxf(float(b.clouds.get_meta("radius")) * SCALE * k, 1e-6)), pos)
	_sun_light.global_transform = Transform3D(Basis.looking_at(-sun_dir, Vector3.UP if absf(sun_dir.y) < 0.99 else Vector3.RIGHT), Vector3.ZERO)
	_sky_mat.set_shader_parameter("up_dir", up)
	_sky_mat.set_shader_parameter("sun_dir", sun_dir)
	_sky_mat.set_shader_parameter("density", density)
	var stars: Node3D = get_node("Stars")
	stars.visible = density < 0.25 or up.dot(sun_dir) < -0.15
	_star_mat.set_shader_parameter("fade", 1.0 - smoothstep(0.05, 0.25, density))
