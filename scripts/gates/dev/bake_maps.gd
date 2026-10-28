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
## Dev: every world's baked map (what it looks like from orbit), flat, with
## its landmarks ringed: map_<body>.png. Env BODIES (comma list).
extends RefCounted


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute("user://dev_shots/")
	var core := TrsSim.new()
	var log := PackedStringArray()
	var bodies := (OS.get_environment("BODIES") if OS.get_environment("BODIES") != "" else "floe,krill,kelp,pebble,narwhal,tusk,cinder,haven,wisp").split(",")
	var err: String = core.load_system(ProjectSettings.globalize_path(Sim.SYSTEM_PATH))
	if err != "":
		return {"status": "FAIL", "log": err}
	for bid in bodies:
		var bi: int = core.body_find(bid)
		var info: Dictionary = JSON.parse_string(core.body_info(bi))
		var imgs: Array = PlanetBake._bake(core, bi, info)
		var img: Image = imgs[0]
		img.convert(Image.FORMAT_RGB8)
		for lm in Sim.landmarks(bid):
			var x := int((float(lm.lon_deg) + 180.0) / 360.0 * img.get_width())
			var y := int((90.0 - float(lm.lat_deg)) / 180.0 * img.get_height())
			var r := int(float(lm.get("radius_km", float(lm.get("length_km", 50)) / 2.0)) * 1000.0 / (PI * float(info.radius)) * img.get_height()) + 4
			for a in 64:
				var px := x + int(cos(a * TAU / 64) * r)
				var py := y + int(sin(a * TAU / 64) * r)
				if px >= 0 and py >= 0 and px < img.get_width() and py < img.get_height():
					img.set_pixel(px, py, Color(1, 0.2, 0.2))
		img.save_png("user://dev_shots/map_%s.png" % bid)
		log.append("%s: %d landmarks" % [bid, Sim.landmarks(bid).size()])
	return {"status": "PASS", "log": "\n".join(log)}
