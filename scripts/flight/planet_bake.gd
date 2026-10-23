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
## Planet textures for scaled space, baked from trs_core's real terrain
## heights so the world seen from orbit is the world you land on.
## Results are cached in user://cache keyed by the body's terrain data.
class_name PlanetBake
extends RefCounted

const W := 1024
const H := 512
const VERSION := 5

const PLANET_SHADER := """
shader_type spatial;
uniform sampler2D albedo_tex : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D normal_tex : filter_linear_mipmap, repeat_enable;
uniform float roughness_v = 0.8;
varying vec3 objp;
void vertex() { objp = VERTEX; }
void fragment() {
	vec3 s = normalize(vec3(objp.x, -objp.z, objp.y));   // Godot object -> sim body-fixed
	float lon = atan(s.y, s.x);
	float lat = asin(clamp(s.z, -1.0, 1.0));
	vec2 uv = vec2(lon / 6.2831853 + 0.5, 0.5 - lat / 3.14159265);
	// seam-free mip selection across the date line (Tarini's trick)
	vec2 uv2 = vec2(fract(uv.x + 0.5) - 0.5, uv.y);
	vec2 dx = dFdx(uv), dy = dFdy(uv), dx2 = dFdx(uv2), dy2 = dFdy(uv2);
	if (length(dx2) + length(dy2) < length(dx) + length(dy)) { dx = dx2; dy = dy2; }
	vec3 a = textureGrad(albedo_tex, uv, dx, dy).rgb;
	vec3 ns = textureGrad(normal_tex, uv, dx, dy).rgb * 2.0 - 1.0;
	vec3 no = vec3(ns.x, ns.z, -ns.y);                       // sim -> Godot object
	NORMAL = normalize((VIEW_MATRIX * MODEL_MATRIX * vec4(no, 0.0)).xyz);
	ALBEDO = a;
	ROUGHNESS = roughness_v;
	SPECULAR = 0.35;
}
"""

const CLOUD_SHADER := """
shader_type spatial;
render_mode blend_mix, depth_draw_never, cull_disabled, diffuse_lambert_wrap;
uniform float coverage = 0.6;
uniform float opacity = 1.0;
varying vec3 objp;
float hash(vec3 p) { return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453); }
float vn(vec3 p) {
	vec3 i = floor(p), f = fract(p);
	vec3 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(hash(i), hash(i + vec3(1,0,0)), u.x), mix(hash(i + vec3(0,1,0)), hash(i + vec3(1,1,0)), u.x), u.y),
		mix(mix(hash(i + vec3(0,0,1)), hash(i + vec3(1,0,1)), u.x), mix(hash(i + vec3(0,1,1)), hash(i + vec3(1,1,1)), u.x), u.y), u.z);
}
void vertex() {
	float a = TIME * 0.004;
	objp = mat3(vec3(cos(a), 0.0, -sin(a)), vec3(0.0, 1.0, 0.0), vec3(sin(a), 0.0, cos(a))) * VERTEX;
}
void fragment() {
	vec3 d = normalize(objp);
	// stretched along latitude: storm bands, then fbm for billows
	vec3 q = d * vec3(4.0, 9.0, 4.0);
	float n = 0.0, amp = 0.5;
	for (int o = 0; o < 6; o++) { n += vn(q) * amp; q *= 2.13; amp *= 0.5; }
	float band = 0.5 + 0.5 * sin(d.y * 11.0 + n * 3.0);
	float c = smoothstep(coverage, coverage + 0.18, n * 0.8 + band * 0.25);
	ALBEDO = vec3(0.97, 0.98, 1.0);
	ALPHA = c * 0.88 * opacity;
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
}
"""

static var _planet_shader: Shader
static var _cloud_shader: Shader


static func planet_material(core: TrsSim, body: int) -> ShaderMaterial:
	var tex := textures(core, body)
	if _planet_shader == null:
		_planet_shader = Shader.new()
		_planet_shader.code = PLANET_SHADER
	var m := ShaderMaterial.new()
	m.shader = _planet_shader
	m.set_shader_parameter("albedo_tex", tex.albedo)
	m.set_shader_parameter("normal_tex", tex.normal)
	return m


static func cloud_material() -> ShaderMaterial:
	if _cloud_shader == null:
		_cloud_shader = Shader.new()
		_cloud_shader.code = CLOUD_SHADER
	var m := ShaderMaterial.new()
	m.shader = _cloud_shader
	return m


