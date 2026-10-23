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
## Not a roster gate: exercised by tools/run_gates.sh to prove both the
## Variant-call and ptrcall marshalling paths of the C binder work.
extends RefCounted


func run(_runner: Node) -> Dictionary:
	var sim := TrsSim.new()
	var out: PackedFloat64Array = sim.echo_f64(PackedFloat64Array([1.0, 2.5, 1e15]))
	var dyn: Variant = sim
	var out2: PackedFloat64Array = dyn.call("echo_f64", PackedFloat64Array([0.1, 0.2]))
	var ok := out.size() == 4 and out[3] == 1.0 + 2.5 + 1e15 and out2[2] == 0.1 + 0.2
	return {"status": "PASS" if ok else "FAIL", "log": "ptrcall %s / varcall %s" % [out, out2]}
