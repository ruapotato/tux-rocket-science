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
extends RefCounted


func run(_r: Node) -> Dictionary:
	var rng := RandomNumberGenerator.new()
	rng.seed = 42
	var bad := 0
	var bad_str := 0
	var samples := [-0.9999999999999999, -10.399999999999999, 0.1 + 0.2, 1e-300, 5e-324, 1.7976931348623157e308, -0.0, 123456789.123456789]
	for i in 200000:
		samples.append(rng.randf_range(-1e6, 1e6) * pow(10.0, rng.randi_range(-12, 12)))
	var ex := ""
	for x in samples:
		var s := String.num_scientific(x)
		if float(s) != x or s.to_float() != x:
			bad += 1
			if ex == "":
				ex = "%s -> %s" % [s, String.num_scientific(float(s))]
		if str(x).to_float() != x:
			bad_str += 1
	return {"status": "PASS" if bad == 0 else "FAIL", "log": "num_scientific mismatches %d/%d (%s); str() mismatches %d" % [bad, samples.size(), ex, bad_str]}
