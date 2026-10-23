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
	var log := "ptrcall %s / varcall %s\n" % [out, out2]

	# World API through the binding: a 100 km circular orbit around Floe.
	var err: String = sim.load_system(ProjectSettings.globalize_path("res://data/bodies/austra_system.json"))
	ok = ok and err == ""
	var floe: int = sim.body_find("floe")
	var info: Dictionary = JSON.parse_string(sim.body_info(floe))
	var r: float = info.radius + 100000.0
	var v: float = sqrt(info.mu / r)
	var id: int = sim.vessel_add(floe, PackedFloat64Array([r, 0, 0, 0, v, 0]))
	sim.step_physics(50)
	sim.vessel_set_rails(id, true)
	var ev: Dictionary = JSON.parse_string(sim.warp(1000.0, id))
	var orb: Dictionary = JSON.parse_string(sim.vessel_orbit(id))
	var patches: Array = JSON.parse_string(sim.predict(id, 4, 1e6))
	ok = ok and ev.event == "none" and absf(orb.pe_alt - 100000.0) < 1e-3 and absf(sim.time() - 1001.0) < 1e-9
	ok = ok and patches.size() == 1 and patches[0].end == "none"
	log += "world: %s, %d bodies, warp %s, t=%.2f, Pe %.4f m, Ap %.4f m" % [
		sim.version(), sim.body_count(), ev.event, sim.time(), orb.pe_alt, orb.ap_alt]
	return {"status": "PASS" if ok else "FAIL", "log": log}