static func textures(core: TrsSim, body: int) -> Dictionary:
	var info: Dictionary = JSON.parse_string(core.body_info(body))
	var sys: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(Sim.SYSTEM_PATH))
	var tdata := ""
	for b in sys.bodies:
		if b.id == info.id:
			tdata = JSON.stringify(b.get("terrain", {})) + str(b.get("launch_site", {}))
	var key := "%s_%d_%d" % [info.id, VERSION, hash(tdata) & 0x7fffffff]
	var dir := "user://cache/"
	DirAccess.make_dir_recursive_absolute(dir)
	var ap := dir + key + "_albedo.png"
	var np := dir + key + "_normal.png"
	var a_img: Image
	var n_img: Image
	if FileAccess.file_exists(ap) and FileAccess.file_exists(np):
		a_img = Image.load_from_file(ProjectSettings.globalize_path(ap))
		n_img = Image.load_from_file(ProjectSettings.globalize_path(np))
	else:
		var baked := _bake(core, body, info)
		a_img = baked[0]
		n_img = baked[1]
		a_img.save_png(ap)
		n_img.save_png(np)
	a_img.generate_mipmaps()
	n_img.generate_mipmaps()
	return {"albedo": ImageTexture.create_from_image(a_img), "normal": ImageTexture.create_from_image(n_img)}


static func _bake(core: TrsSim, body: int, info: Dictionary) -> Array:
	var h: PackedFloat64Array = core.terrain_map(body, W, H)
	var R: float = info.radius
	var a_img := Image.create(W, H, false, Image.FORMAT_RGB8)
	var n_img := Image.create(W, H, false, Image.FORMAT_RGB8)
	var ice_world: bool = info.id == "floe"
	var noise := FastNoiseLite.new()
	noise.seed = hash(info.id)
	noise.frequency = 0.02
	for y in H:
		var lat := PI / 2 - (y + 0.5) / H * PI
		var cl := cos(lat)
		var sl := sin(lat)
		var dn := R * PI / H                       # metres per pixel north-south
		var de := maxf(R * cl * TAU / W, dn * 0.05) # metres per pixel east-west
		for x in W:
			var lon := (x + 0.5) / W * TAU - PI
			var i := y * W + x
			var hc: float = h[i]
			var he: float = h[y * W + (x + 1) % W]
			var hw: float = h[y * W + (x - 1 + W) % W]
			var hn: float = h[maxi(y - 1, 0) * W + x]
			var hs: float = h[mini(y + 1, H - 1) * W + x]
			var gx := (he - hw) / (2.0 * de)
			var gy := (hn - hs) / (2.0 * dn)
			var slope := sqrt(gx * gx + gy * gy)
			var nz := noise.get_noise_2d(x * 1.0, y * 2.0) * 0.5 + 0.5
			var col: Color
			if ice_world:
				var hk := clampf((hc - 200.0) / 3500.0, 0.0, 1.0)
				col = Color(0.70, 0.80, 0.92).lerp(Color(0.97, 0.98, 1.0), clampf(hk * 2.2 + nz * 0.1, 0.0, 1.0))
				if hc < 450.0:  # basins: blue sea ice with pale pressure ridges
					var b := clampf((450.0 - hc) / 450.0, 0.0, 1.0)
					col = col.lerp(Color(0.36, 0.58, 0.84).lerp(Color(0.62, 0.78, 0.94), nz), b * 0.85)
				col = col.lerp(Color(0.3, 0.31, 0.35), clampf((slope - 0.12) * 5.0, 0.0, 0.9))
				if absf(lat) > 1.2:  # polar caps
					col = col.lerp(Color(1.0, 1.0, 1.0), clampf((absf(lat) - 1.2) * 4.0, 0.0, 1.0))
			else:
				var hk := clampf((hc + 1200.0) / 2600.0, 0.0, 1.0)
				col = Color(0.2, 0.195, 0.2).lerp(Color(0.66, 0.64, 0.6), pow(hk, 1.3))
				col = col * (0.95 + 0.1 * nz)
				col = col.lerp(Color(0.13, 0.125, 0.13), clampf((slope - 0.08) * 4.0, 0.0, 0.75))
				col.a = 1.0
			a_img.set_pixel(x, y, col)
			# normal in sim body-fixed axes, relief exaggerated for distance
			var k := 3.0 if ice_world else 7.0
			var ln := Vector3(-gx * k, -gy * k, 1.0).normalized()  # (east, north, up)
			var e := Vector3(-sin(lon), cos(lon), 0.0)
			var nv := Vector3(-sl * cos(lon), -sl * sin(lon), cl)
			var u := Vector3(cl * cos(lon), cl * sin(lon), sl)
			var n := (e * ln.x + nv * ln.y + u * ln.z).normalized()
			n_img.set_pixel(x, y, Color(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5))
	return [a_img, n_img]
